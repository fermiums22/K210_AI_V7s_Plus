#include "esp_flasher.h"
#include "pinout.h"
#include "log.h"
#include "diag_screen.h"

#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <filesystem.h>
#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <sysctl.h>
#include <uart.h>
#include <ff.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <esp_loader.h>
#include <esp_loader_io.h>

#define ESP_FLASH_BAUD       115200u
#define ESP_FLASH_BLOCK      256u

typedef struct {
    esp_loader_port_t port;
    handle_t uart;
    TickType_t deadline_tick;
    uint32_t baud;
} k210_esp_port_t;

static k210_esp_port_t s_port;
static uint8_t s_flash_buf[ESP_FLASH_BLOCK] __attribute__((aligned(64)));
static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;

static void gpio_out_set(int gpio_n, int val)
{
    REG_GPIO->direction.u32[0] |= (1u << gpio_n);
    if (val)
        REG_GPIO->data_output.u32[0] |=  (1u << gpio_n);
    else
        REG_GPIO->data_output.u32[0] &= ~(1u << gpio_n);
}

static void esp_gpio_init(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    fpioa_set_function(PIN_ESP_TX, FUNC_UART2_RX);
    fpioa_set_function(PIN_ESP_RX, FUNC_UART2_TX);
    fpioa_set_function(PIN_ESP_EN, FUNC_GPIO0);
    fpioa_set_function(PIN_ESP_BOOT, FUNC_GPIO3);

    gpio_out_set(GPIO_ESP_BOOT, 1);
    gpio_out_set(GPIO_ESP_EN, 1);
}

static esp_loader_error_t k210_port_init(esp_loader_port_t *port)
{
    k210_esp_port_t *p = container_of(port, k210_esp_port_t, port);

    esp_gpio_init();
    p->uart = io_open("/dev/uart2");
    if (!p->uart)
        return ESP_LOADER_ERROR_FAIL;

    p->baud = ESP_FLASH_BAUD;
    uart_config(p->uart, p->baud, 8, UART_STOP_1, UART_PARITY_NONE);
    return ESP_LOADER_SUCCESS;
}

static void k210_port_deinit(esp_loader_port_t *port)
{
    k210_esp_port_t *p = container_of(port, k210_esp_port_t, port);
    if (p->uart) {
        io_close(p->uart);
        p->uart = 0;
    }
}

