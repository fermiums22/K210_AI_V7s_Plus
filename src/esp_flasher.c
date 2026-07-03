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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_loader.h>
#include <esp_loader_io.h>

#define ESP_FLASH_BAUD       115200u
#define ESP_FLASH_BLOCK      64u

typedef struct {
    esp_loader_port_t port;
    handle_t uart;
    TickType_t deadline_tick;
    uint32_t baud;
} k210_esp_port_t;

static k210_esp_port_t s_port;
static uint8_t s_flash_buf[ESP_FLASH_BLOCK] __attribute__((aligned(64)));
static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;

typedef struct {
    char path[96];
    uint32_t offset;
} flash_part_t;

typedef struct {
    bool enabled;
    int part_count;
    flash_part_t parts[8];
} esp_flash_job_t;

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
        .skip_verify = false,
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
        vTaskDelay(pdMS_TO_TICKS(1));

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

static bool file_present(const char *path)
{
    handle_t f = filesystem_file_open(path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f)
        return false;
    filesystem_file_close(f);
    return true;
}

static char *find_key(char *start, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return strstr(start, needle);
}

static bool json_bool_or_int(char *section, const char *key)
{
    char *p = find_key(section, key);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return *p == '1' || strncmp(p, "true", 4) == 0;
}

static bool json_string(char *section, const char *key, char *out, size_t out_size)
{
    char *p = find_key(section, key);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p && *p != '"')
        p++;
    if (*p != '"')
        return false;
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size)
        out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

static bool json_u32(char *section, const char *key, uint32_t *out)
{
    char tmp[24];
    char *p = find_key(section, key);
    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"' || *p == '\r' || *p == '\n')
        p++;

    size_t n = 0;
    while (p[n] && p[n] != '"' && p[n] != ',' && p[n] != '}' &&
           p[n] != '\r' && p[n] != '\n' && n + 1 < sizeof(tmp)) {
        tmp[n] = p[n];
        n++;
    }
    tmp[n] = 0;
    *out = (uint32_t)strtoul(tmp, NULL, 0);
    return n > 0;
}

static char *json_object_end(char *section)
{
    char *p = strchr(section, '{');
    if (!p)
        return NULL;

    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (; *p; p++) {
        if (esc) {
            esc = false;
            continue;
        }
        if (*p == '\\' && in_str) {
            esc = true;
            continue;
        }
        if (*p == '"') {
            in_str = !in_str;
            continue;
        }
        if (in_str)
            continue;
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0)
                return p;
        }
    }
    return NULL;
}

static bool make_fs_path(const char *name, char *out, size_t out_size)
{
    if (!name[0] || name[0] == '/' || name[0] == '\\' || strstr(name, ".."))
        return false;
    snprintf(out, out_size, "/fs/0/%s", name);
    return true;
}

static bool flash_config_read(char *buf, size_t buf_size)
{
    handle_t f = filesystem_file_open(ESP_FLASH_CONFIG_PATH, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f)
        return false;
    uint64_t size = filesystem_file_get_size(f);
    if (size == 0 || size >= buf_size) {
        filesystem_file_close(f);
        return false;
    }
    int got = filesystem_file_read(f, (uint8_t *)buf, (size_t)size);
    filesystem_file_close(f);
    if (got != (int)size)
        return false;
    buf[got] = 0;
    return true;
}

