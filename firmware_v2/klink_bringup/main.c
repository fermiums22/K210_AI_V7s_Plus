#include <FreeRTOS.h>
#include <task.h>

#include <devices.h>
#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <spi.h>
#include <sysctl.h>
#include <uart.h>
#include <uarths.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "klink_v1.h"
#include "klink_v1_link.h"
#include "kbulk_v1.h"

#define PIN_ESP_SPI_CS    0
#define PIN_ESP_SPI_CLK   1
#define PIN_ESP_SPI_MISO  2
#define PIN_ESP_SPI_MOSI  3
#define PIN_DBG_RX        4
#define PIN_DBG_TX        5
#define PIN_ESP_TX        6
#define PIN_ESP_RX        7
#define PIN_ESP_EN        8
#define PIN_ESP_BOOT      15
#define GPIO_ESP_EN       0
#define GPIO_ESP_BOOT     3

#define LOG_BAUD          115200u
#define ESP_BOOT_MARKER   "STA_READY ssid=Fermiums_2.4"
#define CELLS_PER_RATE    4096u
#define STREAM_TEST_BYTES (1024u * 1024u)
#define LINK_ACQUIRE_CELLS 64u
#define WIRE_PREFIX_BYTES 2u
#define WIRE_CELL_BYTES   (WIRE_PREFIX_BYTES + KLINK_V1_CELL_BYTES)
#define MAX_CELLS_PER_ECHO 64u
#define WIRE_WRITE_OPCODE 2u
#define WIRE_READ_OPCODE  3u
#define K210_CYCLES_PER_US 390u
#define ESP_EDGE_DEADLINE_US 50000u
#define BULK_TEST_PAYLOAD_BYTES (1024u * 1024u)
#define FDX_FRAME_DATA_BYTES 64u
#define FDX_WIRE_BYTES       (2u + FDX_FRAME_DATA_BYTES)
#define STREAM_PAYLOAD_BYTES 60u
#define STREAM_MODE_WRITE    1u
#define STREAM_MODE_READ     2u
#define STREAM_ACK           0xa0000000u
#define STREAM_CS_GAP_US     2u

static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;
static handle_t s_spi;
static handle_t s_spi_device;
static handle_t s_esp_uart;
static klink_v1_endpoint_t s_link;
static uint8_t s_tx[WIRE_CELL_BYTES] __attribute__((aligned(64)));
static uint8_t s_rx[WIRE_CELL_BYTES] __attribute__((aligned(64)));
static uint8_t s_bulk_tx[KBULK_V1_BLOCK_BYTES] __attribute__((aligned(64)));
static uint8_t s_bulk_rx[KBULK_V1_BLOCK_BYTES] __attribute__((aligned(64)));
static uint8_t s_bulk_wire[WIRE_PREFIX_BYTES + KLINK_V1_CELL_BYTES] __attribute__((aligned(64)));
static uint8_t s_fdx_tx[FDX_WIRE_BYTES] __attribute__((aligned(64)));
static uint8_t s_fdx_rx[FDX_WIRE_BYTES] __attribute__((aligned(64)));
static const char *s_halt_reason = "unspecified";
static uint32_t s_bulk_result_bytes;
static uint32_t s_bulk_result_blocks;
static uint32_t s_bulk_result_elapsed_ms;
static uint32_t s_bulk_result_bps;
static uint32_t s_stream_write_bps;
static uint32_t s_stream_read_bps;
static uint32_t s_last_stream_status;
static uint32_t s_stream_status_polls;


static uint64_t cycle_count(void)
{
    uint64_t value;
    __asm__ volatile("rdcycle %0" : "=r"(value));
    return value;
}

static void log_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1u] = 0;
    uarths_puts(line);
    uarths_puts("\r\n");
}

static TickType_t ticks_at_least_one(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks == 0 ? 1 : ticks;
}

static void halt_forever(uint32_t code)
{
    for (;;) {
        log_line("KLINK:HALTED code=%lu reason=%s status=0x%08lx",
                 (unsigned long)code, s_halt_reason,
                 (unsigned long)s_last_stream_status);
        vTaskDelay(ticks_at_least_one(1000));
    }
}

static void clock_init(void)
{
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_ACLK, 0);
    sysctl_pll_set_freq(SYSCTL_PLL0, 780000000u);
    sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_ACLK, SYSCTL_SOURCE_PLL0);
    sysctl_pll_set_freq(SYSCTL_PLL1, 160000000u);
    sysctl_pll_set_freq(SYSCTL_PLL2, 45158400u);
}

