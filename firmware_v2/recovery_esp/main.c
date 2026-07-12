#include <FreeRTOS.h>
#include <task.h>

#include <devices.h>
#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <sysctl.h>
#include <uart.h>
#include <uarths.h>

#include <esp_loader.h>
#include <esp_loader_io.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define PIN_DBG_RX       4
#define PIN_DBG_TX       5
#define PIN_ESP_TX       6
#define PIN_ESP_RX       7
#define PIN_ESP_EN       8
#define PIN_ESP_BOOT     15
#define GPIO_ESP_EN      0
#define GPIO_ESP_BOOT    3

#define LOG_BAUD         115200u
#define ESP_BOOT_LOG_BAUD 115200u
#define ESP_ROM_BAUD     115200u
#define ESP_FLASH_BAUD   921600u
#define ESP_FLASH_BLOCK  16384u
#ifndef ESP_BOOT_MARKER
#define ESP_BOOT_MARKER  "HSPI_ARMED kesp-v2-physical-29-xtal40"
#endif
#if defined(RECOVERY_AT_TEST) || defined(RECOVERY_FORCE_FLASH)
#define RECOVERY_FORCE_FLASH_ONCE 1
#else
#define RECOVERY_FORCE_FLASH_ONCE 0
#endif

#ifdef RECOVERY_AT_TEST
extern const uint8_t esp_at_full_start[];
extern const uint8_t esp_at_full_end[];
#else
extern const uint8_t esp_v2_boot_start[];
extern const uint8_t esp_v2_boot_end[];
extern const uint8_t esp_v2_part_start[];
extern const uint8_t esp_v2_part_end[];
extern const uint8_t esp_v2_app_start[];
extern const uint8_t esp_v2_app_end[];
extern const uint8_t esp_v2_phy_init_start[];
extern const uint8_t esp_v2_phy_init_end[];
extern const uint8_t esp_v2_sys_param_blank_start[];
extern const uint8_t esp_v2_sys_param_blank_end[];
#endif

typedef struct {
    esp_loader_port_t port;
    handle_t uart;
    TickType_t deadline;
    uint32_t baud;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint8_t rx_tail[32];
    uint8_t rx_tail_length;
} recovery_port_t;

typedef struct {
    const char *name;
    uint32_t offset;
    const uint8_t *begin;
    const uint8_t *end;
} embedded_part_t;

static recovery_port_t s_port;
static bool s_loader_boot_armed;
static uint8_t s_flash_block[ESP_FLASH_BLOCK] __attribute__((aligned(64)));
static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;

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

static void log_init(void)
{
    fpioa_set_function(PIN_DBG_RX, FUNC_UARTHS_RX);
    fpioa_set_function(PIN_DBG_TX, FUNC_UARTHS_TX);
    uarths_init();
    uint32_t clock = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
    uint32_t divider = clock / LOG_BAUD;
    if (divider != 0u)
        divider--;
    if (divider > 0xffffu)
        divider = 0xffffu;
    ((volatile uarths_t *)UARTHS_BASE_ADDR)->div.div = (uint16_t)divider;
}

static void clock_init(void)
{
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_ACLK, 0);
    sysctl_pll_set_freq(SYSCTL_PLL0, 780000000u);
    sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_ACLK, SYSCTL_SOURCE_PLL0);
    sysctl_pll_set_freq(SYSCTL_PLL1, 160000000u);
    sysctl_pll_set_freq(SYSCTL_PLL2, 45158400u);
}

static TickType_t ticks_at_least_one(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks == 0 ? 1 : ticks;
}

static void halt_forever(uint32_t code)
{
    for (;;) {
        log_line("RECOVERY:HALTED code=%lu", (unsigned long)code);
        vTaskDelay(ticks_at_least_one(1000));
    }
}

static void pass_forever(const char *state)
{
    for (;;) {
        log_line("RECOVERY:PASS %s", state);
        vTaskDelay(ticks_at_least_one(1000));
    }
}

