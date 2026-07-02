/*
 * Wi-Fi — ESP8285 (Maix M1W) over UART2 AT firmware.
 *
 * Pins (pinout.h):  K210 UART2_TX=IO6 → ESP RX,  UART2_RX=IO7 ← ESP TX,
 *                   EN=IO8 (GPIO0, HIGH=run / LOW=reset).
 *
 * Device name: hardware UART2 is "/dev/uart2" — the driver vars are 0-indexed
 * names for UART1/2/3 (g_uart_driver_uart1 == UART2_BASE, see uart.cpp).
 */
#include "wifi.h"
#include "wifi_cfg.h"
#include "pinout.h"
#include <devices.h>
#include <filesystem.h>
#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <sysctl.h>
#include <ff.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "log.h"

static handle_t uart;
static char     acc[1024];          /* response accumulator */

#define ESP_BOOT_BAUD 115200
static const int esp_fast_bauds[] = { 921600, 460800 };

static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;

/* ── ESP enable pin via the low-speed GPIO peripheral (like amp.c) ────────── */
static void esp_en(int on)
{
    REG_GPIO->direction.u32[0] |= (1u << GPIO_ESP_EN);
    if (on)
        REG_GPIO->data_output.u32[0] |=  (1u << GPIO_ESP_EN);
    else
        REG_GPIO->data_output.u32[0] &= ~(1u << GPIO_ESP_EN);
}

static void esp_write(const char *s)
{
    io_write(uart, (const uint8_t *)s, strlen(s));
}

static void esp_drain_rx(int quiet_ms)
{
    int quiet = 0;
    while (quiet < quiet_ms) {
        uint8_t tmp[128];
        int r = io_read(uart, tmp, sizeof(tmp));
        if (r > 0)
            quiet = 0;
        else {
            usleep(10 * 1000);
            quiet += 10;
        }
    }
}

/* Drain RX into acc[] until `needle` appears (->1), "ERROR"/"FAIL" (-> -1),
 * or timeout (->0). */
static int esp_expect(const char *needle, int timeout_ms)
{
    int n = 0, waited = 0;
    acc[0] = 0;
    while (waited < timeout_ms) {
        uint8_t tmp[128];
        int r = io_read(uart, tmp, sizeof(tmp));
        if (r > 0) {
            for (int i = 0; i < r && n < (int)sizeof(acc) - 1; i++)
                acc[n++] = (char)tmp[i];
            acc[n] = 0;
            if (strstr(acc, needle)) return 1;
            if (strstr(acc, "ERROR") || strstr(acc, "FAIL")) return -1;
        } else {
            usleep(10 * 1000);
            waited += 10;
        }
    }
    return 0;
}

static int esp_cmd(const char *cmd, const char *expect, int timeout_ms)
{
    esp_drain_rx(20);
    esp_write(cmd);
    esp_write("\r\n");
    return esp_expect(expect, timeout_ms);
}

static int esp_boot_at_115200(void)
{
    uart_config(uart, ESP_BOOT_BAUD, 8, UART_STOP_1, UART_PARITY_NONE);

    /* AT+UART_CUR is temporary, so this reset recovers the module to the
     * firmware default instead of leaving external tools stuck at high baud. */
    esp_en(0);
    usleep(300 * 1000);
    esp_en(1);
    usleep(1000 * 1000);
    esp_expect("ready", 2000);
    esp_drain_rx(50);

    int ok = 0;
    for (int i = 0; i < 8 && ok != 1; i++)
        ok = esp_cmd("AT", "OK", 600);
    if (ok != 1)
        return 0;

    esp_cmd("ATE0", "OK", 1000);
    return 1;
}

