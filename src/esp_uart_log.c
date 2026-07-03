#include "esp_uart_log.h"
#include "pinout.h"
#include "log.h"
#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <fpioa.h>
#include <uart.h>
#include <gpio.h>
#include <sysctl.h>
#include <uarths.h>
#include <string.h>

#define ESP_UART_BAUD 115200
#define ESP_LINE_MAX 128

static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;
static volatile int s_spi_ready;
static volatile int s_put_active;
static volatile int s_running;
static volatile int s_started;

static void gpio_set(int n, int v)
{
    REG_GPIO->direction.u32[0] |= 1u << n;
    if (v) REG_GPIO->data_output.u32[0] |= 1u << n;
    else REG_GPIO->data_output.u32[0] &= ~(1u << n);
}

int esp_uart_log_spi_ready(void)
{
    return s_spi_ready;
}

int esp_uart_log_put_active(void)
{
    return s_put_active;
}

void esp_uart_log_clear_put_active(void)
{
    s_put_active = 0;
}

static void esp_reset_normal(void)
{
    s_spi_ready = 0;
    s_put_active = 0;
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    fpioa_set_function(PIN_ESP_BOOT, FUNC_GPIO3);
    fpioa_set_function(PIN_ESP_EN, FUNC_GPIO0);
    gpio_set(GPIO_ESP_BOOT, 1);
    gpio_set(GPIO_ESP_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set(GPIO_ESP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void feed_esp_line_detector(uint8_t ch)
{
    static char line[ESP_LINE_MAX];
    static unsigned pos;

    if (ch == '\r')
        return;

    if (ch == '\n') {
        line[pos] = 0;
        if (strstr(line, "kesp: spi slave ready")) {
            s_spi_ready = 1;
            LOG("[esp-uart] SPI slave ready marker");
        }
        if (strstr(line, "kesp: PUT ")) {
            s_put_active = 1;
            LOG("[esp-uart] TCP PUT marker, enable SPI polling");
        }
        pos = 0;
        return;
    }

    if (pos + 1 < sizeof(line))
        line[pos++] = (char)ch;
    else
        pos = 0;
}

static void esp_uart_log_task(void *arg)
{
    handle_t u = (handle_t)arg;
    uint8_t b[64];

    while (s_running) {
        int r = io_read(u, b, sizeof(b));
        if (r > 0) {
            for (int i = 0; i < r; i++) {
                feed_esp_line_detector(b[i]);
                uarths_write_byte(b[i]);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    io_close(u);
    s_started = 0;
    LOG("[esp-uart] bridge inactive");
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

void esp_uart_log_stop(void)
{
    if (!s_started)
        return;

    LOG("[esp-uart] bridge pause for ESP flash");
    s_running = 0;
    for (int i = 0; i < 100 && s_started; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
}

void esp_uart_log_start(void)
{
    if (s_started) {
        LOG("[esp-uart] bridge already running");
        return;
    }

    fpioa_set_function(PIN_ESP_TX, FUNC_UART2_RX);
    fpioa_set_function(PIN_ESP_RX, FUNC_UART2_TX);
    handle_t u = io_open("/dev/uart2");
    if (!u) {
        LOG("[esp-uart] open failed");
        return;
    }

    uart_config(u, ESP_UART_BAUD, 8, UART_STOP_1, UART_PARITY_NONE);
    s_running = 1;
    s_started = 1;
    xTaskCreate(esp_uart_log_task, "esp_uart_log", 1024, (void *)u, 1, NULL);
    LOG("[esp-uart] bridge on 115200");
    esp_reset_normal();
}
