#include "sd_uart.h"
#include "sd.h"
#include "log.h"
#include "diag_screen.h"
#include "camera.h"
#include "amp.h"
#include "lcd.h"
#include "esp_flasher.h"
#include "esp_spi_link.h"
#include "esp_uart_log.h"

#include <FreeRTOS.h>
#include <task.h>
#include <filesystem.h>
#include <ff.h>
#include <platform.h>
#include <sysctl.h>
#include <uarths.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the host sync word independent from CR/LF handling. The PC helper sends
 * KSD1\n, but accepting KSD1 first makes the persistent command service robust
 * even if the newline is delayed, stripped, or consumed by a previous drain. */
#define UART_SD_MAGIC "KSD1"
#define UART_SD_BUF   512
#define UARTHS_RXDATA_EMPTY_MASK (1u << 31)
#define UART_SD_CMD_TIMEOUT_MS 2000u
#define UART_SD_DATA_TIMEOUT_MS 15000u
#define UART_SD_EMPTY_SPINS_BEFORE_YIELD 2048u
#define CAM_CAPTURE_DEFAULT_PATH "cam/capture.rgb565"

static volatile uarths_t *const REG_UARTHS = (volatile uarths_t *)UARTHS_BASE_ADDR;
static uint8_t rx_buf[UART_SD_BUF] __attribute__((aligned(64)));
static volatile int s_service_started;

static TickType_t ms_to_ticks_min(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0)
        ticks = 1;
    return ticks;
}

static void poll_delay(void)
{
    vTaskDelay(1);
}

static int deadline_expired(TickType_t start, uint32_t timeout_ms)
{
    TickType_t timeout = ms_to_ticks_min(timeout_ms);
    return (xTaskGetTickCount() - start) >= timeout;
}

static int uarths_try_read_byte(uint8_t *out)
{
    uint32_t raw = *(volatile uint32_t *)&REG_UARTHS->rxdata;
    if (raw & UARTHS_RXDATA_EMPTY_MASK)
        return 0;
    *out = (uint8_t)(raw & 0xffu);
    return 1;
}

static void host_puts(const char *s)
{
    uarths_puts(s);
}

static void host_write(const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
        uarths_write_byte(data[i]);
}

static void append_hex_byte(char *dst, size_t dst_size, uint8_t c)
{
    size_t len = strlen(dst);
    if (len + 4 >= dst_size)
        return;
    snprintf(dst + len, dst_size - len, "%02lX ", (unsigned long)c);
}

static void uart_rx_yield_if_idle(uint32_t *empty_spins)
{
    (*empty_spins)++;
    if (*empty_spins >= UART_SD_EMPTY_SPINS_BEFORE_YIELD) {
        *empty_spins = 0;
        taskYIELD();
    }
}

static int read_byte_timeout(uint8_t *out, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    uint32_t empty_spins = 0;
    while (!deadline_expired(start, timeout_ms)) {
        if (uarths_try_read_byte(out))
            return 1;
        uart_rx_yield_if_idle(&empty_spins);
    }
    return 0;
}

static int read_line(char *out, int out_len, uint32_t timeout_ms)
{
    int n = 0;
    int bytes = 0;
    char hex[96];
    uint8_t c;
    hex[0] = 0;
    while (n < out_len - 1) {
        if (!read_byte_timeout(&c, timeout_ms)) {
            out[n] = 0;
            LOGF("[sd-uart] command RX timeout: chars=%d bytes=%d hex=%s text=%s", n, bytes, hex, out);
            return 0;
        }
        bytes++;
        if (bytes <= 24)
            append_hex_byte(hex, sizeof(hex), c);
        if (c == '\n') {
            out[n] = 0;
            LOGF("[sd-uart] command RX OK: chars=%d bytes=%d hex=%s text=%s", n, bytes, hex, out);
            return 1;
        }
        if (c != '\r')
            out[n++] = (char)c;
    }
    out[n] = 0;
    LOGF("[sd-uart] command RX too long: chars=%d bytes=%d hex=%s text=%s", n, bytes, hex, out);
    return 0;
}