static int esp_try_fast_baud(int baud)
{
    char cmd[64];

    snprintf(cmd, sizeof(cmd), "AT+UART_CUR=%d,8,1,0,0", baud);
    if (esp_cmd(cmd, "OK", 1500) != 1) {
        LOGF("[wifi] UART %d rejected: <<%s>>", baud, acc);
        return 0;
    }

    usleep(100 * 1000);
    uart_config(uart, baud, 8, UART_STOP_1, UART_PARITY_NONE);
    usleep(100 * 1000);
    esp_drain_rx(20);

    for (int i = 0; i < 5; i++) {
        if (esp_cmd("AT", "OK", 600) == 1) {
            LOGF("[wifi] ESP UART %d", baud);
            return 1;
        }
    }

    LOGF("[wifi] UART %d handshake failed", baud);
    return 0;
}

static int esp_set_fast_baud(void)
{
    for (int i = 0; i < (int)(sizeof(esp_fast_bauds) / sizeof(esp_fast_bauds[0])); i++) {
        int baud = esp_fast_bauds[i];
        if (i > 0 && !esp_boot_at_115200()) {
            LOG("[wifi] AT recovery failed before baud fallback");
            return ESP_BOOT_BAUD;
        }
        if (esp_try_fast_baud(baud))
            return baud;
    }

    if (esp_boot_at_115200())
        LOG("[wifi] fast UART unavailable, using 115200");
    else
        LOG("[wifi] fast UART unavailable and 115200 recovery failed");
    return ESP_BOOT_BAUD;
}

static int esp_send_bytes(int link_id, const uint8_t *data, int len)
{
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%d", link_id, len);
    esp_write(cmd);
    esp_write("\r\n");
    if (esp_expect(">", 5000) != 1)
        return 0;
    io_write(uart, data, len);
    return esp_expect("SEND OK", 10000) == 1;
}

static int esp_send_plain(const char *s)
{
    int len = (int)strlen(s);
    char cmd[32];

    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d", len);
    if (esp_cmd(cmd, ">", 2000) != 1)
        return 0;
    esp_write(s);
    return esp_expect("SEND OK", 3000) == 1;
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int esp_read_byte_timeout(uint8_t *out, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        int r = io_read(uart, out, 1);
        if (r == 1)
            return 1;
        usleep(10 * 1000);
        waited += 10;
    }
    return 0;
}

static int esp_tcp_read_byte(int timeout_ms)
{
    static int ipd_left = 0;
    uint8_t c;

    if (ipd_left > 0) {
        if (!esp_read_byte_timeout(&c, timeout_ms))
            return -1;
        ipd_left--;
        return c;
    }

    char match[] = "+IPD,";
    int mi = 0;
    int waited = 0;
    while (waited < timeout_ms) {
        if (!esp_read_byte_timeout(&c, 100)) {
            waited += 100;
            continue;
        }
        if (c == (uint8_t)match[mi]) {
            mi++;
            if (match[mi] == 0)
                break;
        } else {
            mi = (c == '+') ? 1 : 0;
        }
    }
    if (match[mi] != 0)
        return -1;

    char hdr[24];
    int n = 0;
    while (n < (int)sizeof(hdr) - 1) {
        if (!esp_read_byte_timeout(&c, 2000))
            return -1;
        if (c == ':')
            break;
        hdr[n++] = (char)c;
    }
    hdr[n] = 0;

    char *last = strrchr(hdr, ',');
    const char *len_s = last ? last + 1 : hdr;
    ipd_left = atoi(len_s);
    if (ipd_left <= 0)
        return -1;

    if (!esp_read_byte_timeout(&c, timeout_ms))
        return -1;
    ipd_left--;
    return c;
}

static int esp_tcp_read_line(char *out, int out_len, int timeout_ms)
{
    int n = 0;
    while (n < out_len - 1) {
        int c = esp_tcp_read_byte(timeout_ms);
        if (c < 0)
            return 0;
        if (c == '\n')
            break;
        if (c != '\r')
            out[n++] = (char)c;
    }
    out[n] = 0;
    return 1;
}