static void gpio_write(int gpio_number, int value)
{
    REG_GPIO->direction.u32[0] |= 1u << gpio_number;
    if (value)
        REG_GPIO->data_output.u32[0] |= 1u << gpio_number;
    else
        REG_GPIO->data_output.u32[0] &= ~(1u << gpio_number);
}

static void esp_pins_init(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    fpioa_set_function(PIN_ESP_TX, FUNC_UART2_RX);
    fpioa_set_function(PIN_ESP_RX, FUNC_UART2_TX);
    fpioa_set_function(PIN_ESP_EN, FUNC_GPIO0);
    fpioa_set_function(PIN_ESP_BOOT, FUNC_GPIO3);
    gpio_write(GPIO_ESP_BOOT, 1);
    gpio_write(GPIO_ESP_EN, 1);
}

static esp_loader_error_t port_init(esp_loader_port_t *port)
{
    recovery_port_t *self = container_of(port, recovery_port_t, port);
    esp_pins_init();
    if (s_loader_boot_armed)
        gpio_write(GPIO_ESP_BOOT, 0);
    self->uart = io_open("/dev/uart2");
    if (!self->uart)
        return ESP_LOADER_ERROR_FAIL;
    self->baud = ESP_ROM_BAUD;
    uart_config(self->uart, self->baud, 8, UART_STOP_1, UART_PARITY_NONE);
    return ESP_LOADER_SUCCESS;
}

static void port_deinit(esp_loader_port_t *port)
{
    recovery_port_t *self = container_of(port, recovery_port_t, port);
    if (self->uart) {
        io_close(self->uart);
        self->uart = 0;
    }
}

static void port_enter_bootloader(esp_loader_port_t *port)
{
    (void)port;
    if (s_loader_boot_armed) {
        s_loader_boot_armed = false;
        log_line("RECOVERY:ENTER_ROM using verified mode=(1,x) session");
        return;
    }
    log_line("RECOVERY:ENTER_ROM gpio0=0 en=0");
    gpio_write(GPIO_ESP_BOOT, 0);
    gpio_write(GPIO_ESP_EN, 0);
    vTaskDelay(ticks_at_least_one(100));
    gpio_write(GPIO_ESP_EN, 1);
    vTaskDelay(ticks_at_least_one(100));
    log_line("RECOVERY:ENTER_ROM held gpio0=0 en=1");
}

static void port_reset_target(esp_loader_port_t *port)
{
    (void)port;
    gpio_write(GPIO_ESP_BOOT, 1);
    gpio_write(GPIO_ESP_EN, 0);
    vTaskDelay(ticks_at_least_one(100));
    gpio_write(GPIO_ESP_EN, 1);
    vTaskDelay(ticks_at_least_one(200));
}

static void port_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    recovery_port_t *self = container_of(port, recovery_port_t, port);
    self->deadline = xTaskGetTickCount() + ticks_at_least_one(ms);
}

static uint32_t port_remaining_time(esp_loader_port_t *port)
{
    recovery_port_t *self = container_of(port, recovery_port_t, port);
    int32_t remaining = (int32_t)(self->deadline - xTaskGetTickCount());
    if (remaining <= 0)
        return 0;
    return (uint32_t)remaining * portTICK_PERIOD_MS;
}

static void port_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    vTaskDelay(ticks_at_least_one(ms));
}

static void port_log(esp_loader_port_t *port, esp_loader_log_level_t level,
                     const char *fmt, va_list args)
{
    (void)port;
    (void)level;
    char line[192];
    vsnprintf(line, sizeof(line), fmt, args);
    line[sizeof(line) - 1u] = 0;
    log_line("[loader] %s", line);
}

static esp_loader_error_t port_change_baud(esp_loader_port_t *port, uint32_t baud)
{
    recovery_port_t *self = container_of(port, recovery_port_t, port);
    if (!self->uart)
        return ESP_LOADER_ERROR_FAIL;
    uart_config(self->uart, baud, 8, UART_STOP_1, UART_PARITY_NONE);
    self->baud = baud;
    return ESP_LOADER_SUCCESS;
}