static int wait_magic(uint32_t window_ms)
{
    const char *magic = UART_SD_MAGIC;
    int mi = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t next_ready = start;
    uint8_t c;
    while (!deadline_expired(start, window_ms)) {
        if (!uarths_try_read_byte(&c)) {
            TickType_t now = xTaskGetTickCount();
            if ((now - next_ready) >= ms_to_ticks_min(1000)) {
                host_puts("KSD:READY\n");
                next_ready = now;
            }
            poll_delay();
            continue;
        }
        if (c == (uint8_t)magic[mi]) {
            mi++;
            if (magic[mi] == 0) {
                LOG("[sd-uart] magic matched");
                return 1;
            }
        } else {
            mi = (c == (uint8_t)magic[0]) ? 1 : 0;
        }
    }
    return 0;
}

static void drain_rx(uint32_t quiet_ms)
{
    TickType_t quiet_start = xTaskGetTickCount();
    uint8_t c;
    while (!deadline_expired(quiet_start, quiet_ms)) {
        if (uarths_try_read_byte(&c)) {
            quiet_start = xTaskGetTickCount();
            continue;
        }
        poll_delay();
    }
}

static int safe_rel_path(const char *s)
{
    if (!s[0] || s[0] == '/' || s[0] == '\\')
        return 0;
    if (strstr(s, ".."))
        return 0;
    return 1;
}

static void make_parent_dirs(const char *rel_path)
{
    char path[160] = "0:/";
    int p = 3;
    for (const char *s = rel_path; *s && p < (int)sizeof(path) - 2; s++) {
        char c = *s == '\\' ? '/' : *s;
        if (c == '/') {
            path[p] = 0;
            if (p > 3)
                f_mkdir(path);
        }
        path[p++] = c;
    }
}

static bool make_fs_path(const char *rel_path, char *fs_path, size_t fs_size,
                         char *fat_path, size_t fat_size)
{
    if (!safe_rel_path(rel_path))
        return false;
    snprintf(fs_path, fs_size, "/fs/0/%s", rel_path);
    if (fat_path && fat_size)
        snprintf(fat_path, fat_size, "0:/%s", rel_path);
    return true;
}

static bool receive_file(const char *rel_path, uint32_t size)
{
    char path[160];
    char fat_path[160];
    if (!make_fs_path(rel_path, path, sizeof(path), fat_path, sizeof(fat_path))) {
        LOGF("[sd-uart] bad path: %s", rel_path);
        diag_printf(5, "UART bad path: %.24s", rel_path);
        host_puts("KSD:ERR bad-path\n");
        return false;
    }
    if (!sd_mount()) {
        LOG("[sd-uart] PUT SD mount failed");
        diag_line(5, "PUT SD mount fail");
        host_puts("KSD:ERR sd\n");
        return false;
    }
    make_parent_dirs(rel_path);
    f_unlink(fat_path);
    handle_t f = filesystem_file_open(path, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
    if (!f) {
        LOGF("[sd-uart] open failed: %s", path);
        diag_printf(5, "UART open failed");
        host_puts("KSD:ERR open\n");
        return false;
    }
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "KSD:GO %lu\n", (unsigned long)sizeof(rx_buf));
    host_puts(hdr);
    host_puts("KSD:READYDATA\n");
    uint32_t got = 0;
    while (got < size) {
        uint32_t chunk = size - got;
        if (chunk > sizeof(rx_buf))
            chunk = sizeof(rx_buf);
        for (uint32_t i = 0; i < chunk; i++) {
            if (!read_byte_timeout(&rx_buf[i], UART_SD_DATA_TIMEOUT_MS)) {
                filesystem_file_close(f);
                LOGF("[sd-uart] short file: %s %lu+%lu/%lu", rel_path, (unsigned long)got, (unsigned long)i, (unsigned long)size);
                diag_printf(5, "UART short %lu/%lu", (unsigned long)(got + i), (unsigned long)size);
                host_puts("KSD:ERR short\n");
                return false;
            }
        }
        int wr = filesystem_file_write(f, rx_buf, chunk);
        if (wr != (int)chunk) {
            filesystem_file_close(f);
            LOGF("[sd-uart] write failed: %s", rel_path);
            diag_printf(5, "SD write failed");
            host_puts("KSD:ERR write\n");
            return false;
        }
        got += chunk;
        if ((got % (32u * 1024u)) == 0 || got == size)
            diag_printf(5, "UART %lu/%lu", (unsigned long)got, (unsigned long)size);
        host_puts("KSD:B\n");
    }
    filesystem_file_close(f);
    host_puts("KSD:OK\n");
    LOGF("[sd-uart] received %s %lu", rel_path, (unsigned long)size);
    return true;
}

