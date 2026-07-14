#include "kstream_slave.h"

#include <FreeRTOS.h>
#include <fpioa.h>
#include <gpio.h>
#include <gpiohs.h>
#include <hal.h>
#include <platform.h>
#include <semphr.h>
#include <spi.h>
#include <sysctl.h>
#include <task.h>
#include <uarths.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kstream_v2.h"

#define PIN_SPI_CS       0
#define PIN_SPI_CLK      1
#define PIN_SPI_MISO     2
#define PIN_SPI_MOSI     3
#define PIN_MASTER_PHASE 7
#define PIN_READY        15
#define GPIO_READY       3
#define GPIO_MASTER_INT  4

#define DOWNLINK_BYTES   (64u * 1024u)
#define UPLINK_BYTES     (64u * 1024u)
#define CONSOLE_RX_BYTES (8u * 1024u)
#define CONSOLE_TX_BYTES (8u * 1024u)
#define TX_FIFO_BYTES     KSTREAM_V2_BURST_BYTES

#define SSI_CTRLR0_TMOD_TX (1u << 8)
#define SSI_CTRLR0_SLV_OE  (1u << 10)
#define SSI_CTRLR0_DFS_32  (31u << 16)

typedef struct byte_ring {
    uint8_t *data;
    uint32_t size;
    volatile uint32_t read_count;
    volatile uint32_t write_count;
} byte_ring_t;

static volatile spi_t *const SSI = (volatile spi_t *)SPI_SLAVE_BASE_ADDR;
static volatile gpio_t *const GPIO_REG = (volatile gpio_t *)GPIO_BASE_ADDR;
static volatile gpiohs_t *const GPIOHS_REG =
    (volatile gpiohs_t *)GPIOHS_BASE_ADDR;
static uint8_t s_downlink_data[DOWNLINK_BYTES] __attribute__((aligned(64)));
static uint8_t s_uplink_data[UPLINK_BYTES] __attribute__((aligned(64)));
static uint8_t s_console_rx_data[CONSOLE_RX_BYTES] __attribute__((aligned(64)));
static uint8_t s_console_tx_data[CONSOLE_TX_BYTES] __attribute__((aligned(64)));
static byte_ring_t s_downlink = {s_downlink_data, DOWNLINK_BYTES, 0, 0};
static byte_ring_t s_uplink = {s_uplink_data, UPLINK_BYTES, 0, 0};
static byte_ring_t s_console_rx = {s_console_rx_data, CONSOLE_RX_BYTES, 0, 0};
static byte_ring_t s_console_tx = {s_console_tx_data, CONSOLE_TX_BYTES, 0, 0};
static kstream_v2_command_t s_command __attribute__((aligned(64)));
static kstream_v2_response_t s_response __attribute__((aligned(64)));
static uint8_t s_command_wire[KSTREAM_V2_FRAME_BYTES]
    __attribute__((aligned(64)));
static uint8_t s_rx_bounce[KSTREAM_V2_BURST_BYTES + 4u]
    __attribute__((aligned(64)));
static handle_t s_dma_rx;
static handle_t s_dma_tx;
static SemaphoreHandle_t s_dma_rx_done;
static SemaphoreHandle_t s_dma_tx_done;
static SemaphoreHandle_t s_downlink_ready;
static SemaphoreHandle_t s_uplink_space;
static StaticSemaphore_t s_console_tx_mutex_storage;
static SemaphoreHandle_t s_console_tx_mutex;
static uint32_t s_ready_level = 1u;
static bool s_int_active;
static bool s_activated;
static uint32_t s_expected_sequence;
static uint32_t s_commands;
static uint32_t s_faults;
static uint32_t s_bad_magic;
static uint32_t s_bad_crc;
static uint32_t s_calculated_crc;
static uint64_t s_downlink_bytes;
static uint64_t s_uplink_bytes;
static void wait_master_int(bool high)
{
    const uint32_t mask = 1u << GPIO_MASTER_INT;
    while (((GPIOHS_REG->input_val.u32[0] & mask) != 0u) != high)
        if (s_command.opcode == KSTREAM_V2_OP_PUSH &&
            s_command.stream == KSTREAM_V2_STREAM_CONSOLE_RX)
            vTaskDelay(1u);
}

static void master_int_mode(void)
{
    fpioa_set_function(PIN_MASTER_PHASE,
                       (fpioa_function_t)(FUNC_GPIOHS0 + GPIO_MASTER_INT));
    fpioa_set_io_pull(PIN_MASTER_PHASE, FPIOA_PULL_UP);
    GPIOHS_REG->output_en.u32[0] &= ~(1u << GPIO_MASTER_INT);
    GPIOHS_REG->input_en.u32[0] |= 1u << GPIO_MASTER_INT;
}