static esp_loader_error_t port_write(esp_loader_port_t *port, const uint8_t *data,
                                     uint16_t size, uint32_t timeout)
{
    (void)timeout;
    recovery_port_t *self = container_of(port, recovery_port_t, port);
    if (!self->uart || (!data && size != 0u))
        return ESP_LOADER_ERROR_FAIL;
    int written = io_write(self->uart, data, size);
    if (written > 0)
        self->tx_bytes += (uint32_t)written;
    return written == size ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

static void record_rx_tail(recovery_port_t *self, const uint8_t *data, uint16_t size)
{
    for (uint16_t i = 0; i < size; ++i) {
        if (self->rx_tail_length < sizeof(self->rx_tail)) {
            self->rx_tail[self->rx_tail_length++] = data[i];
        } else {
            memmove(self->rx_tail, self->rx_tail + 1, sizeof(self->rx_tail) - 1u);
            self->rx_tail[sizeof(self->rx_tail) - 1u] = data[i];
        }
    }
    self->rx_bytes += size;
}

static void log_transport_counters(void)
{
    log_line("RECOVERY:UART tx=%lu rx=%lu tail_len=%u",
             (unsigned long)s_port.tx_bytes, (unsigned long)s_port.rx_bytes,
             (unsigned)s_port.rx_tail_length);
    if (s_port.rx_tail_length != 0u) {
        char line[3u * sizeof(s_port.rx_tail) + 1u];
        size_t used = 0;
        for (uint8_t i = 0; i < s_port.rx_tail_length && used + 3u < sizeof(line); ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%02x ", s_port.rx_tail[i]);
        line[used] = 0;
        log_line("RECOVERY:UART_RX_TAIL %s", line);
    }
}

static esp_loader_error_t port_read(esp_loader_port_t *port, uint8_t *data,
                                    uint16_t size, uint32_t timeout)
{
    recovery_port_t *self = container_of(port, recovery_port_t, port);
    if (!self->uart || (!data && size != 0u))
        return ESP_LOADER_ERROR_FAIL;

    TickType_t deadline = xTaskGetTickCount() + ticks_at_least_one(timeout);
    uint16_t received = 0;
    while (received < size) {
        int count = io_read(self->uart, data + received, size - received);
        if (count > 0) {
            record_rx_tail(self, data + received, (uint16_t)count);
            received = (uint16_t)(received + count);
            continue;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0)
            return ESP_LOADER_ERROR_TIMEOUT;
        taskYIELD();
    }
    return ESP_LOADER_SUCCESS;
}

static const esp_loader_port_ops_t s_port_ops = {
    .init = port_init,
    .deinit = port_deinit,
    .enter_bootloader = port_enter_bootloader,
    .reset_target = port_reset_target,
    .start_timer = port_start_timer,
    .remaining_time = port_remaining_time,
    .delay_ms = port_delay_ms,
    .log = port_log,
    .log_hex = NULL,
    .change_transmission_rate = port_change_baud,
    .write = port_write,
    .read = port_read,
    .spi_set_cs = NULL,
    .sdio_write = NULL,
    .sdio_read = NULL,
    .sdio_card_init = NULL,
};

static bool flash_part(esp_loader_t *loader, const embedded_part_t *part,
                       int index, int total)
{
    uint32_t source_size = (uint32_t)(part->end - part->begin);
    uint32_t image_size = (source_size + 3u) & ~3u;
    esp_loader_flash_cfg_t config = {
        .offset = part->offset,
        .image_size = image_size,
        .block_size = ESP_FLASH_BLOCK,
        .skip_verify = false,
    };

    log_line("RECOVERY:PART %d/%d %s off=0x%06lx size=%lu",
             index + 1, total, part->name,
             (unsigned long)part->offset, (unsigned long)source_size);
    esp_loader_error_t error = esp_loader_flash_start(loader, &config);
    if (error != ESP_LOADER_SUCCESS) {
        log_line("RECOVERY:FAIL flash_start part=%s error=%d", part->name, error);
        return false;
    }

    uint32_t offset = 0;
    while (offset < image_size) {
        uint32_t count = image_size - offset;
        if (count > sizeof(s_flash_block))
            count = sizeof(s_flash_block);
        memset(s_flash_block, 0xff, count);
        if (offset < source_size) {
            uint32_t source_count = source_size - offset;
            if (source_count > count)
                source_count = count;
            memcpy(s_flash_block, part->begin + offset, source_count);
        }
#ifdef RECOVERY_AT_TEST
        if (part->offset == 0u && offset == 0u && count >= 4u) {
            uint8_t old_mode = s_flash_block[2];
            uint8_t old_header = s_flash_block[3];
            s_flash_block[2] = 3u;
            s_flash_block[3] &= 0xf0u;
            log_line("ATTEST:PATCH flash mode=%u->%u freq_header=0x%02x->0x%02x (DOUT/40MHz)",
                     old_mode, s_flash_block[2], old_header, s_flash_block[3]);
        }
#endif

        error = esp_loader_flash_write(loader, &config, s_flash_block, count);
        if (error != ESP_LOADER_SUCCESS) {
            log_line("RECOVERY:FAIL flash_write part=%s offset=%lu error=%d",
                     part->name, (unsigned long)offset, error);
            return false;
        }
        offset += count;
        log_line("RECOVERY:PROGRESS %s %lu/%lu", part->name,
                 (unsigned long)offset, (unsigned long)image_size);
    }

    error = esp_loader_flash_finish(loader, &config);
    if (error != ESP_LOADER_SUCCESS) {
        log_line("RECOVERY:FAIL verify part=%s error=%d", part->name, error);
        return false;
    }
    log_line("RECOVERY:PART_OK %s", part->name);
    return true;
}

static bool wait_for_esp_marker(void)
{
    handle_t uart = io_open("/dev/uart2");
    if (!uart)
        return false;
    uart_config(uart, 115200u, 8, UART_STOP_1, UART_PARITY_NONE);

    const char *marker = ESP_BOOT_MARKER;
    const char *generic = "HSPI_ARMED kesp-v2-physical-";
    size_t matched = 0;
    size_t generic_matched = 0;
    bool generic_seen = false;
    TickType_t deadline = xTaskGetTickCount() + ticks_at_least_one(8000);
    bool found = false;
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t byte;
        int count = io_read(uart, &byte, 1);
        if (count <= 0) {
            taskYIELD();
            continue;
        }
        uarths_write_byte(byte);
        if (!generic_seen) {
            if (byte == (uint8_t)generic[generic_matched]) {
                generic_matched++;
                if (generic[generic_matched] == 0)
                    generic_seen = true;
            } else {
                generic_matched = byte == (uint8_t)generic[0] ? 1u : 0u;
            }
        }
        if (byte == (uint8_t)marker[matched]) {
            matched++;
            if (marker[matched] == 0) {
                found = true;
                break;
            }
        } else {
            matched = byte == (uint8_t)marker[0] ? 1u : 0u;
        }
        if (generic_seen && byte == '\n')
            break;
    }
    io_close(uart);
    return found;
}

static void probe_esp_loader_boot(void)
{
    esp_pins_init();
    handle_t uart = io_open("/dev/uart2");
    if (!uart) {
        log_line("RECOVERY:ROM_PROBE uart-open-failed");
        return;
    }
    uart_config(uart, ESP_BOOT_LOG_BAUD, 8, UART_STOP_1, UART_PARITY_NONE);

    log_line("RECOVERY:ROM_PROBE begin gpio0=0");
    gpio_write(GPIO_ESP_BOOT, 0);
    gpio_write(GPIO_ESP_EN, 0);
    vTaskDelay(ticks_at_least_one(100));
    gpio_write(GPIO_ESP_EN, 1);

    TickType_t deadline = xTaskGetTickCount() + ticks_at_least_one(500);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t byte;
        int count = io_read(uart, &byte, 1);
        if (count > 0)
            uarths_write_byte(byte);
        else
            taskYIELD();
    }
    io_close(uart);
    s_loader_boot_armed = true;
    log_line("\r\nRECOVERY:ROM_PROBE end");
}

