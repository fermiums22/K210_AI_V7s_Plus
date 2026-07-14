#include "kstream_slave.h"

#include <FreeRTOS.h>
#include <devices.h>
#include <fpioa.h>
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

#include "klink_v1.h"
#include "klink_v1_link.h"
#include "kspi_v2.h"

#define PIN_SPI_CS    0
#define PIN_SPI_CLK   1
#define PIN_SPI_MISO  2
#define PIN_SPI_MOSI  3
#define PIN_READY     15
#define PIN_ACK        7
#define GPIOHS_READY  0
#define GPIOHS_ACK    1

#define WIRE_WRITE_STATUS_OPCODE 1u
#define WIRE_WRITE_OPCODE 2u
#define WIRE_READ_OPCODE  3u
#define WIRE_READ_STATUS_OPCODE  4u
#define WIRE_PREFIX_BYTES 2u
#define WIRE_CELL_BYTES   (WIRE_PREFIX_BYTES + KLINK_V1_CELL_BYTES)
#define ACTIVATION_TEXT   "KMASTER1"
#define PHASE_DEADLINE_TICKS pdMS_TO_TICKS(100u)

#define DOWNLINK_BYTES   (64u * 1024u)
#define UPLINK_BYTES     (64u * 1024u)
#define CONSOLE_RX_BYTES (8u * 1024u)
#define CONSOLE_TX_BYTES (8u * 1024u)
#define UPDATE_RX_BYTES  (64u * 1024u)
#define UPDATE_TX_BYTES  (1024u)

typedef struct byte_ring {
    uint8_t *data;
    uint32_t size;
    volatile uint32_t read_count;
    volatile uint32_t write_count;
} byte_ring_t;

static uint8_t s_downlink_data[DOWNLINK_BYTES] __attribute__((aligned(64)));
static uint8_t s_uplink_data[UPLINK_BYTES] __attribute__((aligned(64)));
static uint8_t s_console_rx_data[CONSOLE_RX_BYTES] __attribute__((aligned(64)));
static uint8_t s_console_tx_data[CONSOLE_TX_BYTES] __attribute__((aligned(64)));
static uint8_t s_update_rx_data[UPDATE_RX_BYTES] __attribute__((aligned(64)));
static uint8_t s_update_tx_data[UPDATE_TX_BYTES] __attribute__((aligned(64)));
static byte_ring_t s_downlink = {s_downlink_data, DOWNLINK_BYTES, 0u, 0u};
static byte_ring_t s_uplink = {s_uplink_data, UPLINK_BYTES, 0u, 0u};
static byte_ring_t s_console_rx = {s_console_rx_data, CONSOLE_RX_BYTES, 0u, 0u};
static byte_ring_t s_console_tx = {s_console_tx_data, CONSOLE_TX_BYTES, 0u, 0u};
static byte_ring_t s_update_rx = {s_update_rx_data, UPDATE_RX_BYTES, 0u, 0u};
static byte_ring_t s_update_tx = {s_update_tx_data, UPDATE_TX_BYTES, 0u, 0u};
static klink_v1_endpoint_t s_link;
static klink_v1_cell_t s_tx __attribute__((aligned(64)));
static klink_v1_cell_t s_rx __attribute__((aligned(64)));
static uint8_t s_wire[WIRE_CELL_BYTES] __attribute__((aligned(64)));
static handle_t s_spi;
static handle_t s_spi_device;
static handle_t s_ready_gpio;
static TaskHandle_t s_transport_task;
static SemaphoreHandle_t s_start_event;
static SemaphoreHandle_t s_downlink_ready;
static SemaphoreHandle_t s_uplink_space;
static SemaphoreHandle_t s_update_ready;
static SemaphoreHandle_t s_update_space;
static SemaphoreHandle_t s_link_ready;
static SemaphoreHandle_t s_rearm_armed;
static SemaphoreHandle_t s_rearm_active;
static StaticSemaphore_t s_console_tx_mutex_storage;
static SemaphoreHandle_t s_console_tx_mutex;
static volatile uint32_t s_stage;
static volatile bool s_rearm_waiting;
static uint32_t s_exchanges;
static uint32_t s_route_faults;
static uint32_t s_last_descriptor;
static uint32_t s_last_result;
static uint8_t s_token;
static uint64_t s_downlink_bytes;
static uint64_t s_uplink_bytes;
static uint64_t s_console_rx_bytes;
static uint64_t s_console_tx_bytes;
static uint64_t s_update_rx_bytes;
static uint64_t s_update_tx_bytes;