static void flash_config_disarm(char *json)
{
    char *once = find_key(json, "flash_once");
    if (!once)
        return;
    char *end = json_object_end(once);
    if (!end)
        return;
    for (char *p = once; (p = find_key(p, "enabled")) != NULL && p < end; p++) {
        char *v = strchr(p, ':');
        if (!v)
            break;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
            v++;
        if (*v == '1')
            *v = '0';
    }

    handle_t f = filesystem_file_open(ESP_FLASH_CONFIG_PATH, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
    if (!f) {
        LOG("[esp-flash] config disarm open failed");
        return;
    }
    filesystem_file_write(f, (const uint8_t *)json, strlen(json));
    filesystem_file_close(f);
    LOG("[esp-flash] config disarmed");
}

static bool flash_config_parse_esp(char *json, esp_flash_job_t *job)
{
    memset(job, 0, sizeof(*job));

    char *once = find_key(json, "flash_once");
    if (!once || !json_bool_or_int(once, "enabled"))
        return false;

    char *esp = find_key(once, "esp");
    if (!esp || !json_bool_or_int(esp, "enabled"))
        return false;

    job->enabled = true;

    char *stm32 = find_key(esp, "stm32");
    char *part = find_key(esp, "parts");
    while (part && job->part_count < (int)(sizeof(job->parts) / sizeof(job->parts[0]))) {
        part = find_key(part + 1, "file");
        if (!part)
            break;
        if (stm32 && part > stm32)
            break;

        char rel[80];
        uint32_t offset = 0;
        if (!json_string(part, "file", rel, sizeof(rel)) ||
            !json_u32(part, "offset", &offset) ||
            !make_fs_path(rel, job->parts[job->part_count].path,
                          sizeof(job->parts[job->part_count].path))) {
            LOG("[esp-flash] bad ESP part in config");
            break;
        }

        job->parts[job->part_count].offset = offset;
        job->part_count++;
        part++;
    }

    if (job->part_count == 0) {
        if (file_present(ESP_FLASH_RTOS_BOOT) && file_present(ESP_FLASH_RTOS_IROM)) {
            strcpy(job->parts[0].path, ESP_FLASH_RTOS_BOOT);
            job->parts[0].offset = ESP_FLASH_RTOS_BOOT_OFF;
            strcpy(job->parts[1].path, ESP_FLASH_RTOS_IROM);
            job->parts[1].offset = ESP_FLASH_RTOS_IROM_OFF;
            job->part_count = 2;
            if (file_present(ESP_FLASH_RTOS_INIT) && job->part_count < 4) {
                strcpy(job->parts[job->part_count].path, ESP_FLASH_RTOS_INIT);
                job->parts[job->part_count].offset = ESP_FLASH_RTOS_INIT_OFF;
                job->part_count++;
            }
            if (file_present(ESP_FLASH_RTOS_BLANK) && job->part_count < 4) {
                strcpy(job->parts[job->part_count].path, ESP_FLASH_RTOS_BLANK);
                job->parts[job->part_count].offset = ESP_FLASH_RTOS_BLANK_OFF;
                job->part_count++;
            }
        } else {
            strcpy(job->parts[0].path, ESP_FLASH_DEFAULT_IMAGE);
            job->parts[0].offset = ESP_FLASH_DEFAULT_OFFSET;
            job->part_count = 1;
        }
    }

    return job->enabled && job->part_count > 0;
}

static bool flash_config_parse_esp_fallback(char *json, esp_flash_job_t *job)
{
    memset(job, 0, sizeof(*job));
    if (!strstr(json, "\"enabled\"") || !strstr(json, ": 1"))
        return false;
    if (!file_present(ESP_FLASH_RTOS_BOOT) || !file_present(ESP_FLASH_RTOS_IROM))
        return false;

    job->enabled = true;
    strcpy(job->parts[0].path, ESP_FLASH_RTOS_BOOT);
    job->parts[0].offset = ESP_FLASH_RTOS_BOOT_OFF;
    strcpy(job->parts[1].path, ESP_FLASH_RTOS_IROM);
    job->parts[1].offset = ESP_FLASH_RTOS_IROM_OFF;
    job->part_count = 2;
    if (file_present(ESP_FLASH_RTOS_INIT)) {
        strcpy(job->parts[job->part_count].path, ESP_FLASH_RTOS_INIT);
        job->parts[job->part_count].offset = ESP_FLASH_RTOS_INIT_OFF;
        job->part_count++;
    }
    if (file_present(ESP_FLASH_RTOS_BLANK)) {
        strcpy(job->parts[job->part_count].path, ESP_FLASH_RTOS_BLANK);
        job->parts[job->part_count].offset = ESP_FLASH_RTOS_BLANK_OFF;
        job->part_count++;
    }
    LOG("[esp-flash] using fallback RTOS parts");
    return true;
}

bool esp_flash_run_if_requested(void)
{
    char json[1536];
    esp_flash_job_t job;
    if (!flash_config_read(json, sizeof(json)))
        return false;
    if (!flash_config_parse_esp(json, &job) &&
        !flash_config_parse_esp_fallback(json, &job))
        return false;

    LOG("[esp-flash] config flash_once ESP job found");
    diag_line(6, "ESP config job found");
    flash_config_disarm(json);

    bool ok = true;
    for (int i = 0; i < job.part_count; i++) {
        LOGF("[esp-flash] part %d: %s @ 0x%08lx", i,
             job.parts[i].path, (unsigned long)job.parts[i].offset);
        diag_printf(7, "ESP part %d/%d", i + 1, job.part_count);
        if (!esp_flash_file(job.parts[i].path, job.parts[i].offset)) {
            ok = false;
            break;
        }
    }
    diag_line(11, ok ? "ESP flash result: OK" : "ESP flash result: FAIL");
    return ok;
}