static void ready_set(bool high)
{
    if (!s_int_active)
        return;
    s_ready_level = high ? 1u : 0u;
    if (s_ready_level)
        GPIO_REG->data_output.u32[0] |= 1u << GPIO_READY;
    else
        GPIO_REG->data_output.u32[0] &= ~(1u << GPIO_READY);
}

static void dma_phase(uint32_t request, const volatile void *source,
                      volatile void *destination, bool source_increment,
                      bool destination_increment, size_t words, bool transmit)
{
    handle_t dma = transmit ? s_dma_tx : s_dma_rx;
    SemaphoreHandle_t dma_done = transmit ? s_dma_tx_done : s_dma_rx_done;
    (void)xSemaphoreTake(dma_done, 0u);
    const uint32_t *tx_words = NULL;
    if (transmit) {
        tx_words = (const uint32_t *)source;
        SSI->dr[0] = UINT32_MAX;
        size_t preload = words < 3u ? words : 3u;
        for (size_t i = 0u; i < preload; ++i)
            SSI->dr[0] = tx_words[i];
        tx_words += preload;
        words -= preload;
        if (words != 0u) {
            dma_set_request_source(dma, request);
            dma_transmit_async(dma, tx_words, destination, true, false,
                               4u, words, 4u, dma_done);
        }
    } else {
        dma_set_request_source(dma, request);
        dma_transmit_async(dma, source, destination, source_increment,
                           destination_increment, 4u, words, 4u, dma_done);
    }
    __sync_synchronize();
    if (transmit && s_commands == 1u)
        uarths_puts("KSTREAM:TX DMA_ARMED\r\n");
    if (transmit)
        wait_master_int(true);
    if (transmit && s_commands == 1u)
        uarths_puts("KSTREAM:TX MASTER_IDLE\r\n");
    ready_set(true);
    if (transmit && s_commands == 1u)
        uarths_puts("KSTREAM:TX RESPONSE_ARMED\r\n");
    if (words != 0u)
        (void)xSemaphoreTake(dma_done, portMAX_DELAY);
    wait_master_int(false);
    if (!transmit)
        s_int_active = true;
    ready_set(false);
    wait_master_int(true);
}

static uint32_t ring_used(const byte_ring_t *ring)
{
    return ring->write_count - ring->read_count;
}

static uint32_t ring_free(const byte_ring_t *ring)
{
    return ring->size - ring_used(ring);
}

static uint32_t ring_write_contiguous(const byte_ring_t *ring)
{
    uint32_t free_bytes = ring_free(ring);
    uint32_t to_end = ring->size - (ring->write_count & (ring->size - 1u));
    return free_bytes < to_end ? free_bytes : to_end;
}

static uint32_t ring_read_contiguous(const byte_ring_t *ring)
{
    uint32_t used = ring_used(ring);
    uint32_t to_end = ring->size - (ring->read_count & (ring->size - 1u));
    return used < to_end ? used : to_end;
}

static uint8_t *ring_write_pointer(byte_ring_t *ring)
{
    return ring->data + (ring->write_count & (ring->size - 1u));
}

static uint8_t *ring_read_pointer(byte_ring_t *ring)
{
    return ring->data + (ring->read_count & (ring->size - 1u));
}

static void ring_write_commit(byte_ring_t *ring, uint32_t length)
{
    __sync_synchronize();
    ring->write_count += length;
}

static void ring_read_commit(byte_ring_t *ring, uint32_t length)
{
    __sync_synchronize();
    ring->read_count += length;
}

static void spi_rx_fifo_drain(void)
{
    uint32_t pending = SSI->rxflr;
    if (pending > 32u)
        pending = 32u;
    while (pending-- != 0u)
        (void)SSI->dr[0];
}

static void spi_command_mode(void)
{
    SSI->ssienr = 0u;
    fpioa_set_function(PIN_SPI_MISO, FUNC_RESV0);
    fpioa_set_function(PIN_SPI_MOSI, FUNC_SPI_SLAVE_D0);
    spi_rx_fifo_drain();
    SSI->ctrlr0 = SSI_CTRLR0_SLV_OE | SSI_CTRLR0_DFS_32;
    SSI->dmardlr = 3u;
    SSI->dmacr = 1u;
    SSI->rxftlr = 0u;
    SSI->imr = 0u;
    SSI->ssienr = 1u;
}