static uint32_t ring_used(const byte_ring_t *ring)
{
    return ring->write_count - ring->read_count;
}

static uint32_t ring_free(const byte_ring_t *ring)
{
    return ring->size - ring_used(ring);
}

static uint32_t ring_read_contiguous(const byte_ring_t *ring)
{
    uint32_t used = ring_used(ring);
    uint32_t offset = ring->read_count % ring->size;
    uint32_t tail = ring->size - offset;
    return used < tail ? used : tail;
}

static uint32_t ring_write_contiguous(const byte_ring_t *ring)
{
    uint32_t free = ring_free(ring);
    uint32_t offset = ring->write_count % ring->size;
    uint32_t tail = ring->size - offset;
    return free < tail ? free : tail;
}

static uint8_t *ring_read_pointer(byte_ring_t *ring)
{
    return ring->data + ring->read_count % ring->size;
}

static uint8_t *ring_write_pointer(byte_ring_t *ring)
{
    return ring->data + ring->write_count % ring->size;
}

static void ring_read_commit(byte_ring_t *ring, uint32_t count)
{
    __sync_synchronize();
    ring->read_count += count;
}

static void ring_write_commit(byte_ring_t *ring, uint32_t count)
{
    __sync_synchronize();
    ring->write_count += count;
}

static uint32_t ready_level(void)
{
    return gpio_get_pin_value(s_ready_gpio, GPIOHS_READY) == GPIO_PV_HIGH;
}

static uint32_t ack_level(void)
{
    return gpio_get_pin_value(s_ready_gpio, GPIOHS_ACK) == GPIO_PV_HIGH;
}

static void ack_set(uint32_t level)
{
    gpio_set_pin_value(s_ready_gpio, GPIOHS_ACK,
                       level ? GPIO_PV_HIGH : GPIO_PV_LOW);
}

static void ready_changed(uint32_t pin, void *userdata)
{
    (void)pin;
    (void)userdata;
    BaseType_t task_woken = pdFALSE;
    if (s_transport_task)
        vTaskNotifyGiveFromISR(s_transport_task, &task_woken);
    if (task_woken == pdTRUE)
        portYIELD();
}

static bool wait_ready(uint32_t level)
{
    TickType_t started = xTaskGetTickCount();
    while (ready_level() != level) {
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= PHASE_DEADLINE_TICKS)
            return false;
        (void)ulTaskNotifyTake(pdTRUE, PHASE_DEADLINE_TICKS - elapsed);
    }
    return true;
}

static bool spi_master_init(void)
{
    fpioa_set_function(PIN_SPI_CS, FUNC_SPI1_SS0);
    fpioa_set_function(PIN_SPI_CLK, FUNC_SPI1_SCLK);
    fpioa_set_function(PIN_SPI_MOSI, FUNC_SPI1_D0);
    fpioa_set_function(PIN_SPI_MISO, FUNC_SPI1_D1);
    s_spi = io_open("/dev/spi1");
    if (!s_spi)
        return false;
    s_spi_device = spi_get_device(s_spi, SPI_MODE_0, SPI_FF_STANDARD, 1u, 8u);
    if (!s_spi_device)
        return false;
    double actual = spi_dev_set_clock_rate(s_spi_device, 20000000.0);
    char line[96];
    snprintf(line, sizeof(line), "KLINK:MASTER_SPI clock=%lu\r\n",
             (unsigned long)actual);
    uarths_puts(line);
    return true;
}

