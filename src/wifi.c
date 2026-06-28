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
