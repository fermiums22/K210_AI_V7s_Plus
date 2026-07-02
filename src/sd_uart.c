#include "sd_uart.h"
#include "log.h"
#include "diag_screen.h"

#include <FreeRTOS.h>
#include <task.h>
#include <filesystem.h>
#include <ff.h>
#include <platform.h>
#include <uarths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UART_SD_MAGIC "KSD1\n"
#define UART_SD_BUF   512

static volatile uarths_t *const REG_UARTHS = (volatile uarths_t *)UARTHS_BASE_ADDR;
static uint8_t rx_buf[UART_SD_BUF] __attribute__((aligned(64)));

static int uarths_try_read_byte(uint8_t *out)
{
    uarths_rxdata_t recv = REG_UARTHS->rxdata;
    if (recv.empty)
        return 0;
    *out = recv.data;
    return 1;
}

static void host_puts(const char *s)
{
    uarths_puts(s);
}

static int read_byte_timeout(uint8_t *out, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (uarths_try_read_byte(out))
            return 1;
        vTaskDelay(pdMS_TO_TICKS(1));
        waited++;
    }
    return 0;
}

static int read_line(char *out, int out_len, uint32_t timeout_ms)
{
    int n = 0;
    uint8_t c;
    while (n < out_len - 1) {
        if (!read_byte_timeout(&c, timeout_ms))
            return 0;
        if (c == '\n')
            break;
        if (c != '\r')
            out[n++] = (char)c;
    }
    out[n] = 0;
    return 1;
}

static int wait_magic(uint32_t window_ms)
{
    const char *magic = UART_SD_MAGIC;
    int mi = 0;
    uint32_t waited = 0;
    uint32_t announced = 1000;
    uint8_t c;

    while (waited < window_ms) {
        if (!uarths_try_read_byte(&c)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            waited++;
            announced++;
            if (announced >= 1000) {
                host_puts("KSD:READY\n");
                announced = 0;
            }
            continue;
        }

        if (c == (uint8_t)magic[mi]) {
            mi++;
            if (magic[mi] == 0)
                return 1;
        } else {
            mi = (c == (uint8_t)magic[0]) ? 1 : 0;
        }
    }
    return 0;
}

static void drain_rx(uint32_t quiet_ms)
{
    uint32_t quiet = 0;
    uint8_t c;
    while (quiet < quiet_ms) {
        if (uarths_try_read_byte(&c)) {
            quiet = 0;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        quiet++;
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

static bool receive_file(const char *rel_path, uint32_t size)
{
    if (!safe_rel_path(rel_path)) {
        LOGF("[sd-uart] bad path: %s", rel_path);
        diag_printf(5, "UART bad path: %.24s", rel_path);
        host_puts("KSD:ERR bad-path\n");
        return false;
    }

    make_parent_dirs(rel_path);

    char path[160];
    snprintf(path, sizeof(path), "/fs/0/%s", rel_path);
    handle_t f = filesystem_file_open(path, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
    if (!f) {
        LOGF("[sd-uart] open failed: %s", path);
        diag_printf(5, "UART open failed");
        host_puts("KSD:ERR open\n");
        return false;
    }

    host_puts("KSD:GO\n");

    uint32_t got = 0;
    while (got < size) {
        uint32_t chunk = size - got;
        if (chunk > sizeof(rx_buf))
            chunk = sizeof(rx_buf);

        for (uint32_t i = 0; i < chunk; i++) {
            if (!read_byte_timeout(&rx_buf[i], 5000)) {
                filesystem_file_close(f);
                LOGF("[sd-uart] short file: %s %lu/%lu",
                     rel_path, (unsigned long)got, (unsigned long)size);
                diag_printf(5, "UART short %lu/%lu", (unsigned long)got, (unsigned long)size);
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
        if ((got % (32u * 1024u)) == 0 || got == size) {
            LOGF("[sd-uart] recv %s %lu/%lu", rel_path, (unsigned long)got, (unsigned long)size);
            diag_printf(5, "UART %lu/%lu", (unsigned long)got, (unsigned long)size);
        }
        host_puts("KSD:B\n");
    }

    filesystem_file_close(f);
    host_puts("KSD:OK\n");
    return true;
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

    host_puts("KSD:HELLO\n");
    LOG("[sd-uart] host connected");
    diag_line(3, "UART upload: connected");

    for (;;) {
        char line[192];
        host_puts("KSD:CMD\n");
        if (!read_line(line, sizeof(line), 30000)) {
            LOG("[sd-uart] command timeout");
            diag_line(4, "UART command timeout");
            return false;
        }

        if (strcmp(line, "DONE") == 0) {
            host_puts("KSD:DONE\n");
            LOG("[sd-uart] upload done");
            diag_line(3, "UART upload: done");
            return true;
        }

        char rel_path[128];
        unsigned long size = 0;
        if (sscanf(line, "PUT %127s %lu", rel_path, &size) != 2) {
            LOGF("[sd-uart] bad command: %s", line);
            diag_line(4, "UART bad command");
            host_puts("KSD:ERR command\n");
            return false;
        }

        LOGF("[sd-uart] put %s %lu", rel_path, size);
        diag_printf(4, "PUT %.24s", rel_path);
        if (!receive_file(rel_path, (uint32_t)size))
            return false;
    }
}