static bool activation_send(void)
{
    klink_v1_cell_clear(&s_tx, KLINK_CH_CONTROL, KLINK_T_OPEN);
    if (!klink_v1_cell_set_payload(&s_tx, ACTIVATION_TEXT,
                                   sizeof(ACTIVATION_TEXT) - 1u))
        return false;
    klink_v1_cell_finalize(&s_tx);
    s_wire[0] = WIRE_WRITE_OPCODE;
    s_wire[1] = 0u;
    memcpy(s_wire + WIRE_PREFIX_BYTES, &s_tx, sizeof(s_tx));
    return io_write(s_spi_device, s_wire, sizeof(s_wire)) ==
           (int)sizeof(s_wire);
}

static bool master_read_cell(void)
{
    const uint8_t prefix[WIRE_PREFIX_BYTES] = {
        WIRE_READ_OPCODE, KSPI_V2_REGION_KLINK};
    return spi_dev_transfer_sequential(s_spi_device, prefix, sizeof(prefix),
                                       (uint8_t *)&s_rx, sizeof(s_rx)) ==
           (int)sizeof(s_rx);
}

static bool master_write_cell(void)
{
    s_wire[0] = WIRE_WRITE_OPCODE;
    s_wire[1] = KSPI_V2_REGION_KLINK;
    memcpy(s_wire + WIRE_PREFIX_BYTES, &s_tx, sizeof(s_tx));
    return io_write(s_spi_device, s_wire, sizeof(s_wire)) ==
           (int)sizeof(s_wire);
}

static bool master_write_descriptor(uint32_t descriptor)
{
    s_wire[0] = WIRE_WRITE_STATUS_OPCODE;
    memcpy(s_wire + 1u, &descriptor, sizeof(descriptor));
    return io_write(s_spi_device, s_wire, 1u + sizeof(descriptor)) ==
           (int)(1u + sizeof(descriptor));
}

static bool master_read_result(uint32_t *result)
{
    const uint8_t command = WIRE_READ_STATUS_OPCODE;
    return spi_dev_transfer_sequential(s_spi_device, &command,
                                       sizeof(command), (uint8_t *)result,
                                       sizeof(*result)) ==
           (int)sizeof(*result);
}

static bool master_start_contract(void)
{
    if (!wait_ready(1u))
        return false;
    const uint8_t contract_token = 1u;
    uint32_t descriptor = kspi_v2_descriptor(
        KSPI_V2_OPERATION_EXCHANGE, KSPI_V2_REGION_KLINK,
        KSPI_V2_CELL_BYTES, contract_token);
    s_last_descriptor = descriptor;
    if (!master_write_descriptor(descriptor))
        return false;
    ack_set(0u);
    if (!wait_ready(0u))
        return false;
    uint32_t result = 0u;
    bool result_read = master_read_result(&result);
    s_last_result = result;
    ack_set(1u);
    if (!wait_ready(1u) || !result_read ||
        !kspi_v2_result_valid(result, contract_token))
        return false;
    s_token = 0u;
    return true;
}

static bool ring_to_cell(byte_ring_t *ring, uint8_t channel, uint16_t flags,
                         uint64_t *counter)
{
    if ((s_link.tx_queued_mask & (1u << channel)) != 0u)
        return false;
    uint32_t count = ring_read_contiguous(ring);
    if (count > KLINK_V1_PAYLOAD_BYTES)
        count = KLINK_V1_PAYLOAD_BYTES;
    if (count == 0u)
        return false;
    if (!klink_v1_queue(&s_link, channel, KLINK_T_DATA, flags,
                        ring_read_pointer(ring), count)) {
        ++s_route_faults;
        return false;
    }
    ring_read_commit(ring, count);
    *counter += count;
    if (ring == &s_uplink)
        (void)xSemaphoreGive(s_uplink_space);
    else if (ring == &s_update_tx)
        (void)xSemaphoreGive(s_update_space);
    return true;
}