static bool send_file_raw(const char *rel_path)
{
    char path[160];
    if (!make_fs_path(rel_path, path, sizeof(path), NULL, 0)) {
        LOGF("[sd-uart] bad get path: %s", rel_path);
        host_puts("KSD:ERR bad-path\n");
        return false;
    }
    handle_t f = filesystem_file_open(path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f) {
        LOGF("[sd-uart] get missing: %s", path);
        host_puts("KSD:MISSING\n");
        return true;
    }
    uint64_t size64 = filesystem_file_get_size(f);
    if (size64 > 0xffffffffu) {
        filesystem_file_close(f);
        host_puts("KSD:ERR too-large\n");
        return false;
    }
    uint32_t size = (uint32_t)size64;
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "KSD:SIZE %lu\n", (unsigned long)size);
    host_puts(hdr);
    uint32_t sent = 0;
    while (sent < size) {
        uint32_t chunk = size - sent;
        if (chunk > sizeof(rx_buf))
            chunk = sizeof(rx_buf);
        int got = filesystem_file_read(f, rx_buf, chunk);
        if (got <= 0) {
            filesystem_file_close(f);
            host_puts("KSD:ERR read\n");
            return false;
        }
        host_write(rx_buf, (uint32_t)got);
        sent += (uint32_t)got;
    }
    filesystem_file_close(f);
    host_puts("KSD:OK\n");
    LOGF("[sd-uart] sent %s %lu", rel_path, (unsigned long)size);
    return true;
}

static bool send_file_quiet(const char *rel_path)
{
    int restart_uart = esp_uart_log_is_started();
    esp_spi_link_pause(1);
    if (restart_uart) {
        esp_uart_log_stop();
        vTaskDelay(ms_to_ticks_min(150));
    }
    bool ok = send_file_raw(rel_path);
    if (restart_uart)
        esp_uart_log_start();
    esp_spi_link_pause(0);
    return ok;
}

static void test_line(const char *name, const char *status, const char *detail)
{
    char b[192];
    snprintf(b, sizeof(b), "KSD:TEST %s %s %s\n", name, status, detail ? detail : "");
    host_puts(b);
}