static void log_init(void)
{
    fpioa_set_function(PIN_DBG_RX, FUNC_UARTHS_RX);
    fpioa_set_function(PIN_DBG_TX, FUNC_UARTHS_TX);
    uarths_init();
    uint32_t divider = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU) / LOG_BAUD;
    if (divider != 0u)
        divider--;
    if (divider > 0xffffu)
        divider = 0xffffu;
    ((volatile uarths_t *)UARTHS_BASE_ADDR)->div.div = (uint16_t)divider;
}

static void gpio_write(int gpio_number, int value)
{
    REG_GPIO->direction.u32[0] |= 1u << gpio_number;
    if (value)
        REG_GPIO->data_output.u32[0] |= 1u << gpio_number;
    else
        REG_GPIO->data_output.u32[0] &= ~(1u << gpio_number);
}

static uint32_t gpio_read(int gpio_number)
{
    return (REG_GPIO->data_input.u32[0] >> gpio_number) & 1u;
}

static void esp_uart_log_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t byte;
        if (io_read(s_esp_uart, &byte, 1) > 0)
            uarths_write_byte(byte);
        else
            vTaskDelay(1u);
    }
}

static bool boot_esp_and_wait_marker(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    fpioa_set_function(PIN_ESP_TX, FUNC_UART2_RX);
    fpioa_set_function(PIN_ESP_RX, FUNC_UART2_TX);
    fpioa_set_function(PIN_ESP_EN, FUNC_GPIO0);
    fpioa_set_function(PIN_ESP_BOOT, FUNC_GPIO3);
    gpio_write(GPIO_ESP_BOOT, 1);
    gpio_write(GPIO_ESP_EN, 0);

    s_esp_uart = io_open("/dev/uart2");
    if (!s_esp_uart)
        return false;
    uart_config(s_esp_uart, 115200u, 8, UART_STOP_1, UART_PARITY_NONE);
    vTaskDelay(ticks_at_least_one(100));
    gpio_write(GPIO_ESP_EN, 1);
    vTaskDelay(ticks_at_least_one(500));
    REG_GPIO->direction.u32[0] &= ~(1u << GPIO_ESP_BOOT);
    return xTaskCreate(esp_uart_log_task, "esp_uart_log", 1024, NULL, 2u,
                       NULL) == pdPASS;
}

static bool spi_init_fixed(void)
{
    fpioa_set_function(PIN_ESP_SPI_CS, FUNC_SPI1_SS0);
    fpioa_set_function(PIN_ESP_SPI_CLK, FUNC_SPI1_SCLK);
    fpioa_set_function(PIN_ESP_SPI_MOSI, FUNC_SPI1_D0);
    fpioa_set_function(PIN_ESP_SPI_MISO, FUNC_SPI1_D1);

    s_spi = io_open("/dev/spi1");
    if (!s_spi)
        return false;
    s_spi_device = spi_get_device(s_spi, SPI_MODE_0, SPI_FF_STANDARD, 1u, 8u);
    return s_spi_device != 0;
}

static int transfer_cell(klink_v1_cell_t *tx, klink_v1_cell_t *rx)
{
    if (gpio_read(GPIO_ESP_BOOT) == 0u)
        return -100;

    uint8_t read_prefix[WIRE_PREFIX_BYTES] = { WIRE_READ_OPCODE, 0u };
    memset(s_rx, 0, KLINK_V1_CELL_BYTES);
    int read = spi_dev_transfer_sequential(s_spi_device, read_prefix,
                                           sizeof(read_prefix), s_rx,
                                           KLINK_V1_CELL_BYTES);
    if (read != (int)KLINK_V1_CELL_BYTES)
        return read;
    memcpy(rx, s_rx, sizeof(*rx));

    uint64_t read_done_deadline =
        cycle_count() + (uint64_t)K210_CYCLES_PER_US * ESP_EDGE_DEADLINE_US;
    while (gpio_read(GPIO_ESP_BOOT) != 0u) {
        if ((int64_t)(read_done_deadline - cycle_count()) <= 0)
            return -101;
    }

    s_tx[0] = WIRE_WRITE_OPCODE;
    s_tx[1] = 0u;
    memcpy(s_tx + WIRE_PREFIX_BYTES, tx, sizeof(*tx));
    int written = io_write(s_spi_device, s_tx, sizeof(s_tx));
    if (written != (int)sizeof(s_tx))
        return written;

    uint64_t write_done_deadline =
        cycle_count() + (uint64_t)K210_CYCLES_PER_US * ESP_EDGE_DEADLINE_US;
    while (gpio_read(GPIO_ESP_BOOT) == 0u) {
        if ((int64_t)(write_done_deadline - cycle_count()) <= 0)
            return -102;
    }
    return read;
}