static void hold_esp_reset(void)
{
    esp_pins_init();
    gpio_write(GPIO_ESP_BOOT, 1);
    gpio_write(GPIO_ESP_EN, 0);
}

static bool reset_esp_and_wait_marker(void)
{
    esp_pins_init();
    port_reset_target(&s_port.port);
    return wait_for_esp_marker();
}

#ifdef RECOVERY_AT_TEST
static bool at_command(handle_t uart, const char *command, const char *required,
                       uint32_t timeout_ms)
{
    uint8_t drain[64];
    while (io_read(uart, drain, sizeof(drain)) > 0) { }
    log_line("ATTEST:TX %s", command);
    if (io_write(uart, (const uint8_t *)command, strlen(command)) != (int)strlen(command) ||
        io_write(uart, (const uint8_t *)"\r\n", 2) != 2) {
        log_line("ATTEST:FAIL uart-write command=%s", command);
        return false;
    }

    size_t matched = 0;
    const char *error_token = "ERROR";
    size_t error_matched = 0;
    TickType_t deadline = xTaskGetTickCount() + ticks_at_least_one(timeout_ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t byte;
        if (io_read(uart, &byte, 1) <= 0) { taskYIELD(); continue; }
        uarths_write_byte(byte);
        if (byte == (uint8_t)required[matched]) {
            if (required[++matched] == 0) {
                log_line("\r\nATTEST:PASS command=%s token=%s", command, required);
                return true;
            }
        } else {
            matched = byte == (uint8_t)required[0] ? 1u : 0u;
        }
        if (byte == (uint8_t)error_token[error_matched]) {
            if (error_token[++error_matched] == 0) {
                log_line("\r\nATTEST:FAIL command=%s response=ERROR", command);
                return false;
            }
        } else {
            error_matched = byte == (uint8_t)error_token[0] ? 1u : 0u;
        }
    }
    log_line("\r\nATTEST:FAIL timeout command=%s token=%s", command, required);
    return false;
}

