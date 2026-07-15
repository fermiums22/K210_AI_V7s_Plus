#include <FreeRTOS.h>
#include <devices.h>
#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sysctl.h>
#include <task.h>
#include <uart.h>
#include <uarths.h>

#include "kstream_slave.h"
#include "kupdate_task.h"

#define PIN_DBG_RX   4
#define PIN_DBG_TX   5
#define PIN_ESP_TX   6
#define PIN_ESP_EN   8
#define PIN_ESP_BOOT 15
#define GPIO_ESP_EN  0
#define GPIO_ESP_BOOT 3
#define LOG_BAUD     115200u

static volatile gpio_t *const GPIO_REG = (volatile gpio_t *)GPIO_BASE_ADDR;
static handle_t s_esp_uart;
static volatile bool s_benchmark_uplink;
static volatile bool s_benchmark_downlink;
static uint64_t s_downlink_expected;
static uint64_t s_downlink_received;
static uint64_t s_downlink_skipped;
static uint64_t s_downlink_errors;

static uint8_t pattern_byte(uint64_t offset);

static TickType_t ticks(uint32_t ms)
{
    TickType_t value = pdMS_TO_TICKS(ms);
    return value == 0u ? 1u : value;
}

static void log_line(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line) - 3u, format, args);
    va_end(args);
    if (length < 0)
        return;
    size_t used = (size_t)length;
    if (used > sizeof(line) - 3u)
        used = sizeof(line) - 3u;
    line[used++] = '\r';
    line[used++] = '\n';
    line[used] = 0;
    (void)kstream_console_write(line, used);
}

static void clock_init(void)
{
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_ACLK, 0u);
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
        --divider;
    if (divider > 0xffffu)
        divider = 0xffffu;
    ((volatile uarths_t *)UARTHS_BASE_ADDR)->div.div = (uint16_t)divider;
}

static void gpio_write(unsigned pin, bool high)
{
    GPIO_REG->direction.u32[0] |= 1u << pin;
    if (high)
        GPIO_REG->data_output.u32[0] |= 1u << pin;
    else
        GPIO_REG->data_output.u32[0] &= ~(1u << pin);
}

static void esp_uart_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t byte;
        if (io_read(s_esp_uart, &byte, 1u) > 0) {
            uarths_write_byte(byte);
            taskYIELD();
        } else {
            vTaskDelay(1u);
        }
    }
}

static bool esp_prepare(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    fpioa_set_function(PIN_ESP_TX, FUNC_UART2_RX);
    fpioa_set_function(PIN_ESP_EN, FUNC_GPIO0);
    fpioa_set_function(PIN_ESP_BOOT, FUNC_GPIO3);
    gpio_write(GPIO_ESP_BOOT, true);
    gpio_write(GPIO_ESP_EN, false);

    s_esp_uart = io_open("/dev/uart2");
    if (!s_esp_uart)
        return false;
    uart_config(s_esp_uart, 115200u, 8u, UART_STOP_1, UART_PARITY_NONE);
    return xTaskCreate(esp_uart_task, "esp_log", 1024u, NULL,
                       KSTREAM_TASK_PRIORITY, NULL) == pdPASS;
}

static void esp_enable(void)
{
    /* IO15 stays driven HIGH.  The ESP application transfers it to the
     * working INT protocol by sending ACTIVATE_INT over SPI. */
    gpio_write(GPIO_ESP_EN, true);
}

static void downlink_sink_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t length;
        uint8_t *source = kstream_downlink_read_acquire(&length);
        if (length == 0u) {
            kstream_downlink_wait();
            continue;
        }
        if (s_benchmark_downlink) {
            for (size_t i = 0u; i < length; ++i) {
                if (source[i] != pattern_byte(s_downlink_expected)) {
                    uint64_t skipped;
                    for (skipped = 1200u; skipped <= 4800u; skipped += 1200u) {
                        size_t matched = 0u;
                        size_t check = length - i;
                        if (check > 16u)
                            check = 16u;
                        while (matched < check &&
                               source[i + matched] == pattern_byte(
                                   s_downlink_expected + skipped + matched))
                            ++matched;
                        if (matched == check)
                            break;
                    }
                    if (skipped <= 4800u) {
                        s_downlink_expected += skipped;
                        s_downlink_skipped += skipped;
                    }
                }
                if (source[i] != pattern_byte(s_downlink_expected))
                    ++s_downlink_errors;
                ++s_downlink_expected;
            }
            s_downlink_received += length;
        }
        /* Robot application replaces this sink with its KNET frame parser.
         * Consuming in-place proves that SPI DMA never needs another copy. */
        kstream_downlink_read_commit(length);
        taskYIELD();
    }
}

static uint8_t pattern_byte(uint64_t offset)
{
    return (uint8_t)(0x6du ^ offset ^ (offset >> 9) ^ (offset >> 21));
}

static void uplink_benchmark_task(void *arg)
{
    (void)arg;
    uint64_t offset = 0u;
    for (;;) {
        if (!s_benchmark_uplink) {
            vTaskDelay(ticks(20u));
            continue;
        }
        size_t length;
        uint8_t *destination = kstream_uplink_write_acquire(&length);
        if (length == 0u) {
            kstream_uplink_wait();
            continue;
        }
        for (size_t i = 0; i < length; ++i)
            destination[i] = pattern_byte(offset + i);
        kstream_uplink_write_commit(length);
        offset += length;
        vTaskDelay(1u);
    }
}