static void spi_rx_dma_mode(void)
{
    SSI->ssienr = 0u;
    fpioa_set_function(PIN_SPI_MISO, FUNC_RESV0);
    fpioa_set_function(PIN_SPI_MOSI, FUNC_SPI_SLAVE_D0);
    spi_rx_fifo_drain();
    SSI->ctrlr0 = SSI_CTRLR0_SLV_OE | SSI_CTRLR0_DFS_32;
    SSI->dmardlr = 3u;
    SSI->dmacr = 1u;
    SSI->imr = 0u;
    SSI->ssienr = 1u;
}

static void spi_tx_mode(bool dma)
{
    SSI->ssienr = 0u;
    fpioa_set_function(PIN_SPI_MISO, FUNC_SPI_SLAVE_D0);
    SSI->ctrlr0 = SSI_CTRLR0_TMOD_TX | SSI_CTRLR0_DFS_32;
    SSI->dmatdlr = 4u;
    SSI->dmacr = dma ? 2u : 0u;
    SSI->imr = 0u;
    SSI->ssienr = 1u;
}

static void fill_status(kstream_v2_response_t *response, uint32_t sequence,
                        uint8_t result, const char *message)
{
    memset(response, 0, sizeof(*response));
    response->magic = KSTREAM_V2_MAGIC_RESPONSE;
    response->version = KSTREAM_V2_VERSION;
    response->result = result;
    response->sequence = sequence;
    response->downlink_free = ring_write_contiguous(&s_downlink) & ~3u;
    uint32_t uplink_used = ring_read_contiguous(&s_uplink);
    uint32_t console_tx_used = ring_read_contiguous(&s_console_tx);
    response->uplink_used =
        (uplink_used < TX_FIFO_BYTES ? uplink_used : TX_FIFO_BYTES) & ~3u;
    response->console_tx_used =
        console_tx_used < TX_FIFO_BYTES ? console_tx_used : TX_FIFO_BYTES;
    response->console_rx_free = ring_write_contiguous(&s_console_rx) & ~3u;
    response->faults = s_faults;
    if (message)
        strncpy((char *)response->message, message, sizeof(response->message));
    kstream_v2_response_finalize(response);
}

static void send_response(uint8_t result, const char *message)
{
    fill_status(&s_response, s_command.sequence, result, message);
    spi_tx_mode(true);
    dma_phase(SYSCTL_DMA_SELECT_SSI2_TX_REQ, &s_response, &SSI->dr[0],
              true, false, sizeof(s_response) / 4u, true);
    spi_command_mode();
}

static byte_ring_t *push_ring(uint8_t stream)
{
    if (stream == KSTREAM_V2_STREAM_DOWNLINK)
        return &s_downlink;
    if (stream == KSTREAM_V2_STREAM_CONSOLE_RX)
        return &s_console_rx;
    return NULL;
}

static byte_ring_t *pull_ring(uint8_t stream)
{
    if (stream == KSTREAM_V2_STREAM_UPLINK)
        return &s_uplink;
    if (stream == KSTREAM_V2_STREAM_CONSOLE_TX)
        return &s_console_tx;
    return NULL;
}

static void handle_push(void)
{
    byte_ring_t *ring = push_ring(s_command.stream);
    uint32_t wire = s_command.arg0;
    uint32_t expected_wire = (s_command.length + 3u) & ~3u;
    if (expected_wire < KSTREAM_V2_FRAME_BYTES)
        expected_wire = KSTREAM_V2_FRAME_BYTES;
    if (!ring || s_command.length == 0u ||
        wire != expected_wire ||
        wire > KSTREAM_V2_BURST_BYTES || wire > ring_write_contiguous(ring)) {
        ++s_faults;
        send_response(ring ? KSTREAM_V2_RESULT_NO_CREDIT
                           : KSTREAM_V2_RESULT_BAD_STREAM,
                      "push rejected");
        return;
    }

    uint8_t *ring_destination = ring_write_pointer(ring);
    spi_rx_dma_mode();
    dma_phase(SYSCTL_DMA_SELECT_SSI2_RX_REQ, &SSI->dr[0],
              s_rx_bounce, false, true, wire / 4u, false);
    memcpy(ring_destination, s_rx_bounce, s_command.length);
    ring_write_commit(ring, s_command.length);
    if (ring == &s_downlink) {
        s_downlink_bytes += s_command.length;
        (void)xSemaphoreGive(s_downlink_ready);
    }
    send_response(KSTREAM_V2_RESULT_OK, "push complete");
}