static bool run_at_wifi_test(void)
{
    esp_pins_init();
    handle_t uart = io_open("/dev/uart2");
    if (!uart) { log_line("ATTEST:FAIL uart-open"); return false; }
    uart_config(uart, 115200u, 8, UART_STOP_1, UART_PARITY_NONE);
    gpio_write(GPIO_ESP_BOOT, 1);
    gpio_write(GPIO_ESP_EN, 0);
    vTaskDelay(ticks_at_least_one(100));
    gpio_write(GPIO_ESP_EN, 1);
    log_line("ATTEST:BOOT_CAPTURE baud=115200");
    TickType_t boot_deadline = xTaskGetTickCount() + ticks_at_least_one(1200u);
    uint32_t boot_bytes = 0;
    while ((int32_t)(boot_deadline - xTaskGetTickCount()) > 0) {
        uint8_t byte;
        if (io_read(uart, &byte, 1) > 0) { uarths_write_byte(byte); ++boot_bytes; }
        else taskYIELD();
    }
    log_line("\r\nATTEST:BOOT_CAPTURE_DONE bytes=%lu", (unsigned long)boot_bytes);

    const uint32_t bauds[] = { 115200u, 9600u, 57600u, 230400u, 921600u };
    uint32_t at_baud = 0;
    for (unsigned i = 0; i < sizeof(bauds) / sizeof(bauds[0]); ++i) {
        uart_config(uart, bauds[i], 8, UART_STOP_1, UART_PARITY_NONE);
        log_line("ATTEST:PROBE_BAUD %lu", (unsigned long)bauds[i]);
        if (at_command(uart, "AT", "OK", 1500u)) { at_baud = bauds[i]; break; }
    }
    if (!at_baud) { io_close(uart); return false; }
    log_line("ATTEST:BAUD_FOUND %lu", (unsigned long)at_baud);

    bool ok = at_command(uart, "AT+UART_CUR?", "OK", 3000u) &&
              at_command(uart, "AT+GMR", "OK", 3000u) &&
              at_command(uart, "AT+CWMODE=1", "OK", 3000u) &&
              at_command(uart, "AT+CWLAP", "Fermiums_2.4", 15000u) &&
              at_command(uart, "AT+CWJAP=\"Fermiums_2.4\",\"876543212\"", "OK", 25000u) &&
              at_command(uart, "AT+CIFSR", "STAIP", 5000u);
    io_close(uart);
    return ok;
}
#endif

