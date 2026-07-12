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
#include <string.h>

#include "kstream_v2.h"

#define PIN_SPI_CS       0
#define PIN_SPI_CLK      1
#define PIN_SPI_MISO     2
#define PIN_SPI_MOSI     3
#define PIN_READY        15
#define GPIO_READY       3
#define GPIO_MASTER_INT  4

#define DOWNLINK_BYTES   (64u * 1024u)
#define UPLINK_BYTES     (64u * 1024u)
#define CONSOLE_RX_BYTES (8u * 1024u)
#define CONSOLE_TX_BYTES (8u * 1024u)

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
static uint8_t s_rx_bounce[KSTREAM_V2_BURST_BYTES] __attribute__((aligned(64)));
static handle_t s_dma;
static SemaphoreHandle_t s_dma_done;
static uint32_t s_ready_level = 1u;
static bool s_int_active;
static uint32_t s_expected_sequence;
static uint32_t s_commands;
static uint32_t s_faults;
static uint64_t s_downlink_bytes;
static uint64_t s_uplink_bytes;
static bool s_trace_tx;
static bool s_trace_tx_used;

static void trace_tx(const char *message)
{
    if (s_trace_tx)
        uarths_puts(message);
}

static void ready_signal(void)
{
    if (!s_int_active)
        return;
    s_ready_level ^= 1u;
    if (s_ready_level)
        GPIO_REG->data_output.u32[0] |= 1u << GPIO_READY;
    else
        GPIO_REG->data_output.u32[0] &= ~(1u << GPIO_READY);
}