static void handle_pull(void)
{
    byte_ring_t *ring = pull_ring(s_command.stream);
    uint32_t wire = s_command.arg0;
    uint32_t expected_wire = s_command.length;
    if (s_command.stream == KSTREAM_V2_STREAM_CONSOLE_TX) {
        expected_wire = (s_command.length + 3u) & ~3u;
        if (expected_wire < KSTREAM_V2_FRAME_BYTES)
            expected_wire = KSTREAM_V2_FRAME_BYTES;
    }
    if (!ring || s_command.length == 0u || wire != expected_wire ||
        (wire & 3u) != 0u || wire > KSTREAM_V2_BURST_BYTES ||
        s_command.length > ring_read_contiguous(ring)) {
        ++s_faults;
        send_response(ring ? KSTREAM_V2_RESULT_NO_CREDIT
                           : KSTREAM_V2_RESULT_BAD_STREAM,
                      "pull rejected");
        return;
    }

    const void *source = ring_read_pointer(ring);
    if (ring == &s_console_tx && wire != s_command.length) {
        memcpy(s_rx_bounce, source, s_command.length);
        memset(s_rx_bounce + s_command.length, 0, wire - s_command.length);
        source = s_rx_bounce;
    }
    spi_tx_mode(true);
    dma_phase(SYSCTL_DMA_SELECT_SSI2_TX_REQ, source,
              &SSI->dr[0], true, false, wire / 4u, true);
    ring_read_commit(ring, s_command.length);
    if (ring == &s_uplink) {
        s_uplink_bytes += s_command.length;
        (void)xSemaphoreGive(s_uplink_space);
    }
    send_response(KSTREAM_V2_RESULT_OK, "pull complete");
}

static void transport_task(void *arg)
{
    (void)arg;
    for (;;) {
        spi_command_mode();
        dma_phase(SYSCTL_DMA_SELECT_SSI2_RX_REQ, &SSI->dr[0], s_command_wire,
                  false, true, sizeof(s_command_wire) / 4u, false);
        memcpy(&s_command, s_command_wire, sizeof(s_command));
        ++s_commands;
        if (!kstream_v2_command_valid(&s_command)) {
            s_bad_magic = s_command.magic;
            s_bad_crc = s_command.crc32;
            s_calculated_crc = kstream_v2_crc32(
                &s_command, offsetof(kstream_v2_command_t, crc32));
            char line[160];
            snprintf(line, sizeof(line),
                     "KSTREAM:BAD_CMD magic=%08lx seq=%lu crc=%08lx calc=%08lx op=%u\r\n",
                     (unsigned long)s_command.magic,
                     (unsigned long)s_command.sequence,
                     (unsigned long)s_command.crc32,
                     (unsigned long)kstream_v2_crc32(
                         &s_command, offsetof(kstream_v2_command_t, crc32)),
                     (unsigned)s_command.opcode);
            uarths_puts(line);
            ++s_faults;
            send_response(KSTREAM_V2_RESULT_BAD_CRC, "bad command crc");
            continue;
        }
        if (s_command.opcode == KSTREAM_V2_OP_ACTIVATE_INT &&
            s_command.flags == KSTREAM_V2_INT_MODE_LEVEL &&
            s_command.arg0 == KSTREAM_V2_INT_EVENT_DMA_READY &&
            s_command.arg1 == KSTREAM_V2_INT_BOOT_LEVEL_HIGH) {
            taskENTER_CRITICAL();
            s_downlink.read_count = s_downlink.write_count;
            s_uplink.read_count = s_uplink.write_count;
            s_console_rx.read_count = s_console_rx.write_count;
            taskEXIT_CRITICAL();
            s_expected_sequence = s_command.sequence;
            s_activated = true;
            send_response(KSTREAM_V2_RESULT_OK, "int active");
            continue;
        }
        if (s_command.sequence != s_expected_sequence + 1u) {
            ++s_faults;
            send_response(KSTREAM_V2_RESULT_BAD_SEQUENCE, "bad sequence");
            continue;
        }
        s_expected_sequence = s_command.sequence;
        if (!s_activated) {
            ++s_faults;
            /* There is intentionally no response edge before a valid
             * activation descriptor.  GPIO0 remains a stable boot HIGH. */
            continue;
        }
        switch (s_command.opcode) {
        case KSTREAM_V2_OP_HELLO:
            send_response(KSTREAM_V2_RESULT_OK, "k210-spi-slave-v2");
            break;
        case KSTREAM_V2_OP_STATUS:
        case KSTREAM_V2_OP_CONTROL:
            send_response(KSTREAM_V2_RESULT_OK, "ok");
            break;
        case KSTREAM_V2_OP_PUSH:
            handle_push();
            break;
        case KSTREAM_V2_OP_PULL:
            handle_pull();
            break;
        default:
            ++s_faults;
            send_response(KSTREAM_V2_RESULT_BAD_OPCODE, "bad opcode");
            break;
        }
        taskYIELD();
    }
}