static bool sd_rw_probe(void)
{
    if (!sd_mount())
        return false;
    const char *fs_path = "/fs/0/selftest_probe.bin";
    const char *fat_path = "0:/selftest_probe.bin";
    uint8_t wr[64];
    uint8_t rd[64];
    for (uint32_t i = 0; i < sizeof(wr); i++)
        wr[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
    memset(rd, 0, sizeof(rd));
    f_unlink(fat_path);
    handle_t f = filesystem_file_open(fs_path, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
    if (!f)
        return false;
    int n = filesystem_file_write(f, wr, sizeof(wr));
    filesystem_file_close(f);
    if (n != (int)sizeof(wr))
        return false;
    f = filesystem_file_open(fs_path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f)
        return false;
    n = filesystem_file_read(f, rd, sizeof(rd));
    filesystem_file_close(f);
    if (n != (int)sizeof(rd))
        return false;
    return memcmp(wr, rd, sizeof(wr)) == 0;
}

static bool camera_ram_probe(char *detail, size_t detail_size)
{
    const uint16_t *pixels = NULL;
    int w = 0;
    int h = 0;
    if (!cam_capture_rgb565(&pixels, &w, &h) || !pixels || w <= 0 || h <= 0) {
        snprintf(detail, detail_size, "capture");
        return false;
    }
    uint32_t total = (uint32_t)w * (uint32_t)h * 2u;
    snprintf(detail, detail_size, "%dx%d-%lu-RGB565", w, h, (unsigned long)total);
    return w == 640 && h == 480 && total == 614400u;
}

static bool write_camera_capture_file(const char *rel_path)
{
    char path[160];
    char fat_path[160];
    if (!make_fs_path(rel_path, path, sizeof(path), fat_path, sizeof(fat_path))) {
        LOGF("[sd-uart] CAM_CAPTURE bad path: %s", rel_path);
        diag_printf(6, "CAM bad path: %.24s", rel_path);
        host_puts("KSD:CAPTURE_FAIL bad-path\n");
        return false;
    }
    host_puts("KSD:CAPTURING\n");
    LOGF("[sd-uart] CAM_CAPTURE requested: %s", rel_path);
    diag_line(6, "CAM_CAPTURE requested");
    if (!sd_mount()) {
        LOG("[sd-uart] CAM_CAPTURE SD mount failed");
        diag_line(6, "CAM_CAPTURE SD fail");
        host_puts("KSD:CAPTURE_FAIL sd\n");
        return false;
    }
    const uint16_t *pixels = NULL;
    int w = 0;
    int h = 0;
    if (!cam_capture_rgb565(&pixels, &w, &h) || !pixels || w <= 0 || h <= 0) {
        LOG("[sd-uart] CAM_CAPTURE camera failed");
        diag_line(6, "CAM_CAPTURE camera fail");
        host_puts("KSD:CAPTURE_FAIL camera\n");
        return false;
    }
    uint32_t total = (uint32_t)w * (uint32_t)h * 2u;
    make_parent_dirs(rel_path);
    f_unlink(fat_path);
    handle_t f = filesystem_file_open(path, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
    if (!f) {
        LOGF("[sd-uart] CAM_CAPTURE open failed: %s", path);
        diag_line(6, "CAM_CAPTURE open fail");
        host_puts("KSD:CAPTURE_FAIL open\n");
        return false;
    }
    const uint8_t *src = (const uint8_t *)pixels;
    uint32_t written = 0;
    while (written < total) {
        uint32_t chunk = total - written;
        if (chunk > sizeof(rx_buf))
            chunk = sizeof(rx_buf);
        memcpy(rx_buf, src + written, chunk);
        int wr = filesystem_file_write(f, rx_buf, chunk);
        if (wr != (int)chunk) {
            filesystem_file_close(f);
            LOGF("[sd-uart] CAM_CAPTURE write failed: %lu/%lu", (unsigned long)written, (unsigned long)total);
            diag_line(6, "CAM_CAPTURE write fail");
            host_puts("KSD:CAPTURE_FAIL write\n");
            return false;
        }
        written += chunk;
    }
    filesystem_file_close(f);
    char hdr[192];
    snprintf(hdr, sizeof(hdr), "KSD:CAPTURE_OK %s %lu %d %d RGB565\n", rel_path, (unsigned long)total, w, h);
    host_puts(hdr);
    LOGF("[sd-uart] CAM_CAPTURE OK: %s %lu bytes %dx%d RGB565", rel_path, (unsigned long)total, w, h);
    diag_printf(6, "CAM OK %dx%d %lu", w, h, (unsigned long)total);
    return true;
}

static bool run_cam_capture_command(const char *rel_path)
{
    int restart_uart = esp_uart_log_is_started();
    esp_spi_link_pause(1);
    if (restart_uart) {
        esp_uart_log_stop();
        vTaskDelay(ms_to_ticks_min(150));
    }
    bool ok = write_camera_capture_file(rel_path);
    if (restart_uart)
        esp_uart_log_start();
    esp_spi_link_pause(0);
    return ok;
}

static void board_reset(void)
{
    LOG("[sd-uart] host requested SOC reset");
    diag_line(6, "UART reset requested");
    host_puts("KSD:RESETTING\n");
    vTaskDelay(ms_to_ticks_min(200));
    sysctl_reset(SYSCTL_RESET_SOC);
    while (1)
        vTaskDelay(ms_to_ticks_min(1000));
}

static bool run_esp_flash_command(void)
{
    host_puts("KSD:FLASHING\n");
    LOG("[sd-uart] host requested ESP flash");
    diag_line(6, "UART ESP flash requested");
    esp_spi_link_pause(1);
    esp_uart_log_stop();
    vTaskDelay(ms_to_ticks_min(150));
    bool ok = esp_flash_run_if_requested();
    if (ok)
        esp_uart_log_start();
    esp_spi_link_pause(0);
    host_puts(ok ? "KSD:FLASH_OK\n" : "KSD:FLASH_FAIL\n");
    return ok;
}

static void run_spi_command(void)
{
    LOG("[sd-uart] host requested ESP UART/SPI run");
    diag_line(6, "UART RUN_SPI requested");
    esp_uart_log_start();
    esp_spi_link_start();
    host_puts("KSD:RUNSPI\n");
}

static void format_sd_command(void)
{
    host_puts("KSD:FORMATTING\n");
    LOG("[sd-uart] host requested FORMAT_SD");
    diag_line(6, "UART FORMAT_SD requested");
    esp_spi_link_pause(1);
    bool ok = sd_format();
    esp_spi_link_pause(0);
    if (ok) {
        diag_line(6, "FORMAT_SD OK");
        host_puts("KSD:FORMAT_OK\n");
    } else {
        diag_line(6, "FORMAT_SD FAIL");
        host_puts("KSD:FORMAT_FAIL\n");
    }
}

static void help_command(void)
{
    host_puts("KSD:HELP HELP SELFTEST LCD_TEST AMP_TEST SD_TEST CAM_TEST MIC_TEST\n");
    host_puts("KSD:HELP CAM_CAPTURE [path] GET <path> PUT <path> <size> FORMAT_SD FLASH_ESP RUN_SPI RESET DONE\n");
    host_puts("KSD:HELP_END\n");
}

static void lcd_test_command(void)
{
    LOG("[sd-uart] host requested LCD_TEST");
    lcd_clear(BLUE);
    lcd_draw_string_bg(0, 0, "KSD LCD TEST", WHITE, BLUE);
    lcd_draw_string_bg(0, 16, "screen command OK", YELLOW, BLUE);
    host_puts("KSD:LCD_OK\n");
}

static void amp_test_command(void)
{
    LOG("[sd-uart] host requested AMP_TEST");
    amp_init();
    amp_set(true);
    vTaskDelay(ms_to_ticks_min(200));
    amp_set(false);
    host_puts("KSD:AMP_OK\n");
}

static void sd_test_command(void)
{
    LOG("[sd-uart] host requested SD_TEST");
    host_puts(sd_rw_probe() ? "KSD:SD_OK rw-64\n" : "KSD:SD_FAIL rw\n");
}

static void cam_test_command(void)
{
    LOG("[sd-uart] host requested CAM_TEST");
    char detail[64];
    char b[128];
    if (camera_ram_probe(detail, sizeof(detail)))
        snprintf(b, sizeof(b), "KSD:CAM_OK %s\n", detail);
    else
        snprintf(b, sizeof(b), "KSD:CAM_FAIL %s\n", detail);
    host_puts(b);
}

static void mic_test_command(void)
{
    LOG("[sd-uart] host requested MIC_TEST");
    host_puts("KSD:MIC_SKIP no-mic-capture-driver\n");
}

static void selftest_command(void)
{
    bool ok = true;
    char detail[64];
    LOG("[sd-uart] host requested SELFTEST");
    host_puts("KSD:TEST_BEGIN\n");
    test_line("KSD_SERVER", "PASS", "command-loop");
    lcd_clear(DARKGREEN);
    lcd_draw_string_bg(0, 0, "KSD SELFTEST", WHITE, DARKGREEN);
    test_line("LCD", "PASS", "color-text");
    amp_init();
    amp_set(true);
    vTaskDelay(ms_to_ticks_min(120));
    amp_set(false);
    test_line("AUDIO_AMP", "PASS", "toggle");
    if (sd_rw_probe())
        test_line("SD_RW", "PASS", "64-bytes");
    else { test_line("SD_RW", "FAIL", "rw"); ok = false; }
    if (camera_ram_probe(detail, sizeof(detail)))
        test_line("CAMERA_RAM", "PASS", detail);
    else { test_line("CAMERA_RAM", "FAIL", detail); ok = false; }
    test_line("MICROPHONE", "SKIP", "no-mic-capture-driver");
    test_line("ESP_UART_SPI", "SKIP", "use-RUN_SPI-after-ESP-fw");
    host_puts(ok ? "KSD:TEST_END PASS\n" : "KSD:TEST_END FAIL\n");
}

static bool parse_optional_path(const char *line, const char *cmd,
                                const char *default_path, char *rel_path,
                                size_t rel_path_size)
{
    size_t cmd_len = strlen(cmd);
    const char *p = line + cmd_len;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == 0) {
        snprintf(rel_path, rel_path_size, "%s", default_path);
        return true;
    }
    return sscanf(p, "%127s", rel_path) == 1;
}

static bool command_loop(void)
{
    for (;;) {
        char line[192];
        host_puts("KSD:CMD\n");
        if (!read_line(line, sizeof(line), UART_SD_CMD_TIMEOUT_MS)) {
            diag_line(4, "UART command timeout");
            return false;
        }
        if (strcmp(line, "DONE") == 0) {
            host_puts("KSD:DONE\n");
            LOG("[sd-uart] command session done");
            diag_line(3, "UART command: done");
            return true;
        }
        if (strcmp(line, "HELP") == 0) { help_command(); continue; }
        if (strcmp(line, "SELFTEST") == 0) { selftest_command(); continue; }
        if (strcmp(line, "LCD_TEST") == 0) { lcd_test_command(); continue; }
        if (strcmp(line, "AMP_TEST") == 0) { amp_test_command(); continue; }
        if (strcmp(line, "SD_TEST") == 0) { sd_test_command(); continue; }
        if (strcmp(line, "CAM_TEST") == 0) { cam_test_command(); continue; }
        if (strcmp(line, "MIC_TEST") == 0) { mic_test_command(); continue; }
        if (strcmp(line, "FORMAT_SD") == 0) { format_sd_command(); continue; }
        if (strncmp(line, "CAM_CAPTURE", 11) == 0 && (line[11] == 0 || line[11] == ' ' || line[11] == '\t')) {
            char rel_path[128];
            if (!parse_optional_path(line, "CAM_CAPTURE", CAM_CAPTURE_DEFAULT_PATH, rel_path, sizeof(rel_path))) {
                host_puts("KSD:CAPTURE_FAIL bad-args\n");
                continue;
            }
            run_cam_capture_command(rel_path);
            continue;
        }
        if (strcmp(line, "RUN_SPI") == 0) { run_spi_command(); continue; }
        if (strcmp(line, "RESET") == 0) { board_reset(); }
        if (strcmp(line, "FLASH_ESP") == 0) { run_esp_flash_command(); continue; }
        char rel_path[128];
        unsigned long size = 0;
        if (sscanf(line, "GET %127s", rel_path) == 1) {
            if (!send_file_quiet(rel_path))
                return false;
            continue;
        }
        if (sscanf(line, "PUT %127s %lu", rel_path, &size) == 2) {
            LOGF("[sd-uart] put %s %lu", rel_path, size);
            diag_printf(4, "PUT %.24s", rel_path);
            if (!receive_file(rel_path, (uint32_t)size))
                return false;
            continue;
        }
        LOGF("[sd-uart] bad command: %s", line);
        diag_line(4, "UART bad command");
        host_puts("KSD:ERR command\n");
        return false;
    }
}

bool sd_uart_receive_window(uint32_t window_ms)
{
    LOGF("[sd-uart] waiting %lu ms for UART upload", (unsigned long)window_ms);
    diag_printf(3, "UART upload: waiting %lu s", (unsigned long)(window_ms / 1000));
    drain_rx(300);
    if (!wait_magic(window_ms)) {
        diag_line(3, "UART upload: no host");
        return false;
    }
    drain_rx(20);
    host_puts("KSD:HELLO\n");
    LOG("[sd-uart] host connected");
    diag_line(3, "UART upload: connected");
    return command_loop();
}

static void sd_uart_task(void *arg)
{
    (void)arg;
    LOG("[sd-uart] persistent service started");
    for (;;) {
        if (!wait_magic(1000)) {
            vTaskDelay(ms_to_ticks_min(20));
            continue;
        }
        drain_rx(20);
        host_puts("KSD:HELLO\n");
        LOG("[sd-uart] persistent host connected");
        diag_line(3, "UART service: connected");
        command_loop();
    }
}

void sd_uart_service_start(void)
{
    if (s_service_started)
        return;
    s_service_started = 1;
    xTaskCreate(sd_uart_task, "ksd", 6144, NULL, tskIDLE_PRIORITY + 2, NULL);
}