static void make_parent_dirs(const char *rel_path)
{
    char path[160] = "0:/";
    char alt[160] = "0/";
    int p = 3;
    int a = 2;
    for (const char *s = rel_path; *s && p < (int)sizeof(path) - 2; s++) {
        char c = *s == '\\' ? '/' : *s;
        if (c == '/') {
            path[p] = 0;
            alt[a] = 0;
            if (p > 3)
                f_mkdir(path);
            if (a > 2)
                f_mkdir(alt);
        }
        path[p++] = c;
        alt[a++] = c;
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

static int esp_send_bmp(int link_id, const uint16_t *rgb565, int w, int h)
{
    const int row_bytes = w * 3;
    const int image_bytes = row_bytes * h;
    const int bmp_bytes = 54 + image_bytes;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_u32le(&hdr[2], bmp_bytes);
    put_u32le(&hdr[10], 54);
    put_u32le(&hdr[14], 40);
    put_u32le(&hdr[18], (uint32_t)w);
    put_u32le(&hdr[22], (uint32_t)h);
    put_u16le(&hdr[26], 1);
    put_u16le(&hdr[28], 24);
    put_u32le(&hdr[34], image_bytes);
    if (!esp_send_bytes(link_id, hdr, sizeof(hdr)))
        return 0;

    uint8_t row[320 * 3];
    if (w * 3 > (int)sizeof(row)) {
        LOG("[wifi] image row too wide");
        return 0;
    }

    for (int y = h - 1; y >= 0; y--) {
        uint8_t *out = row;
        const uint16_t *src = rgb565 + y * w;
        for (int x = 0; x < w; x++) {
            uint16_t p = src[x];
            uint8_t r = (uint8_t)(((p >> 11) & 0x1f) * 255 / 31);
            uint8_t g = (uint8_t)(((p >> 5) & 0x3f) * 255 / 63);
            uint8_t b = (uint8_t)((p & 0x1f) * 255 / 31);
            *out++ = b;
            *out++ = g;
            *out++ = r;
        }
        if (!esp_send_bytes(link_id, row, row_bytes))
            return 0;
    }

    LOGF("[wifi] snapshot sent (%d bytes BMP)", bmp_bytes);
    return 1;
}

static void wifi_init_pins(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    /* NB: the "TX/RX" labels in pinout.h are ESP-side. K210-side is mirrored
     * (verified against the working MaixPy config):
     *   IO6 = UART2_RX  (ESP TX → K210 RX)
     *   IO7 = UART2_TX  (K210 TX → ESP RX) */
    fpioa_set_function(PIN_ESP_TX, FUNC_UART2_RX);   /* IO6 = K210 RX */
    fpioa_set_function(PIN_ESP_RX, FUNC_UART2_TX);   /* IO7 = K210 TX */
    fpioa_set_function(PIN_ESP_EN, FUNC_GPIO0);      /* IO8 enable    */
}

bool wifi_connect(char *ip_out, int ip_len)
{
    ip_out[0] = 0;

    wifi_init_pins();
    uart = io_open("/dev/uart2");                    /* == hw UART2 */
    configASSERT(uart);

    if (!esp_boot_at_115200()) {
        LOG("[wifi] no AT response");
        return false;
    }

    int active_baud = esp_set_fast_baud();
    LOGF("[wifi] active UART baud=%d", active_baud);
    esp_cmd("AT+CWMODE=1", "OK", 2000);              /* station mode */

    char cmd[160];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASS);
    LOGF("[wifi] joining %s ...", WIFI_SSID);
    int jr = esp_cmd(cmd, "OK", 20000);              /* join can take a while */
    LOGF("[wifi] CWJAP rc=%d resp=<<%s>>", jr, acc);
    if (jr != 1) {
        LOG("[wifi] CWJAP failed");
        return false;
    }

    /* Query the assigned STA IP: line is +CIFSR:STAIP,"x.x.x.x" */
    int cr = esp_cmd("AT+CIFSR", "OK", 3000);
    LOGF("[wifi] CIFSR rc=%d resp=<<%s>>", cr, acc);
    if (cr != 1)
        return false;

    char *p = strstr(acc, "STAIP,\"");
    if (!p) return false;
    p += 7;
    char *q = strchr(p, '"');
    if (!q) return false;

    int len = q - p;
    if (len <= 0 || len >= ip_len) return false;
    memcpy(ip_out, p, len);
    ip_out[len] = 0;

    if (strcmp(ip_out, "0.0.0.0") == 0) {
        ip_out[0] = 0;
        return false;
    }
    LOGF("[wifi] connected ip=%s", ip_out);
    return true;
}

bool wifi_pull_files(const char *host, int port)
{
    if (!uart || !host)
        return false;

    esp_cmd("AT+CIPSERVER=0", "OK", 1000);
    esp_cmd("AT+CIPMUX=0", "OK", 2000);

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", host, port);
    if (esp_cmd(cmd, "OK", 12000) != 1 && !strstr(acc, "ALREADY CONNECTED")) {
        LOGF("[wifi] pull CIPSTART failed: <<%s>>", acc);
        return false;
    }

    LOGF("[wifi] pulling files from %s:%d", host, port);
    if (esp_send_plain("RDY\n")) {
        LOG("[wifi] pull ready sent");
    } else {
        LOGF("[wifi] pull ready failed: <<%s>>", acc);
        esp_cmd("AT+CIPCLOSE", "OK", 2000);
        return false;
    }

    int count = 0;
    int ok = 0;
    int blank_lines = 0;
    for (;;) {
        char name[96];
        char size_line[24];
        if (!esp_tcp_read_line(name, sizeof(name), 30000)) {
            LOG("[wifi] pull: no name");
            break;
        }
        if (name[0] == 0) {
            blank_lines++;
            LOG("[wifi] pull: blank line");
            if (blank_lines > 4)
                break;
            continue;
        }
        blank_lines = 0;
        if (strcmp(name, "EOF") == 0) {
            LOG("[wifi] pull: EOF");
            ok = 1;
            break;
        }
        if (!safe_rel_path(name)) {
            LOGF("[wifi] pull: bad path %s", name);
            break;
        }
        if (!esp_tcp_read_line(size_line, sizeof(size_line), 5000)) {
            LOGF("[wifi] pull: no size for %s", name);
            break;
        }

        int size = atoi(size_line);
        char padded_line[24];
        if (!esp_tcp_read_line(padded_line, sizeof(padded_line), 5000)) {
            LOGF("[wifi] pull: no padded size for %s", name);
            break;
        }

        int padded = atoi(padded_line);
        LOGF("[wifi] pull file: %s size=%d padded=%d", name, size, padded);
        if (size < 0) {
            LOGF("[wifi] pull: bad size %s", size_line);
            break;
        }
        if (padded < size) {
            LOGF("[wifi] pull: bad padded size %s", padded_line);
            break;
        }

        make_parent_dirs(name);
        char path[128];
        snprintf(path, sizeof(path), "/fs/0/%s", name);
        LOGF("[wifi] open write: %s", path);
        handle_t f = filesystem_file_open(path, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
        if (!f) {
            LOGF("[wifi] pull: open failed %s", path);
            break;
        }
        LOGF("[wifi] open ok: %s", path);
        if (!esp_send_plain("GO\n")) {
            filesystem_file_close(f);
            LOGF("[wifi] go ack failed for %s", name);
            return false;
        }

        uint8_t buf[512];
        int got = 0;
        int next_log = 32768;
        while (got < padded) {
            int chunk = padded - got;
            if (chunk > (int)sizeof(buf))
                chunk = sizeof(buf);
            for (int i = 0; i < chunk; i++) {
                int c = esp_tcp_read_byte(10000);
                if (c < 0) {
                    filesystem_file_close(f);
                    LOGF("[wifi] pull: short %s %d/%d", name, got, padded);
                    return false;
                }
                buf[i] = (uint8_t)c;
            }
            if (got < size) {
                int write_len = size - got;
                if (write_len > chunk)
                    write_len = chunk;
                int wr = filesystem_file_write(f, buf, write_len);
                if (wr != write_len) {
                    filesystem_file_close(f);
                    LOGF("[wifi] pull: write failed %s", name);
                    return false;
                }
            }
            got += chunk;
            if (got >= next_log || got == padded) {
                int written = got > size ? size : got;
                LOGF("[wifi] write progress: %s %d/%d", name, written, size);
                next_log += 32768;
            }
            if ((got % 2048) == 0 || got == padded) {
                if (!esp_send_plain("B\n")) {
                    filesystem_file_close(f);
                    LOGF("[wifi] block ack failed for %s", name);
                    return false;
                }
            }
        }

        filesystem_file_close(f);
        count++;
        LOGF("[wifi] recv %s %d", name, size);
        if (!esp_send_plain("OK\n")) {
            LOGF("[wifi] ack failed after %s: <<%s>>", name, acc);
            return false;
        }

    }

    esp_send_plain("DONE\n");
    esp_cmd("AT+CIPCLOSE", "OK", 2000);
    LOGF("[wifi] pull done: %d files", count);
    return ok && count > 0;
}

bool wifi_serve_bmp_snapshot(const uint16_t *rgb565, int w, int h, int port)
{
    if (!uart || !rgb565 || w <= 0 || h <= 0)
        return false;

    char cmd[48];
    esp_cmd("AT+CIPSERVER=0", "OK", 2000);
    if (esp_cmd("AT+CIPMUX=1", "OK", 2000) != 1) {
        LOGF("[wifi] CIPMUX failed: <<%s>>", acc);
        return false;
    }
    snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%d", port);
    if (esp_cmd(cmd, "OK", 3000) != 1) {
        LOGF("[wifi] CIPSERVER failed: <<%s>>", acc);
        return false;
    }

    LOGF("[wifi] snapshot server ready on port %d", port);
    int link_id = -1;
    int waited = 0;
    acc[0] = 0;
    int n = 0;
    while (waited < 120000 && link_id < 0) {
        uint8_t tmp[128];
        int r = io_read(uart, tmp, sizeof(tmp));
        if (r > 0) {
            for (int i = 0; i < r && n < (int)sizeof(acc) - 1; i++)
                acc[n++] = (char)tmp[i];
            acc[n] = 0;
            char *p = strstr(acc, ",CONNECT");
            if (p && p > acc)
                link_id = p[-1] - '0';
            p = strstr(acc, "+IPD,");
            if (p && p[5] >= '0' && p[5] <= '4')
                link_id = p[5] - '0';
        } else {
            usleep(10 * 1000);
            waited += 10;
        }
    }

    if (link_id < 0) {
        LOG("[wifi] snapshot server timeout");
        esp_cmd("AT+CIPSERVER=0", "OK", 2000);
        return false;
    }

    const int bmp_bytes = 54 + w * h * 3;

    char http[160];
    int http_len = snprintf(http, sizeof(http),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/bmp\r\n"
        "Content-Disposition: attachment; filename=\"snapshot.bmp\"\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        bmp_bytes);
    if (!esp_send_bytes(link_id, (const uint8_t *)http, http_len))
        return false;

    if (!esp_send_bmp(link_id, rgb565, w, h))
        return false;

    snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", link_id);
    esp_cmd(cmd, "OK", 2000);
    esp_cmd("AT+CIPSERVER=0", "OK", 2000);
    LOGF("[wifi] snapshot sent (%d bytes BMP)", bmp_bytes);
    return true;
}

bool wifi_push_bmp_snapshot(const char *host, int port, const uint16_t *rgb565, int w, int h)
{
    if (!uart || !host || !rgb565 || w <= 0 || h <= 0)
        return false;

    esp_cmd("AT+CIPSERVER=0", "OK", 1000);
    esp_cmd("AT+CIPMUX=1", "OK", 2000);

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=0,\"TCP\",\"%s\",%d", host, port);
    if (esp_cmd(cmd, "OK", 12000) != 1 && !strstr(acc, "ALREADY CONNECTED")) {
        LOGF("[wifi] CIPSTART failed: <<%s>>", acc);
        return false;
    }

    bool ok = esp_send_bmp(0, rgb565, w, h);
    esp_cmd("AT+CIPCLOSE=0", "OK", 2000);
    return ok;
}