static bool prepare_tx(void)
{
    if (!s_link.inflight && s_link.tx_queued_mask == 0u) {
        if (!ring_to_cell(&s_update_tx, KLINK_CH_BULK,
                          KLINK_F_RELIABLE, &s_update_tx_bytes) &&
            !ring_to_cell(&s_console_tx, KLINK_CH_DIAG,
                          KLINK_F_RELIABLE, &s_console_tx_bytes))
            (void)ring_to_cell(&s_uplink, KLINK_CH_AUDIO_IN, 0u,
                               &s_uplink_bytes);
    }
    klink_v1_build_tx(&s_link, &s_tx);
    return KLINK_TYPE(s_tx.channel_type) != KLINK_T_IDLE;
}

static void stamp_stream_command(uint8_t token)
{
    s_tx.flags = kspi_v2_cell_result_flags(s_tx.flags,
                                           KSPI_V2_ERROR_NONE, token);
    klink_v1_cell_finalize(&s_tx);
}

static bool cell_to_ring(byte_ring_t *ring, const klink_v1_cell_t *cell,
                         uint64_t *counter, SemaphoreHandle_t ready)
{
    uint32_t count = cell->payload_length;
    if (ring_free(ring) < count) {
        ++s_route_faults;
        return false;
    }
    uint32_t first = ring_write_contiguous(ring);
    if (first > count)
        first = count;
    memcpy(ring_write_pointer(ring), cell->payload, first);
    ring_write_commit(ring, first);
    if (first != count) {
        memcpy(ring_write_pointer(ring), cell->payload + first, count - first);
        ring_write_commit(ring, count - first);
    }
    *counter += count;
    if (ready)
        (void)xSemaphoreGive(ready);
    return true;
}

static bool process_rx(bool *activity)
{
    klink_v1_event_t event;
    uint32_t flags = klink_v1_process_rx(&s_link, &s_rx, &event);
    *activity = (flags & KLINK_EVENT_RX) != 0u;
    if ((flags & KLINK_EVENT_FAULT) != 0u)
        return false;
    if ((flags & KLINK_EVENT_RX) == 0u)
        return true;

    bool accepted = false;
    if (event.rx_channel == KLINK_CH_AUDIO_OUT)
        accepted = cell_to_ring(&s_downlink, &s_rx, &s_downlink_bytes,
                                s_downlink_ready);
    else if (event.rx_channel == KLINK_CH_DIAG)
        accepted = cell_to_ring(&s_console_rx, &s_rx, &s_console_rx_bytes,
                                NULL);
    else if (event.rx_channel == KLINK_CH_BULK)
        accepted = cell_to_ring(&s_update_rx, &s_rx, &s_update_rx_bytes,
                                s_update_ready);
    else {
        ++s_route_faults;
        return false;
    }
    if (accepted && (s_rx.flags & KLINK_F_RELIABLE) != 0u)
        (void)klink_v1_release_credit(&s_link, event.rx_channel, 1u);
    return accepted;
}

static bool rearm_after_esp_restart(void)
{
    s_stage = 0x20u;
    s_rearm_waiting = true;
    __sync_synchronize();
    uarths_puts("KLINK:SUSPENDED reason=esp-ota waiting=KLINK_ARMED\r\n");
    (void)xSemaphoreTake(s_rearm_armed, portMAX_DELAY);
    if (!activation_send())
        return false;
    uarths_puts("KLINK:REARM activation-sent waiting=KLINK_ACTIVE\r\n");
    (void)xSemaphoreTake(s_rearm_active, portMAX_DELAY);
    klink_v1_endpoint_init(&s_link, 31u);
    if (!master_start_contract())
        return false;
    s_rearm_waiting = false;
    __sync_synchronize();
    uarths_puts("KLINK:RESUMED role=master register-protocol=2\r\n");
    return true;
}

