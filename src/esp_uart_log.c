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

static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;

static void gpio_set(int n, int v)
{
    REG_GPIO->direction.u32[0] |= 1u << n;
    if (v) REG_GPIO->data_output.u32[0] |= 1u << n;
    else REG_GPIO->data_output.u32[0] &= ~(1u << n);
}

static void esp_reset_normal(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);
    fpioa_set_function(PIN_ESP_BOOT, FUNC_GPIO3);
    fpioa_set_function(PIN_ESP_EN, FUNC_GPIO0);
    gpio_set(GPIO_ESP_BOOT, 1);
    gpio_set(GPIO_ESP_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set(GPIO_ESP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void esp_uart_log_task(void *arg)
{
    handle_t u = (handle_t)arg;
    uint8_t b[64];
    for (;;) {
        int r = io_read(u, b, sizeof(b));
        if (r > 0) {
            for (int i = 0; i < r; i++)
                uarths_write_byte(b[i]);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void esp_uart_log_start(void)
{
    fpioa_set_function(PIN_ESP_TX, FUNC_UART2_RX);
    fpioa_set_function(PIN_ESP_RX, FUNC_UART2_TX);
    handle_t u = io_open("/dev/uart2");
    if (!u) {
        LOG("[esp-uart] open failed");
        return;
    }
    uart_config(u, 115200, 8, UART_STOP_1, UART_PARITY_NONE);
    xTaskCreate(esp_uart_log_task, "esp_uart_log", 512, (void *)u, 1, NULL);
    LOG("[esp-uart] bridge on 115200");
    esp_reset_normal();
}