bool kstream_slave_start(void)
{
    s_dma_rx = dma_open_free();
    s_dma_tx = dma_open_free();
    if (!s_dma_rx || !s_dma_tx)
        return false;
    s_dma_rx_done = xSemaphoreCreateBinary();
    s_dma_tx_done = xSemaphoreCreateBinary();
    s_downlink_ready = xSemaphoreCreateBinary();
    s_uplink_space = xSemaphoreCreateBinary();
    if (!s_dma_rx_done || !s_dma_tx_done || !s_downlink_ready ||
        !s_uplink_space)
        return false;
    s_console_tx_mutex = xSemaphoreCreateMutexStatic(&s_console_tx_mutex_storage);
    if (!s_console_tx_mutex)
        return false;

    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    fpioa_set_function(PIN_READY, FUNC_GPIO3);
    GPIO_REG->direction.u32[0] |= 1u << GPIO_READY;
    GPIO_REG->data_output.u32[0] |= 1u << GPIO_READY;
    fpioa_set_function(PIN_SPI_CS, FUNC_SPI_SLAVE_SS);
    fpioa_set_function(PIN_SPI_CLK, FUNC_SPI_SLAVE_SCLK);
    fpioa_set_function(PIN_SPI_MOSI, FUNC_SPI_SLAVE_D0);
    master_int_mode();
    sysctl_reset(SYSCTL_RESET_SPI2);
    sysctl_clock_enable(SYSCTL_CLOCK_SPI2);
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_SPI2, 0u);
    spi_command_mode();
    return xTaskCreate(transport_task, "kstream_slave", 3072, NULL, 4,
                       NULL) == pdPASS;
}

uint8_t *kstream_downlink_read_acquire(size_t *length)
{
    *length = ring_read_contiguous(&s_downlink);
    return ring_read_pointer(&s_downlink);
}

void kstream_downlink_read_commit(size_t length)
{
    ring_read_commit(&s_downlink, (uint32_t)length);
}

void kstream_downlink_wait(void)
{
    (void)xSemaphoreTake(s_downlink_ready, portMAX_DELAY);
}

uint8_t *kstream_uplink_write_acquire(size_t *length)
{
    *length = ring_write_contiguous(&s_uplink);
    return ring_write_pointer(&s_uplink);
}

void kstream_uplink_write_commit(size_t length)
{
    ring_write_commit(&s_uplink, (uint32_t)length);
}

void kstream_uplink_wait(void)
{
    (void)xSemaphoreTake(s_uplink_space, portMAX_DELAY);
}

size_t kstream_console_read(void *data, size_t length)
{
    uint32_t available = ring_read_contiguous(&s_console_rx);
    if (length > available)
        length = available;
    memcpy(data, ring_read_pointer(&s_console_rx), length);
    ring_read_commit(&s_console_rx, (uint32_t)length);
    return length;
}

size_t kstream_console_write(const void *data, size_t length)
{
    const uint8_t *source = (const uint8_t *)data;
    size_t total = 0u;
    (void)xSemaphoreTake(s_console_tx_mutex, portMAX_DELAY);
    while (length != 0u) {
        uint32_t space = ring_write_contiguous(&s_console_tx);
        if (space == 0u)
            break;
        size_t count = length < space ? length : space;
        memcpy(ring_write_pointer(&s_console_tx), source, count);
        ring_write_commit(&s_console_tx, (uint32_t)count);
        source += count;
        length -= count;
        total += count;
    }
    (void)xSemaphoreGive(s_console_tx_mutex);
    return total;
}

void kstream_slave_get_stats(kstream_slave_stats_t *stats)
{
    stats->downlink_used = ring_used(&s_downlink);
    stats->downlink_free = ring_free(&s_downlink);
    stats->uplink_used = ring_used(&s_uplink);
    stats->uplink_free = ring_free(&s_uplink);
    stats->console_rx_used = ring_used(&s_console_rx);
    stats->console_tx_used = ring_used(&s_console_tx);
    stats->commands = s_commands;
    stats->faults = s_faults;
    stats->bad_magic = s_bad_magic;
    stats->bad_crc = s_bad_crc;
    stats->calculated_crc = s_calculated_crc;
    stats->downlink_bytes = s_downlink_bytes;
    stats->uplink_bytes = s_uplink_bytes;
}