static void transport_task(void *arg)
{
    (void)arg;
    s_transport_task = xTaskGetCurrentTaskHandle();
    (void)xSemaphoreTake(s_start_event, portMAX_DELAY);
    klink_v1_endpoint_init(&s_link, 31u);
    s_stage = 1u;
    if (!master_start_contract())
        goto transport_fatal;
    bool tx_activity = prepare_tx();
    stamp_stream_command(1u);
    bool announced = false;
    for (;;) {
        if (!wait_ready(1u))
            break;
        s_stage = 3u;
        if (!master_read_cell())
            break;
        ack_set(0u);
        if (!wait_ready(0u))
            break;

        /* The cell just read carries the result of the preceding exchange.
         * Validate its CRC before trusting the inline token/result. */
        if (!klink_v1_cell_validate(&s_rx))
            break;
        uint8_t result_class = kspi_v2_cell_result(s_rx.flags);
        uint8_t result_token = kspi_v2_cell_token(s_rx.flags);
        uint8_t result_error = result_class == KSPI_V2_CELL_RESULT_QUIESCE
                                   ? KSPI_V2_ERROR_QUIESCE
                                   : (result_class == KSPI_V2_CELL_RESULT_FATAL
                                          ? KSPI_V2_ERROR_CELL
                                          : KSPI_V2_ERROR_NONE);
        s_last_result = kspi_v2_result(result_error,
                                       KSPI_V2_PHASE_COMPLETE, result_token);
        if (result_token != s_token)
            break;
        if (result_class == KSPI_V2_CELL_RESULT_QUIESCE) {
            if (!rearm_after_esp_restart())
                break;
            tx_activity = prepare_tx();
            stamp_stream_command(1u);
            continue;
        }
        if (result_class != KSPI_V2_CELL_RESULT_OK)
            break;

        s_stage = 4u;
        if (!master_write_cell())
            break;
        ack_set(1u);

        /* Both endpoints now process the cells they just received in
         * parallel.  The already prepared transmit cell made the WRITE phase
         * independent of this work. */
        bool rx_activity = false;
        if (!process_rx(&rx_activity))
            break;
        ++s_token;
        tx_activity = prepare_tx();
        stamp_stream_command((uint8_t)(s_token + 1u));

        s_stage = 5u;
        if (!wait_ready(1u))
            break;

        ++s_exchanges;
        s_stage = 6u;
        if (!announced) {
            announced = true;
            (void)xSemaphoreGive(s_link_ready);
            uarths_puts(
                "KLINK:LINK_UP role=master cell=64 register-protocol=2 ack=io7\r\n");
        }
        /* READY remains the transaction gate, while an all-IDLE exchange is
         * the explicit bus-idle state.  Block one scheduler tick only in that
         * state so Wi-Fi, console, and producers can run; payload and reliable
         * traffic stay back-to-back at full DMA rate. */
        if (!rx_activity && !tx_activity)
            vTaskDelay(1u);
    }
transport_fatal:
    ++s_route_faults;
    s_stage = 0x80u;
    char fatal[144];
    snprintf(fatal, sizeof(fatal),
             "KLINK:FATAL transport halted token=%u desc=%08lx result=%08lx ready=%lu ack=%lu\r\n",
             (unsigned)s_token, (unsigned long)s_last_descriptor,
             (unsigned long)s_last_result, (unsigned long)ready_level(),
             (unsigned long)ack_level());
    uarths_puts(fatal);
    vTaskDelete(NULL);
}

bool kstream_slave_start(void)
{
    s_start_event = xSemaphoreCreateBinary();
    s_downlink_ready = xSemaphoreCreateBinary();
    s_uplink_space = xSemaphoreCreateBinary();
    s_update_ready = xSemaphoreCreateBinary();
    s_update_space = xSemaphoreCreateBinary();
    s_link_ready = xSemaphoreCreateBinary();
    s_rearm_armed = xSemaphoreCreateBinary();
    s_rearm_active = xSemaphoreCreateBinary();
    s_console_tx_mutex = xSemaphoreCreateMutexStatic(&s_console_tx_mutex_storage);
    s_ready_gpio = io_open("/dev/gpio0");
    if (!s_start_event || !s_downlink_ready || !s_uplink_space ||
        !s_update_ready || !s_update_space || !s_link_ready ||
        !s_rearm_armed || !s_rearm_active ||
        !s_console_tx_mutex || !s_ready_gpio || !spi_master_init())
        return false;
    if (!activation_send())
        return false;
    uarths_puts("KLINK:ACTIVATION_SENT ready-owner=esp\r\n");
    return xTaskCreate(transport_task, "klink_master", 3072u, NULL, 4u,
                       NULL) == pdPASS;
}