static bool wait_ready_level(uint32_t level)
{
    uint64_t deadline =
        cycle_count() + (uint64_t)K210_CYCLES_PER_US * ESP_EDGE_DEADLINE_US;
    while (gpio_read(GPIO_ESP_BOOT) != level) {
        if ((int64_t)(deadline - cycle_count()) <= 0)
            return false;
    }
    return true;
}

static bool wait_ready_toggle(uint32_t previous)
{
    return wait_ready_level(previous ^ 1u);
}

static bool bulk_write_status(uint32_t length)
{
    uint8_t command[5];
    command[0] = 1u;
    memcpy(command + 1u, &length, sizeof(length));
    uint32_t previous = gpio_read(GPIO_ESP_BOOT);
    if (io_write(s_spi_device, command, sizeof(command)) != (int)sizeof(command))
        return false;
    return wait_ready_toggle(previous);
}

static bool bulk_write_block(const uint8_t *block)
{
    for (uint32_t offset = 0; offset < KBULK_V1_BLOCK_BYTES;
         offset += KLINK_V1_CELL_BYTES) {
        s_bulk_wire[0] = WIRE_WRITE_OPCODE;
        s_bulk_wire[1] = 0u;
        memcpy(s_bulk_wire + WIRE_PREFIX_BYTES, block + offset,
               KLINK_V1_CELL_BYTES);
        uint32_t previous = gpio_read(GPIO_ESP_BOOT);
        if (io_write(s_spi_device, s_bulk_wire, sizeof(s_bulk_wire)) !=
            (int)sizeof(s_bulk_wire))
            return false;
        if (!wait_ready_toggle(previous))
            return false;
    }
    return true;
}

static bool bulk_read_status(uint32_t *length)
{
    uint8_t command = 4u;
    *length = 0u;
    uint32_t previous = gpio_read(GPIO_ESP_BOOT);
    if (spi_dev_transfer_sequential(s_spi_device, &command, sizeof(command),
                                    (uint8_t *)length, sizeof(*length)) !=
        (int)sizeof(*length))
        return false;
    return wait_ready_toggle(previous);
}

static bool bulk_read_block(uint8_t *block)
{
    const uint8_t command[WIRE_PREFIX_BYTES] = { WIRE_READ_OPCODE, 0u };
    for (uint32_t offset = 0; offset < KBULK_V1_BLOCK_BYTES;
         offset += KLINK_V1_CELL_BYTES) {
        uint32_t previous = gpio_read(GPIO_ESP_BOOT);
        if (spi_dev_transfer_sequential(s_spi_device, command, sizeof(command),
                                        block + offset,
                                        KLINK_V1_CELL_BYTES) !=
            (int)KLINK_V1_CELL_BYTES)
            return false;
        if (!wait_ready_toggle(previous))
            return false;
    }
    return true;
}

static void fill_bulk_payload(uint8_t *payload, uint32_t length,
                              uint32_t absolute_offset)
{
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t position = absolute_offset + i;
        payload[i] = (uint8_t)(0x69u ^ position ^ (position >> 7) ^
                               (position >> 15) ^ (i * 29u));
    }
}

static void fill_fdx_frame(uint8_t *frame, uint32_t index)
{
    memcpy(frame, &index, sizeof(index));
    uint32_t base = index * STREAM_PAYLOAD_BYTES;
    for (uint32_t i = 0; i < STREAM_PAYLOAD_BYTES; ++i) {
        uint32_t position = base + i;
        frame[4u + i] = position < BULK_TEST_PAYLOAD_BYTES
            ? (uint8_t)(0x5du ^ position ^ (position >> 7) ^
                        (position >> 15) ^ (position >> 23))
            : 0u;
    }
}

static void stream_cs_gap(void)
{
    uint64_t until = cycle_count() +
        (uint64_t)K210_CYCLES_PER_US * STREAM_CS_GAP_US;
    while ((int64_t)(until - cycle_count()) > 0)
        ;
}

static bool stream_write_status(uint32_t request)
{
    uint8_t command[5] = {1u, 0u, 0u, 0u, 0u};
    memcpy(command + 1u, &request, sizeof(request));
    return io_write(s_spi_device, command, sizeof(command)) == (int)sizeof(command);
}