static void console_command(const char *line)
{
    if (strcmp(line, "help") == 0) {
        log_line("commands: help, status, bench on/off, bench down reset/off");
    } else if (strcmp(line, "status") == 0) {
        kstream_slave_stats_t stats;
        kstream_slave_get_stats(&stats);
        log_line("status down=%lu/%lu up=%lu/%lu cmd=%lu faults=%lu rx=%llu tx=%llu dloss=%llu derr=%llu",
                 (unsigned long)stats.downlink_used,
                 (unsigned long)stats.downlink_free,
                 (unsigned long)stats.uplink_used,
                 (unsigned long)stats.uplink_free,
                 (unsigned long)stats.commands, (unsigned long)stats.faults,
                 (unsigned long long)stats.downlink_bytes,
                 (unsigned long long)stats.uplink_bytes,
                 (unsigned long long)s_downlink_skipped,
                 (unsigned long long)s_downlink_errors);
    } else if (strcmp(line, "bench on") == 0) {
        s_benchmark_uplink = true;
        log_line("uplink benchmark enabled");
    } else if (strcmp(line, "bench off") == 0) {
        s_benchmark_uplink = false;
        log_line("uplink benchmark disabled");
    } else if (strcmp(line, "bench down reset") == 0) {
        s_downlink_expected = 0u;
        s_downlink_received = 0u;
        s_downlink_skipped = 0u;
        s_downlink_errors = 0u;
        __sync_synchronize();
        s_benchmark_downlink = true;
        log_line("downlink benchmark enabled");
    } else if (strcmp(line, "bench down off") == 0) {
        s_benchmark_downlink = false;
        log_line("downlink benchmark disabled bytes=%llu skipped=%llu errors=%llu",
                 (unsigned long long)s_downlink_received,
                 (unsigned long long)s_downlink_skipped,
                 (unsigned long long)s_downlink_errors);
    } else if (line[0] != 0) {
        log_line("unknown command: %s", line);
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t used = 0u;
    for (;;) {
        uint8_t byte;
        if (kstream_console_read(&byte, 1u) == 0u) {
            vTaskDelay(1u);
            continue;
        }
        if (byte == '\r')
            continue;
        if (byte == '\n') {
            line[used] = 0;
            console_command(line);
            used = 0u;
            taskYIELD();
        } else if (used + 1u < sizeof(line)) {
            line[used++] = (char)byte;
        }
    }
}

int main(void)
{
    clock_init();
    log_init();
    uarths_puts("KSTREAM:BOOT k210-slave-v2 spi2=slave dma=1 log=115200\r\n");
    if (!esp_prepare()) {
        uarths_puts("KSTREAM:FATAL ESP prepare failed\r\n");
        for (;;)
            vTaskDelay(ticks(1000u));
    }
    if (!kstream_slave_start()) {
        uarths_puts("KSTREAM:FATAL slave init failed\r\n");
        for (;;)
            vTaskDelay(ticks(1000u));
    }
    if (!kupdate_task_start()) {
        uarths_puts("KSTREAM:FATAL update task failed\r\n");
        for (;;)
            vTaskDelay(ticks(1000u));
    }
    /* This delay is only the EN-low reset pulse.  It is not a boot-readiness
     * decision: IO15 remains HIGH until ACTIVATE_INT is received. */
    vTaskDelay(ticks(100u));
    esp_enable();
    xTaskCreate(downlink_sink_task, "down_sink", 1536u, NULL,
                KSTREAM_TASK_PRIORITY, NULL);
    xTaskCreate(uplink_benchmark_task, "up_source", 1536u, NULL,
                KSTREAM_TASK_PRIORITY, NULL);
    xTaskCreate(console_task, "console", 1536u, NULL,
                KSTREAM_TASK_PRIORITY, NULL);
    log_line("KSTREAM:SLAVE_READY cs=io0 clk=io1 miso=io2 mosi=io3");
    for (;;) {
        kstream_slave_stats_t stats;
        kstream_slave_get_stats(&stats);
        char heartbeat[192];
        snprintf(heartbeat, sizeof(heartbeat),
                 "KSTREAM:HEARTBEAT stage=%lu op=%u stream=%u commands=%lu faults=%lu crx=%lu bad=%08lx crc=%08lx/%08lx down=%llu up=%llu\r\n",
                 (unsigned long)stats.stage, (unsigned)stats.opcode,
                 (unsigned)stats.stream,
                 (unsigned long)stats.commands, (unsigned long)stats.faults,
                 (unsigned long)stats.console_rx_used,
                 (unsigned long)stats.bad_magic, (unsigned long)stats.bad_crc,
                 (unsigned long)stats.calculated_crc,
                 (unsigned long long)stats.downlink_bytes,
                 (unsigned long long)stats.uplink_bytes);
        uarths_puts(heartbeat);
        vTaskDelay(ticks(1000u));
    }
}