int main(void)
{
    clock_init();
    log_init();
    log_line("RECOVERY:BOOT v2 single-pass rom=%lu flash=%lu block=%lu",
             (unsigned long)ESP_ROM_BAUD,
             (unsigned long)ESP_FLASH_BAUD,
             (unsigned long)ESP_FLASH_BLOCK);

    if (!RECOVERY_FORCE_FLASH_ONCE && reset_esp_and_wait_marker()) {
        log_line("\r\nRECOVERY:ALREADY_OK ESP application marker verified");
        pass_forever("already-current");
    }

    probe_esp_loader_boot();

#ifdef RECOVERY_AT_TEST
    const embedded_part_t parts[] = {
        { "esp8285_at_full", 0x000000u, esp_at_full_start, esp_at_full_end },
    };
#else
    const embedded_part_t parts[] = {
        { "bootloader", 0x000000u, esp_v2_boot_start, esp_v2_boot_end },
        { "partitions", 0x008000u, esp_v2_part_start, esp_v2_part_end },
        { "application", 0x010000u, esp_v2_app_start, esp_v2_app_end },
        { "phy_init", 0x0fc000u, esp_v2_phy_init_start, esp_v2_phy_init_end },
        { "sys_param_blank", 0x0fe000u, esp_v2_sys_param_blank_start, esp_v2_sys_param_blank_end },
    };
#endif

    memset(&s_port, 0, sizeof(s_port));
    s_port.port.ops = &s_port_ops;
    esp_loader_t loader;
    esp_loader_error_t error = esp_loader_init_serial(&loader, &s_port.port);
    if (error != ESP_LOADER_SUCCESS) {
        log_line("RECOVERY:FAIL init error=%d", error);
        hold_esp_reset();
        halt_forever(1u);
    }

    esp_loader_connect_args_t connect = ESP_LOADER_CONNECT_DEFAULT();
    connect.sync_timeout = 250u;
    connect.trials = 2u;
    error = esp_loader_connect_with_stub(&loader, &connect);
    if (error != ESP_LOADER_SUCCESS) {
        log_line("RECOVERY:FAIL stub_connect error=%d", error);
        log_transport_counters();
        esp_loader_deinit(&loader);
        hold_esp_reset();
        halt_forever(2u);
    }
    log_line("RECOVERY:STUB_OK target=%d", (int)esp_loader_get_target(&loader));

    error = esp_loader_change_transmission_rate(&loader, ESP_FLASH_BAUD);
    if (error != ESP_LOADER_SUCCESS) {
        log_line("RECOVERY:FAIL baud_change error=%d", error);
        esp_loader_deinit(&loader);
        hold_esp_reset();
        halt_forever(3u);
    }
    log_line("RECOVERY:BAUD_OK %lu", (unsigned long)ESP_FLASH_BAUD);

    bool ok = true;
    for (int i = 0; i < (int)(sizeof(parts) / sizeof(parts[0])); ++i) {
        if (!flash_part(&loader, &parts[i], i, (int)(sizeof(parts) / sizeof(parts[0])))) {
            ok = false;
            break;
        }
    }
    esp_loader_deinit(&loader);
    if (!ok) {
        hold_esp_reset();
        halt_forever(4u);
    }

#ifdef RECOVERY_AT_TEST
    if (!run_at_wifi_test()) {
        hold_esp_reset();
        halt_forever(6u);
    }
    log_line("ATTEST:ALL_PASS station=Fermiums_2.4");
    pass_forever("at-wifi-confirmed");
#else
    esp_pins_init();
    port_reset_target(&s_port.port);
    if (!wait_for_esp_marker()) {
        log_line("RECOVERY:FAIL boot marker missing: %s", ESP_BOOT_MARKER);
        hold_esp_reset();
        halt_forever(5u);
    }

    log_line("\r\nRECOVERY:PASS ESP application marker verified");
    pass_forever("flashed-and-verified");
#endif
}