void kstream_slave_esp_armed(void)
{
    if (s_rearm_waiting && s_rearm_armed)
        (void)xSemaphoreGive(s_rearm_armed);
}

void kstream_slave_esp_active(void)
{
    if (s_rearm_waiting && s_rearm_active)
        (void)xSemaphoreGive(s_rearm_active);
}

void kstream_slave_release_ready(void)
{
    /* Preserve the UART TX idle HIGH level while IO7 becomes the application
     * phase ACK.  Recovery is a separate image and maps IO7 back to UART2_TX. */
    gpio_set_drive_mode(s_ready_gpio, GPIOHS_ACK, GPIO_DM_OUTPUT);
    gpio_set_pin_value(s_ready_gpio, GPIOHS_ACK, GPIO_PV_HIGH);
    fpioa_set_function(PIN_ACK, FUNC_GPIOHS0 + GPIOHS_ACK);

    /* Remap the strap-only slow GPIO to an interrupt-capable GPIOHS input
     * only after ESP has taken over the line at the same HIGH level. */
    fpioa_set_function(PIN_READY, FUNC_GPIOHS0 + GPIOHS_READY);
    gpio_set_drive_mode(s_ready_gpio, GPIOHS_READY, GPIO_DM_INPUT_PULL_UP);
    gpio_set_on_changed(s_ready_gpio, GPIOHS_READY, ready_changed, NULL);
    gpio_set_pin_edge(s_ready_gpio, GPIOHS_READY, GPIO_PE_BOTH);
    __sync_synchronize();
    (void)xSemaphoreGive(s_start_event);
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

size_t kstream_update_read(void *data, size_t length)
{
    while (ring_used(&s_update_rx) == 0u)
        (void)xSemaphoreTake(s_update_ready, portMAX_DELAY);
    uint32_t available = ring_read_contiguous(&s_update_rx);
    if (length > available)
        length = available;
    memcpy(data, ring_read_pointer(&s_update_rx), length);
    ring_read_commit(&s_update_rx, (uint32_t)length);
    return length;
}

size_t kstream_update_write(const void *data, size_t length)
{
    const uint8_t *source = (const uint8_t *)data;
    if (length > s_update_tx.size)
        return 0u;
    while (ring_free(&s_update_tx) < length)
        (void)xSemaphoreTake(s_update_space, portMAX_DELAY);
    size_t requested = length;
    while (length != 0u) {
        uint32_t space = ring_write_contiguous(&s_update_tx);
        size_t count = length < space ? length : space;
        memcpy(ring_write_pointer(&s_update_tx), source, count);
        ring_write_commit(&s_update_tx, (uint32_t)count);
        source += count;
        length -= count;
    }
    return requested;
}

void kstream_link_wait(void)
{
    (void)xSemaphoreTake(s_link_ready, portMAX_DELAY);
}

void kstream_slave_get_stats(kstream_slave_stats_t *stats)
{
    stats->downlink_used = ring_used(&s_downlink);
    stats->downlink_free = ring_free(&s_downlink);
    stats->uplink_used = ring_used(&s_uplink);
    stats->uplink_free = ring_free(&s_uplink);
    stats->console_rx_used = ring_used(&s_console_rx);
    stats->console_tx_used = ring_used(&s_console_tx);
    stats->commands = s_exchanges;
    stats->faults = s_link.stats.faults + s_route_faults;
    stats->bad_magic = s_link.stats.bad_cells;
    stats->bad_crc = s_link.stats.bad_cells;
    stats->calculated_crc = s_last_result;
    stats->stage = s_stage;
    stats->opcode = (uint8_t)ready_level();
    stats->stream = (uint8_t)ack_level();
    stats->downlink_bytes = s_downlink_bytes;
    stats->uplink_bytes = s_uplink_bytes;
}
