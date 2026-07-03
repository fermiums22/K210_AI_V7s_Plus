#include "sd_uart.h"
#include "log.h"
#include "diag_screen.h"

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

/* Keep the host sync word independent from CR/LF handling.  The PC helper sends
 * KSD1\n, but accepting KSD1 first makes the boot-window handshake robust even if
 * the newline is delayed, stripped, or consumed by a previous drain. */
#define UART_SD_MAGIC "KSD1"
#define UART_SD_BUF   512
#define UARTHS_RXDATA_EMPTY_MASK (1u << 31)

static volatile uarths_t *const REG_UARTHS = (volatile uarths_t *)UARTHS_BASE_ADDR;
static uint8_t rx_buf[UART_SD_BUF] __attribute__((aligned(64)));

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
    /* UARTHS rxdata is SiFive-style: bit31 is EMPTY, bits[7:0] are DATA.
     * Use a raw register read instead of the SDK bitfield type so RX polling is
     * not dependent on compiler bitfield layout. */
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

static void log_rx_byte(uint8_t c)
{
    if (c >= 0x20 && c <= 0x7e)
        LOGF("[sd-uart] rx byte %02X '%c'", (unsigned)c, (char)c);
    else
        LOGF("[sd-uart] rx byte %02X", (unsigned)c);
}

static int read_byte_timeout(uint8_t *out, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    while (!deadline_expired(start, timeout_ms)) {
        if (uarths_try_read_byte(out))
            return 1;
        poll_delay();
    }
    return 0;
}

static int read_line(char *out, int out_len, uint32_t timeout_ms)
{
    int n = 0;
    uint8_t c;

    LOGF("[sd-uart] read_line: waiting %lu ms", (unsigned long)timeout_ms);

    while (n < out_len - 1) {
        if (!read_byte_timeout(&c, timeout_ms)) {
            out[n] = 0;
            LOGF("[sd-uart] read_line timeout after %d chars: %s", n, out);
            return 0;
        }

        log_rx_byte(c);

        if (c == '\n')
            break;
        if (c != '\r')
            out[n++] = (char)c;
    }
    out[n] = 0;
    LOGF("[sd-uart] read_line done %d chars: %s", n, out);
    return 1;
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

    make_parent_dirs(rel_path);
    f_unlink(fat_path);
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

static bool send_file(const char *rel_path)
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

bool sd_uart_receive_window(uint32_t window_ms)
{
    LOGF("[sd-uart] waiting %lu ms for UART upload", (unsigned long)window_ms);
    diag_printf(3, "UART upload: waiting %lu s", (unsigned long)(window_ms / 1000));
    drain_rx(300);

    if (!wait_magic(window_ms)) {
        diag_line(3, "UART upload: no host");
        return false;
    }

    /* Clear a possible trailing CR/LF from the sync word before command mode. */
    drain_rx(20);

    host_puts("KSD:HELLO\n");
    LOG("[sd-uart] host connected");
    diag_line(3, "UART upload: connected");

    for (;;) {
        char line[192];
        host_puts("KSD:CMD\n");
        LOG("[sd-uart] command prompt sent; waiting for command line");
        if (!read_line(line, sizeof(line), 30000)) {
            LOG("[sd-uart] command timeout");
            diag_line(4, "UART command timeout");
            return false;
        }
        LOGF("[sd-uart] command line: %s", line);

        if (strcmp(line, "DONE") == 0) {
            host_puts("KSD:DONE\n");
            LOG("[sd-uart] upload done");
            diag_line(3, "UART upload: done");
            return true;
        }

        if (strcmp(line, "RESET") == 0) {
            board_reset();
        }

        char rel_path[128];
        unsigned long size = 0;
        if (sscanf(line, "GET %127s", rel_path) == 1) {
            if (!send_file(rel_path))
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