static void dma_phase(uint32_t request, const volatile void *source,
                      volatile void *destination, bool source_increment,
                      bool destination_increment, size_t words, bool transmit)
{
    (void)xSemaphoreTake(s_dma_done, 0u);
    if (transmit) {
        const uint32_t *tx_words = (const uint32_t *)source;
        size_t preload = words < 4u ? words : 4u;
        for (size_t i = 0u; i < preload; ++i)
            SSI->dr[0] = tx_words[i];
        tx_words += preload;
        words -= preload;
        if (words != 0u) {
            dma_set_request_source(s_dma, request);
            dma_transmit_async(s_dma, tx_words, destination, true, false,
                               4u, words, 4u, s_dma_done);
        }
        trace_tx("KSTREAM:TRACE TX_ARMED\r\n");
    } else {
        dma_set_request_source(s_dma, request);
        dma_transmit_async(s_dma, source, destination, source_increment,
                           destination_increment, 4u, words, 4u, s_dma_done);
    }
    ready_signal();
    if (!transmit || words != 0u)
        (void)xSemaphoreTake(s_dma_done, portMAX_DELAY);
    if (transmit) {
        trace_tx("KSTREAM:TRACE DMA_DONE\r\n");
        /* DMA done only means that the TX FIFO owns the bytes.  ESP finishes
         * the read with a LOW->HIGH token on the otherwise idle MOSI wire.
         * This edge says the GPIO sampler is armed, so the short token cannot
         * race the DMA completion wake-up. */
        ready_signal();
        trace_tx("KSTREAM:TRACE ACK_ARMED\r\n");
        while ((GPIOHS_REG->input_val.u32[0] &
                (1u << GPIO_MASTER_INT)) != 0u)
            ;
        trace_tx("KSTREAM:TRACE ACK_LOW\r\n");
        while ((GPIOHS_REG->input_val.u32[0] &
                (1u << GPIO_MASTER_INT)) == 0u)
            ;
        trace_tx("KSTREAM:TRACE ACK_HIGH\r\n");
    }
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

static void spi_command_mode(void)
{
    SSI->ssienr = 0u;
    fpioa_set_function(PIN_SPI_MISO, FUNC_RESV0);
    fpioa_set_function(PIN_SPI_MOSI, FUNC_SPI_SLAVE_D0);
    while (SSI->rxflr != 0u)
        (void)SSI->dr[0];
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
    SSI->ctrlr0 = SSI_CTRLR0_SLV_OE | SSI_CTRLR0_DFS_32;
    SSI->dmardlr = 3u;
    SSI->dmacr = 1u;
    SSI->imr = 0u;
    SSI->ssienr = 1u;
}

static void spi_tx_mode(bool dma)
{
    SSI->ssienr = 0u;
    fpioa_set_function(PIN_SPI_MOSI,
                       (fpioa_function_t)(FUNC_GPIOHS0 + GPIO_MASTER_INT));
    fpioa_set_io_pull(PIN_SPI_MOSI, FPIOA_PULL_DOWN);
    GPIOHS_REG->output_en.u32[0] &= ~(1u << GPIO_MASTER_INT);
    GPIOHS_REG->input_en.u32[0] |= 1u << GPIO_MASTER_INT;
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
    response->uplink_used = ring_read_contiguous(&s_uplink) & ~3u;
    response->console_tx_used = ring_read_contiguous(&s_console_tx) & ~3u;
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
    bool direct = wire == s_command.length &&
                  (((uintptr_t)ring_destination & 3u) == 0u);
    uint8_t *dma_destination = direct ? ring_destination : s_rx_bounce;
    spi_rx_dma_mode();
    dma_phase(SYSCTL_DMA_SELECT_SSI2_RX_REQ, &SSI->dr[0],
              dma_destination, false, true, wire / 4u, false);
    if (!direct)
        memcpy(ring_destination, s_rx_bounce, s_command.length);
    ring_write_commit(ring, s_command.length);
    if (ring == &s_downlink)
        s_downlink_bytes += s_command.length;
    send_response(KSTREAM_V2_RESULT_OK, "push complete");
}

static void handle_pull(void)
{
    byte_ring_t *ring = pull_ring(s_command.stream);
    uint32_t wire = s_command.arg0;
    if (!ring || s_command.length == 0u || wire != s_command.length ||
        (wire & 3u) != 0u || wire > KSTREAM_V2_BURST_BYTES ||
        wire > ring_read_contiguous(ring)) {
        ++s_faults;
        send_response(ring ? KSTREAM_V2_RESULT_NO_CREDIT
                           : KSTREAM_V2_RESULT_BAD_STREAM,
                      "pull rejected");
        return;
    }

    spi_tx_mode(true);
    if (!s_trace_tx_used && s_commands > 200u) {
        s_trace_tx = true;
        s_trace_tx_used = true;
    }
    dma_phase(SYSCTL_DMA_SELECT_SSI2_TX_REQ, ring_read_pointer(ring),
              &SSI->dr[0], true, false, wire / 4u, true);
    s_trace_tx = false;
    ring_read_commit(ring, wire);
    if (ring == &s_uplink)
        s_uplink_bytes += wire;
    send_response(KSTREAM_V2_RESULT_OK, "pull complete");
}

static void transport_task(void *arg)
{
    (void)arg;
    for (;;) {
        spi_command_mode();
        dma_phase(SYSCTL_DMA_SELECT_SSI2_RX_REQ, &SSI->dr[0], &s_command,
                  false, true, sizeof(s_command) / 4u, false);
        ++s_commands;
        if (!kstream_v2_command_valid(&s_command)) {
            ++s_faults;
            send_response(KSTREAM_V2_RESULT_BAD_CRC, "bad command crc");
            continue;
        }
        if (s_command.sequence != s_expected_sequence + 1u) {
            ++s_faults;
            send_response(KSTREAM_V2_RESULT_BAD_SEQUENCE, "bad sequence");
            continue;
        }
        s_expected_sequence = s_command.sequence;
        if (!s_int_active) {
            if (s_command.opcode != KSTREAM_V2_OP_ACTIVATE_INT ||
                s_command.flags != KSTREAM_V2_INT_MODE_TOGGLE ||
                s_command.arg0 != KSTREAM_V2_INT_EVENT_PHASE_ARMED ||
                s_command.arg1 != KSTREAM_V2_INT_BOOT_LEVEL_HIGH) {
                ++s_faults;
                /* There is intentionally no response edge before a valid
                 * activation descriptor.  GPIO0 remains a stable boot HIGH. */
                continue;
            }
            s_int_active = true;
            send_response(KSTREAM_V2_RESULT_OK, "int active");
            continue;
        }
        switch (s_command.opcode) {
        case KSTREAM_V2_OP_ACTIVATE_INT:
            ++s_faults;
            send_response(KSTREAM_V2_RESULT_BUSY, "int already active");
            break;
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
    }
}

bool kstream_slave_start(void)
{
    s_dma = dma_open_free();
    if (!s_dma)
        return false;
    s_dma_done = xSemaphoreCreateBinary();
    if (!s_dma_done)
        return false;

    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    fpioa_set_function(PIN_READY, FUNC_GPIO3);
    GPIO_REG->direction.u32[0] |= 1u << GPIO_READY;
    GPIO_REG->data_output.u32[0] |= 1u << GPIO_READY;
    fpioa_set_function(PIN_SPI_CS, FUNC_SPI_SLAVE_SS);
    fpioa_set_function(PIN_SPI_CLK, FUNC_SPI_SLAVE_SCLK);
    fpioa_set_function(PIN_SPI_MOSI, FUNC_SPI_SLAVE_D0);
    sysctl_reset(SYSCTL_RESET_SPI2);
    sysctl_clock_enable(SYSCTL_CLOCK_SPI2);
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_SPI2, 0u);
    spi_command_mode();
    return xTaskCreate(transport_task, "kstream_slave", 3072, NULL, 9,
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

uint8_t *kstream_uplink_write_acquire(size_t *length)
{
    *length = ring_write_contiguous(&s_uplink);
    return ring_write_pointer(&s_uplink);
}

void kstream_uplink_write_commit(size_t length)
{
    ring_write_commit(&s_uplink, (uint32_t)length);
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
    stats->downlink_bytes = s_downlink_bytes;
    stats->uplink_bytes = s_uplink_bytes;
}