static bool stream_read_status(uint32_t *status)
{
    uint8_t command = 4u;
    *status = 0u;
    return spi_dev_transfer_sequential(s_spi_device, &command, 1u,
                                       (uint8_t *)status, sizeof(*status)) ==
           (int)sizeof(*status);
}

static bool stream_wait_status(uint32_t expected, uint32_t *status)
{
    for (uint32_t poll = 1u; poll <= 100000u; ++poll) {
        if (!stream_read_status(status))
            return false;
        if (*status == expected) {
            s_stream_status_polls += poll;
            return true;
        }
        if ((*status & 0xf0000000u) == 0xf0000000u)
            return false;
    }
    return false;
}

static bool run_cs_stream(void)
{
    double actual = spi_dev_set_clock_rate(s_spi_device, 20000000.0);
    const uint32_t frames =
        (BULK_TEST_PAYLOAD_BYTES + STREAM_PAYLOAD_BYTES - 1u) /
        STREAM_PAYLOAD_BYTES;
    uint32_t status = 0u;

    if (!stream_write_status((STREAM_MODE_WRITE << 28) | frames) ||
        !stream_wait_status(STREAM_ACK, &status)) {
        s_last_stream_status = status;
        s_halt_reason = "stream-write-open";
        return false;
    }
    TickType_t start = xTaskGetTickCount();
    for (uint32_t frame = 0; frame < frames; ++frame) {
        s_fdx_tx[0] = WIRE_WRITE_OPCODE;
        s_fdx_tx[1] = 0u;
        fill_fdx_frame(s_fdx_tx + 2u, frame);
        if (io_write(s_spi_device, s_fdx_tx, sizeof(s_fdx_tx)) !=
            (int)sizeof(s_fdx_tx)) {
            s_halt_reason = "stream-write-transfer";
            return false;
        }
        stream_cs_gap();
    }
    if (!stream_wait_status(STREAM_ACK | frames, &status)) {
        s_last_stream_status = status;
        s_halt_reason = "stream-write-verify";
        log_line("STREAM:FAIL write-status=0x%08lx expected=%lu",
                 (unsigned long)status, (unsigned long)frames);
        return false;
    }
    uint32_t elapsed_write =
        (uint32_t)(xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    if (elapsed_write == 0u)
        elapsed_write = 1u;
    s_stream_write_bps =
        (uint32_t)(((uint64_t)BULK_TEST_PAYLOAD_BYTES * 1000u) / elapsed_write);

    if (!stream_write_status((STREAM_MODE_READ << 28) | frames) ||
        !stream_wait_status(STREAM_ACK, &status)) {
        s_last_stream_status = status;
        s_halt_reason = "stream-read-open";
        return false;
    }
    const uint8_t read_command[2] = { WIRE_READ_OPCODE, 0u };
    uint8_t expected[FDX_FRAME_DATA_BYTES];
    start = xTaskGetTickCount();
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (spi_dev_transfer_sequential(s_spi_device, read_command,
                                        sizeof(read_command), s_fdx_rx,
                                        FDX_FRAME_DATA_BYTES) !=
            (int)FDX_FRAME_DATA_BYTES) {
            s_halt_reason = "stream-read-transfer";
            return false;
        }
        stream_cs_gap();
        fill_fdx_frame(expected, frame);
        if (memcmp(s_fdx_rx, expected, sizeof(expected)) != 0) {
            uint32_t observed = 0u;
            memcpy(&observed, s_fdx_rx, sizeof(observed));
            s_halt_reason = "stream-read-payload";
            log_line("STREAM:FAIL read frame=%lu observed=%lu",
                     (unsigned long)frame, (unsigned long)observed);
            return false;
        }
    }
    if (!stream_wait_status(STREAM_ACK | frames, &status)) {
        s_last_stream_status = status;
        s_halt_reason = "stream-read-verify";
        return false;
    }
    uint32_t elapsed_read =
        (uint32_t)(xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    if (elapsed_read == 0u)
        elapsed_read = 1u;
    s_stream_read_bps =
        (uint32_t)(((uint64_t)BULK_TEST_PAYLOAD_BYTES * 1000u) / elapsed_read);
    s_bulk_result_bytes = BULK_TEST_PAYLOAD_BYTES;
    s_bulk_result_blocks = frames;
    s_bulk_result_elapsed_ms = elapsed_write + elapsed_read;
    s_bulk_result_bps = s_stream_write_bps;
    log_line("STREAM:PASS payload=%lu frames=%lu write_Bps=%lu read_Bps=%lu spi=%lu status_polls=%lu ready_gpio=unused",
             (unsigned long)s_bulk_result_bytes,
             (unsigned long)s_bulk_result_blocks,
             (unsigned long)s_stream_write_bps,
             (unsigned long)s_stream_read_bps,
             (unsigned long)actual,
             (unsigned long)s_stream_status_polls);
    return true;
}

static bool run_buffered_bulk(void)
{
    (void)spi_dev_set_clock_rate(s_spi_device, 20000000.0);
    uint32_t payload_done = 0u;
    uint32_t sequence = 0u;
    TickType_t start = xTaskGetTickCount();
    while (payload_done < BULK_TEST_PAYLOAD_BYTES) {
        kbulk_v1_header_t *header = (kbulk_v1_header_t *)s_bulk_tx;
        uint32_t length = BULK_TEST_PAYLOAD_BYTES - payload_done;
        if (length > KBULK_V1_PAYLOAD_BYTES)
            length = KBULK_V1_PAYLOAD_BYTES;
        memset(s_bulk_tx, 0, sizeof(s_bulk_tx));
        header->magic = KBULK_V1_MAGIC;
        header->sequence = sequence;
        header->payload_length = length;
        fill_bulk_payload(s_bulk_tx + sizeof(*header), length, payload_done);
        header->payload_crc32 =
            klink_v1_crc32(s_bulk_tx + sizeof(*header), length);

        if (!bulk_write_status(KBULK_V1_BLOCK_BYTES)) {
            s_halt_reason = "bulk-write-status";
            log_line("KBULK:FAIL write-status seq=%lu ready=%lu",
                     (unsigned long)sequence,
                     (unsigned long)gpio_read(GPIO_ESP_BOOT));
            return false;
        }
        if (!bulk_write_block(s_bulk_tx)) {
            s_halt_reason = "bulk-write-block";
            log_line("KBULK:FAIL write seq=%lu ready=%lu",
                     (unsigned long)sequence,
                     (unsigned long)gpio_read(GPIO_ESP_BOOT));
            return false;
        }

        uint32_t available = 0u;
        if (!bulk_read_status(&available)) {
            s_halt_reason = "bulk-read-status";
            log_line("KBULK:FAIL read-status seq=%lu",
                     (unsigned long)sequence);
            return false;
        }
        if (available == 0u) {
            if (!bulk_read_status(&available)) {
                s_halt_reason = "bulk-read-status-after-ready";
                return false;
            }
        }
        if (available != KBULK_V1_BLOCK_BYTES) {
            s_halt_reason = "bulk-available-length";
            log_line("KBULK:FAIL available seq=%lu available=%lu",
                     (unsigned long)sequence, (unsigned long)available);
            return false;
        }
        if (!bulk_read_block(s_bulk_rx)) {
            s_halt_reason = "bulk-read-block";
            log_line("KBULK:FAIL read-block seq=%lu ready=%lu",
                     (unsigned long)sequence,
                     (unsigned long)gpio_read(GPIO_ESP_BOOT));
            return false;
        }
        if (memcmp(s_bulk_rx, s_bulk_tx, sizeof(s_bulk_tx)) != 0) {
            s_halt_reason = "bulk-memcmp";
            kbulk_v1_header_t *received = (kbulk_v1_header_t *)s_bulk_rx;
            log_line("KBULK:FAIL verify seq=%lu available=%lu magic=0x%08lx rx_seq=%lu",
                     (unsigned long)sequence, (unsigned long)available,
                     (unsigned long)received->magic,
                     (unsigned long)received->sequence);
            return false;
        }
        payload_done += length;
        sequence++;
    }

    uint32_t elapsed_ms = (uint32_t)(xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    if (elapsed_ms == 0u)
        elapsed_ms = 1u;
    uint32_t bytes_per_second =
        (uint32_t)(((uint64_t)payload_done * 1000u) / elapsed_ms);
    s_bulk_result_bytes = payload_done;
    s_bulk_result_blocks = sequence;
    s_bulk_result_elapsed_ms = elapsed_ms;
    s_bulk_result_bps = bytes_per_second;
    log_line("KBULK:PASS payload=%lu blocks=%lu elapsed_ms=%lu payload_Bps=%lu spi=19500000",
             (unsigned long)payload_done, (unsigned long)sequence,
             (unsigned long)elapsed_ms, (unsigned long)bytes_per_second);
    return true;
}

static bool acquire_link(void)
{
    bool capabilities_seen = false;
    for (uint32_t cell_index = 0; cell_index < LINK_ACQUIRE_CELLS; ++cell_index) {
        klink_v1_cell_t tx;
        klink_v1_cell_t rx;
        klink_v1_build_tx(&s_link, &tx);
        int transferred = transfer_cell(&tx, &rx);
        if (transferred != (int)KLINK_V1_CELL_BYTES) {
            log_line("KLINK:FAIL acquire-transfer cell=%lu result=%d ready=%lu",
                     (unsigned long)cell_index, transferred,
                     (unsigned long)gpio_read(GPIO_ESP_BOOT));
            return false;
        }
        if (!klink_v1_cell_validate(&rx)) {
            const uint8_t *raw = (const uint8_t *)&rx;
            log_line("KLINK:FAIL acquire-cell cell=%lu raw=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     (unsigned long)cell_index,
                     raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                     raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
            return false;
        }
        klink_v1_event_t event;
        (void)klink_v1_process_rx(&s_link, &rx, &event);
        if ((event.flags & KLINK_EVENT_FAULT) != 0u) {
            log_line("KLINK:FAIL acquire-fault=%u channel=%u expected=%u observed=%u",
                     (unsigned)s_link.fault, (unsigned)s_link.fault_channel,
                     (unsigned)s_link.fault_expected, (unsigned)s_link.fault_observed);
            return false;
        }
        if ((event.flags & KLINK_EVENT_RX) != 0u &&
            (rx.flags & KLINK_F_RELIABLE) != 0u) {
            (void)klink_v1_release_credit(&s_link, event.rx_channel, 1u);
            if (event.rx_channel == KLINK_CH_CONTROL &&
                event.rx_type == KLINK_T_CAPABILITIES)
                capabilities_seen = true;
        }
        if (capabilities_seen && KLINK_TYPE(rx.channel_type) == KLINK_T_IDLE &&
            s_link.peer_credit[KLINK_CH_DIAG] != 0u) {
            log_line("KLINK:SYNC cells=%lu diag_credit=%u",
                     (unsigned long)(cell_index + 1u),
                     (unsigned)s_link.peer_credit[KLINK_CH_DIAG]);
            return true;
        }
    }
    log_line("KLINK:FAIL acquire-no-capabilities cells=%lu",
             (unsigned long)LINK_ACQUIRE_CELLS);
    return false;
}

static void make_pattern(uint8_t *payload, uint32_t counter, uint32_t rate)
{
    for (uint32_t i = 0; i < KLINK_V1_PAYLOAD_BYTES; ++i)
        payload[i] = (uint8_t)(0x5au ^ (counter * 37u) ^ (rate >> (i & 7u)) ^ (i * 13u));
    payload[0] = (uint8_t)counter;
    payload[1] = (uint8_t)(counter >> 8);
    payload[2] = (uint8_t)(counter >> 16);
    payload[3] = (uint8_t)(counter >> 24);
}

static bool run_rate(uint32_t rate)
{
    double actual = spi_dev_set_clock_rate(s_spi_device, (double)rate);
    log_line("KLINK:RATE_BEGIN requested=%lu actual=%lu cells=%lu",
             (unsigned long)rate, (unsigned long)actual, (unsigned long)CELLS_PER_RATE);

    uint8_t expected[KLINK_V1_PAYLOAD_BYTES];
    uint32_t sent = 0;
    uint32_t echoed = 0;
    bool awaiting_echo = false;
    uint32_t cells_for_echo = 0;
    TickType_t start = xTaskGetTickCount();

    while (echoed < CELLS_PER_RATE) {
        if (!awaiting_echo && sent < CELLS_PER_RATE) {
            make_pattern(expected, sent, rate);
            if (!klink_v1_queue(&s_link, KLINK_CH_DIAG, KLINK_T_DATA,
                                KLINK_F_RELIABLE, expected, sizeof(expected))) {
                log_line("KLINK:FAIL queue rate=%lu sent=%lu", (unsigned long)rate,
                         (unsigned long)sent);
                return false;
            }
            awaiting_echo = true;
            cells_for_echo = 0;
            sent++;
        }

        klink_v1_cell_t tx;
        klink_v1_cell_t rx;
        klink_v1_event_t event;
        klink_v1_build_tx(&s_link, &tx);
        int transferred = transfer_cell(&tx, &rx);
        cells_for_echo++;
        if (awaiting_echo && cells_for_echo > MAX_CELLS_PER_ECHO) {
            log_line("KLINK:FAIL echo-deadlock rate=%lu sent=%lu echoed=%lu inflight=%u queued=0x%02x",
                     (unsigned long)rate, (unsigned long)sent, (unsigned long)echoed,
                     s_link.inflight ? 1u : 0u, (unsigned)s_link.tx_queued_mask);
            return false;
        }
        if (transferred < (int)KLINK_V1_CELL_BYTES) {
            log_line("KLINK:FAIL short-transfer rate=%lu got=%d", (unsigned long)rate, transferred);
            return false;
        }
        (void)klink_v1_process_rx(&s_link, &rx, &event);
        if ((event.flags & KLINK_EVENT_FAULT) != 0u) {
            log_line("KLINK:FAIL fault=%u rate=%lu echoed=%lu bad=%lu",
                     event.fault, (unsigned long)rate, (unsigned long)echoed,
                     (unsigned long)s_link.stats.bad_cells);
            return false;
        }
        if ((event.flags & KLINK_EVENT_RX) != 0u) {
            if (event.rx_channel == KLINK_CH_CONTROL) {
                (void)klink_v1_release_credit(&s_link, event.rx_channel, 1u);
            } else if (event.rx_channel == KLINK_CH_DIAG && event.rx_type == KLINK_T_DATA) {
                if (!awaiting_echo || rx.payload_length != sizeof(expected) ||
                    memcmp(rx.payload, expected, sizeof(expected)) != 0) {
                    log_line("KLINK:FAIL echo-mismatch rate=%lu echoed=%lu seq=%u",
                             (unsigned long)rate, (unsigned long)echoed, event.rx_sequence);
                    return false;
                }
                (void)klink_v1_release_credit(&s_link, event.rx_channel, 1u);
                awaiting_echo = false;
                echoed++;
            }
        }
    }

    /* One final physical transaction delivers the ACK for the last echo. */
    klink_v1_cell_t tx;
    klink_v1_cell_t rx;
    klink_v1_event_t event;
    klink_v1_build_tx(&s_link, &tx);
    if (transfer_cell(&tx, &rx) < (int)KLINK_V1_CELL_BYTES) {
        log_line("KLINK:FAIL final-ack-transfer rate=%lu", (unsigned long)rate);
        return false;
    }
    (void)klink_v1_process_rx(&s_link, &rx, &event);
    if ((event.flags & KLINK_EVENT_FAULT) != 0u)
        return false;

    uint32_t elapsed_ms = (uint32_t)(xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    if (elapsed_ms == 0u)
        elapsed_ms = 1u;
    uint32_t bytes_per_second =
        (uint32_t)(((uint64_t)CELLS_PER_RATE * KLINK_V1_PAYLOAD_BYTES * 1000u) / elapsed_ms);
    log_line("KLINK:RATE_PASS requested=%lu actual=%lu elapsed_ms=%lu payload_Bps=%lu dup=%lu",
             (unsigned long)rate, (unsigned long)actual, (unsigned long)elapsed_ms,
             (unsigned long)bytes_per_second, (unsigned long)s_link.stats.rx_duplicates);
    return true;
}

static bool run_stream_rate(uint32_t rate)
{
    double actual = spi_dev_set_clock_rate(s_spi_device, (double)rate);
    log_line("KLINK:STREAM_BEGIN requested=%lu actual=%lu bytes=%lu",
             (unsigned long)rate, (unsigned long)actual,
             (unsigned long)STREAM_TEST_BYTES);

    uint32_t sent_bytes = 0;
    uint32_t echoed_bytes = 0;
    uint32_t transfers = 0;
    TickType_t start = xTaskGetTickCount();

    while (echoed_bytes < STREAM_TEST_BYTES) {
        if (sent_bytes < STREAM_TEST_BYTES) {
            uint8_t payload[KLINK_V1_PAYLOAD_BYTES];
            uint32_t count = STREAM_TEST_BYTES - sent_bytes;
            if (count > sizeof(payload))
                count = sizeof(payload);
            make_pattern(payload, sent_bytes / KLINK_V1_PAYLOAD_BYTES, rate);
            if (!klink_v1_queue(&s_link, KLINK_CH_DIAG, KLINK_T_DATA, 0u,
                                payload, count)) {
                log_line("KLINK:FAIL stream-queue sent=%lu echoed=%lu",
                         (unsigned long)sent_bytes, (unsigned long)echoed_bytes);
                return false;
            }
            sent_bytes += count;
        }

        klink_v1_cell_t tx;
        klink_v1_cell_t rx;
        klink_v1_event_t event;
        klink_v1_build_tx(&s_link, &tx);
        int transferred = transfer_cell(&tx, &rx);
        transfers++;
        if (transferred != (int)KLINK_V1_CELL_BYTES) {
            log_line("KLINK:FAIL stream-transfer result=%d sent=%lu echoed=%lu",
                     transferred, (unsigned long)sent_bytes,
                     (unsigned long)echoed_bytes);
            return false;
        }
        (void)klink_v1_process_rx(&s_link, &rx, &event);
        if ((event.flags & KLINK_EVENT_FAULT) != 0u) {
            log_line("KLINK:FAIL stream-fault=%u sent=%lu echoed=%lu",
                     event.fault, (unsigned long)sent_bytes,
                     (unsigned long)echoed_bytes);
            return false;
        }
        if ((event.flags & KLINK_EVENT_RX) != 0u &&
            event.rx_channel == KLINK_CH_DIAG &&
            event.rx_type == KLINK_T_DATA) {
            uint8_t expected[KLINK_V1_PAYLOAD_BYTES];
            uint32_t count = STREAM_TEST_BYTES - echoed_bytes;
            if (count > sizeof(expected))
                count = sizeof(expected);
            make_pattern(expected, echoed_bytes / KLINK_V1_PAYLOAD_BYTES, rate);
            if ((rx.flags & KLINK_F_RELIABLE) != 0u ||
                rx.payload_length != count ||
                memcmp(rx.payload, expected, count) != 0) {
                uint32_t actual_index = 0;
                uint32_t expected_index = echoed_bytes / KLINK_V1_PAYLOAD_BYTES;
                if (rx.payload_length >= sizeof(actual_index))
                    memcpy(&actual_index, rx.payload, sizeof(actual_index));
                log_line("KLINK:FAIL stream-mismatch transfer=%lu echoed=%lu len=%u seq=%u expected_index=%lu actual_index=%lu gaps=%lu",
                         (unsigned long)transfers, (unsigned long)echoed_bytes,
                         (unsigned)rx.payload_length, (unsigned)rx.sequence,
                         (unsigned long)expected_index, (unsigned long)actual_index,
                         (unsigned long)s_link.stats.rx_realtime_gaps);
                return false;
            }
            echoed_bytes += count;
        }
    }

    uint32_t elapsed_ms = (uint32_t)(xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    if (elapsed_ms == 0u)
        elapsed_ms = 1u;
    uint32_t bytes_per_second =
        (uint32_t)(((uint64_t)STREAM_TEST_BYTES * 1000u) / elapsed_ms);
    log_line("KLINK:STREAM_PASS actual=%lu bytes=%lu elapsed_ms=%lu payload_Bps=%lu transfers=%lu gaps=%lu bad=%lu",
             (unsigned long)actual, (unsigned long)STREAM_TEST_BYTES,
             (unsigned long)elapsed_ms, (unsigned long)bytes_per_second,
             (unsigned long)transfers,
             (unsigned long)s_link.stats.rx_realtime_gaps,
             (unsigned long)s_link.stats.bad_cells);
    return true;
}

int main(void)
{
    clock_init();
    log_init();
    log_line("KLINK:BOOT v2 fixed-map spi1 mode=0 ready=io15 wr=2 rd=3 addr=8 cell=64 log=115200");

    if (!boot_esp_and_wait_marker()) {
        log_line("KLINK:STOP ESP transport boot failed");
        s_halt_reason = "esp-transport-boot";
        halt_forever(1u);
    }
    log_line("KLINK:ESP_TRANSPORT_BOOTED");

    if (!spi_init_fixed()) {
        log_line("KLINK:STOP SPI1 init failed");
        halt_forever(2u);
    }
    if (!run_cs_stream())
        halt_forever(8u);
    for (;;) {
        log_line("STREAM:PASS payload=%lu frames=%lu write_Bps=%lu read_Bps=%lu spi=max dma=1 ready_gpio=unused",
                 (unsigned long)s_bulk_result_bytes,
                 (unsigned long)s_bulk_result_blocks,
                 (unsigned long)s_stream_write_bps,
                 (unsigned long)s_stream_read_bps);
        vTaskDelay(ticks_at_least_one(1000));
    }
}
