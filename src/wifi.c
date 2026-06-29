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
#include <fpioa.h>
#include <gpio.h>
#include <platform.h>
#include <sysctl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static handle_t uart;
static char     acc[1024];          /* response accumulator */

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
    esp_write(cmd);
    esp_write("\r\n");
    return esp_expect(expect, timeout_ms);
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
        printf("[wifi] image row too wide\n");
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

    printf("[wifi] snapshot sent (%d bytes BMP)\n", bmp_bytes);
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
    uart_config(uart, 115200, 8, UART_STOP_1, UART_PARITY_NONE);

    /* Power-cycle the ESP so it cold-boots from any stuck AT state
     * (timing matches the working MaixPy bring-up: 300 ms low, 1 s settle). */
    esp_en(0);
    usleep(300 * 1000);
    esp_en(1);
    usleep(1000 * 1000);
    esp_expect("ready", 2000);                       /* AT fw prints "ready" */

    /* Handshake (retry — first bytes after boot are noisy). */
    int ok = 0;
    for (int i = 0; i < 6 && ok != 1; i++)
        ok = esp_cmd("AT", "OK", 500);
    if (ok != 1) {
        printf("[wifi] no AT response\n");
        return false;
    }

    esp_cmd("ATE0", "OK", 1000);                     /* echo off */
    esp_cmd("AT+CWMODE=1", "OK", 2000);              /* station mode */

    char cmd[160];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASS);
    printf("[wifi] joining %s ...\n", WIFI_SSID);
    int jr = esp_cmd(cmd, "OK", 20000);              /* join can take a while */
    printf("[wifi] CWJAP rc=%d resp=<<%s>>\n", jr, acc);
    if (jr != 1) {
        printf("[wifi] CWJAP failed\n");
        return false;
    }

    /* Query the assigned STA IP: line is +CIFSR:STAIP,"x.x.x.x" */
    int cr = esp_cmd("AT+CIFSR", "OK", 3000);
    printf("[wifi] CIFSR rc=%d resp=<<%s>>\n", cr, acc);
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
    printf("[wifi] connected ip=%s\n", ip_out);
    return true;
}

bool wifi_serve_bmp_snapshot(const uint16_t *rgb565, int w, int h, int port)
{
    if (!uart || !rgb565 || w <= 0 || h <= 0)
        return false;

    char cmd[48];
    esp_cmd("AT+CIPSERVER=0", "OK", 2000);
    if (esp_cmd("AT+CIPMUX=1", "OK", 2000) != 1) {
        printf("[wifi] CIPMUX failed: <<%s>>\n", acc);
        return false;
    }
    snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%d", port);
    if (esp_cmd(cmd, "OK", 3000) != 1) {
        printf("[wifi] CIPSERVER failed: <<%s>>\n", acc);
        return false;
    }

    printf("[wifi] snapshot server ready on port %d\n", port);
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
        printf("[wifi] snapshot server timeout\n");
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
    printf("[wifi] snapshot sent (%d bytes BMP)\n", bmp_bytes);
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
        printf("[wifi] CIPSTART failed: <<%s>>\n", acc);
        return false;
    }

    bool ok = esp_send_bmp(0, rgb565, w, h);
    esp_cmd("AT+CIPCLOSE=0", "OK", 2000);
    return ok;
}