static void k210_enter_bootloader(esp_loader_port_t *port)
{
    (void)port;

    gpio_out_set(GPIO_ESP_BOOT, 0);
    gpio_out_set(GPIO_ESP_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_out_set(GPIO_ESP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_out_set(GPIO_ESP_BOOT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void k210_reset_target(esp_loader_port_t *port)
{
    (void)port;

    gpio_out_set(GPIO_ESP_BOOT, 1);
    gpio_out_set(GPIO_ESP_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_out_set(GPIO_ESP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void k210_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    k210_esp_port_t *p = container_of(port, k210_esp_port_t, port);
    uint32_t scaled = ms >= 1000 ? ms * 20u : ms * 4u;
    if (ms == 1000)
        scaled = 30000u;
    if (scaled < ms)
        scaled = ms;
    if (scaled > 120000u)
        scaled = 120000u;
    p->deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(scaled);
}

static uint32_t k210_remaining_time(esp_loader_port_t *port)
{
    k210_esp_port_t *p = container_of(port, k210_esp_port_t, port);
    TickType_t now = xTaskGetTickCount();
    int32_t ticks_left = (int32_t)(p->deadline_tick - now);
    if (ticks_left <= 0)
        return 0;
    return (uint32_t)(ticks_left * portTICK_PERIOD_MS);
}

static void k210_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void k210_log(esp_loader_port_t *port, esp_loader_log_level_t level, const char *fmt, va_list args)
{
    (void)port;
    (void)level;

    char buf[192];
    vsnprintf(buf, sizeof(buf), fmt, args);
    buf[sizeof(buf) - 1] = 0;
    LOGF("[esp-flash] %s", buf);
}

static esp_loader_error_t k210_change_baud(esp_loader_port_t *port, uint32_t rate)
{
    k210_esp_port_t *p = container_of(port, k210_esp_port_t, port);
    if (!p->uart)
        return ESP_LOADER_ERROR_FAIL;
    p->baud = rate;
    uart_config(p->uart, rate, 8, UART_STOP_1, UART_PARITY_NONE);
    return ESP_LOADER_SUCCESS;
}

static esp_loader_error_t k210_write(esp_loader_port_t *port, const uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)timeout;
    k210_esp_port_t *p = container_of(port, k210_esp_port_t, port);
    if (!p->uart)
        return ESP_LOADER_ERROR_FAIL;

    int wr = io_write(p->uart, data, size);
    return wr == size ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

static esp_loader_error_t k210_read(esp_loader_port_t *port, uint8_t *data, uint16_t size, uint32_t timeout)
{
    k210_esp_port_t *p = container_of(port, k210_esp_port_t, port);
    if (!p->uart)
        return ESP_LOADER_ERROR_FAIL;

    uint16_t got = 0;
    uint32_t waited = 0;
    while (got < size && waited < timeout) {
        int r = io_read(p->uart, data + got, size - got);
        if (r > 0) {
            got += (uint16_t)r;
            waited = 0;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
            waited++;
        }
    }

    return got == size ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_TIMEOUT;
}

static const esp_loader_port_ops_t k210_esp_ops = {
    .init                     = k210_port_init,
    .deinit                   = k210_port_deinit,
    .enter_bootloader         = k210_enter_bootloader,
    .reset_target             = k210_reset_target,
    .start_timer              = k210_start_timer,
    .remaining_time           = k210_remaining_time,
    .delay_ms                 = k210_delay_ms,
    .log                      = k210_log,
    .log_hex                  = NULL,
    .change_transmission_rate = k210_change_baud,
    .write                    = k210_write,
    .read                     = k210_read,
    .spi_set_cs               = NULL,
    .sdio_write               = NULL,
    .sdio_read                = NULL,
    .sdio_card_init           = NULL,
};

static void esp_reset_normal_boot(void)
{
    esp_gpio_init();
    k210_reset_target(&s_port.port);
}

static const char *target_name(target_chip_t chip)
{
    switch (chip) {
    case ESP8266_CHIP: return "ESP8266/ESP8285";
    case ESP32_CHIP: return "ESP32";
    default: return "ESP-family";
    }
}

bool esp_flash_file(const char *path, uint32_t offset)
{
    if (!path)
        return false;

    handle_t f = filesystem_file_open(path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f) {
        LOGF("[esp-flash] image missing: %s", path);
        diag_line(7, "ESP image missing");
        return false;
    }

    uint64_t file_size64 = filesystem_file_get_size(f);
    if (file_size64 == 0 || file_size64 > 0x200000u) {
        LOGF("[esp-flash] bad image size: %lu", (unsigned long)file_size64);
        diag_printf(7, "ESP bad size: %lu", (unsigned long)file_size64);
        filesystem_file_close(f);
        return false;
    }

    uint32_t file_size = (uint32_t)file_size64;
    uint32_t image_size = (file_size + 3u) & ~3u;
    LOGF("[esp-flash] flashing %s size=%lu offset=0x%08lx",
         path, (unsigned long)file_size, (unsigned long)offset);
    diag_line(6, "ESP flash: start");
    diag_printf(7, "Image %lu bytes", (unsigned long)file_size);

    memset(&s_port, 0, sizeof(s_port));
    s_port.port.ops = &k210_esp_ops;

    esp_loader_t loader;
    esp_loader_error_t err = esp_loader_init_serial(&loader, &s_port.port);
    if (err != ESP_LOADER_SUCCESS) {
        LOGF("[esp-flash] init failed: %d", err);
        diag_printf(8, "ESP init failed %d", err);
        filesystem_file_close(f);
        return false;
    }

    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    args.sync_timeout = 250;
    args.trials = 12;
    err = esp_loader_connect_with_stub(&loader, &args);
    if (err != ESP_LOADER_SUCCESS) {
        LOGF("[esp-flash] stub connect failed: %d, trying ROM", err);
        diag_printf(8, "Stub fail %d, ROM", err);
        err = esp_loader_connect(&loader, &args);
    }
    if (err != ESP_LOADER_SUCCESS) {
        LOGF("[esp-flash] connect failed: %d", err);
        diag_printf(8, "ESP connect failed %d", err);
        esp_reset_normal_boot();
        esp_loader_deinit(&loader);
        filesystem_file_close(f);
        return false;
    }

    target_chip_t chip = esp_loader_get_target(&loader);
    LOGF("[esp-flash] connected target=%s", target_name(chip));
    diag_printf(8, "ESP target: %s", target_name(chip));

    esp_loader_flash_cfg_t cfg = {
        .offset = offset,
        .image_size = image_size,
        .block_size = sizeof(s_flash_buf),
        .skip_verify = true,
    };

    err = esp_loader_flash_start(&loader, &cfg);
    if (err != ESP_LOADER_SUCCESS) {
        LOGF("[esp-flash] flash_start failed: %d", err);
        diag_printf(9, "Erase/start failed %d", err);
        esp_reset_normal_boot();
        esp_loader_deinit(&loader);
        filesystem_file_close(f);
        return false;
    }

    uint32_t sent = 0;
    while (sent < image_size) {
        uint32_t chunk = image_size - sent;
        if (chunk > sizeof(s_flash_buf))
            chunk = sizeof(s_flash_buf);

        memset(s_flash_buf, 0xff, chunk);
        if (sent < file_size) {
            uint32_t want = file_size - sent;
            if (want > chunk)
                want = chunk;
            int got = filesystem_file_read(f, s_flash_buf, want);
            if (got != (int)want) {
                LOGF("[esp-flash] read failed at %lu", (unsigned long)sent);
                diag_printf(9, "Read fail at %lu", (unsigned long)sent);
                esp_reset_normal_boot();
                esp_loader_deinit(&loader);
                filesystem_file_close(f);
                return false;
            }
        }

        err = esp_loader_flash_write(&loader, &cfg, s_flash_buf, chunk);
        if (err != ESP_LOADER_SUCCESS) {
            LOGF("[esp-flash] write failed at %lu: %d", (unsigned long)sent, err);
            diag_printf(9, "Write fail %lu e%d", (unsigned long)sent, err);
            esp_reset_normal_boot();
            esp_loader_deinit(&loader);
            filesystem_file_close(f);
            return false;
        }

        sent += chunk;
        if ((sent % (32u * 1024u)) == 0 || sent == image_size) {
            uint32_t pct = (sent * 100u) / image_size;
            LOGF("[esp-flash] progress %lu/%lu (%lu%%)",
                 (unsigned long)sent, (unsigned long)image_size, (unsigned long)pct);
            diag_printf(9, "ESP write %lu/%lu %lu%%",
                        (unsigned long)sent, (unsigned long)image_size, (unsigned long)pct);
        }
    }

    err = esp_loader_flash_finish(&loader, &cfg);
    esp_loader_deinit(&loader);
    filesystem_file_close(f);

    if (err != ESP_LOADER_SUCCESS) {
        LOGF("[esp-flash] finish failed: %d", err);
        diag_printf(10, "ESP finish failed %d", err);
        esp_reset_normal_boot();
        return false;
    }

    LOG("[esp-flash] done, resetting ESP to normal boot");
    diag_line(10, "ESP flash: done");
    esp_reset_normal_boot();
    return true;
}

bool esp_flash_marker_present(void)
{
    handle_t marker = filesystem_file_open(ESP_FLASH_DEFAULT_MARKER, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!marker)
        return false;
    filesystem_file_close(marker);
    return true;
}

bool esp_flash_run_if_requested(void)
{
    if (!esp_flash_marker_present())
        return false;
    LOG("[esp-flash] marker found");
    diag_line(6, "ESP marker: flash_now");
    bool ok = esp_flash_file(ESP_FLASH_DEFAULT_IMAGE, ESP_FLASH_DEFAULT_OFFSET);
    if (ok) {
        const char *paths[] = {
            "0:/flash_now",
            "0:flash_now",
            "/flash_now",
            "flash_now",
        };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            FRESULT fr = f_unlink(paths[i]);
            LOGF("[esp-flash] unlink %s -> %d", paths[i], fr);
            if (fr == FR_OK)
                break;
        }
    }
    diag_line(11, ok ? "ESP flash result: OK" : "ESP flash result: FAIL");
    return ok;
}
