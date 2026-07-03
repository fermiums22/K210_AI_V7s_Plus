#include "esp_spi_link.h"
#include "pinout.h"
#include "log.h"
#include "diag_screen.h"
#include "amp.h"
#include "esp_uart_log.h"

#include <FreeRTOS.h>
#include <task.h>
#include <fpioa.h>
#include <gpio.h>
#include <sysctl.h>
#include <platform.h>
#include <stdio.h>

#define KGPIO_CS_OUT     4
#define KGPIO_CLK_OUT    5
#define KGPIO_MOSI_OUT   6
#define KGPIO_MISO_IN    7

#define FUNC_GPIO_NUM(n) ((fpioa_function_t)(56 + (n)))
#define GPIO_BIT(n)      (1u << (n))

static volatile gpio_t *const REG_GPIO = (volatile gpio_t *)GPIO_BASE_ADDR;

static volatile int s_paused;
static uint32_t s_step;
static int s_last_miso = -1;
static uint32_t s_miso_changes;
static uint32_t s_miso_seen0;
static uint32_t s_miso_seen1;
static uint32_t s_result_logged;
static TickType_t s_last_log_tick;

void esp_spi_link_pause(int pause)
{
    int new_state = pause ? 1 : 0;
    if (s_paused == new_state)
        return;
    s_paused = new_state;
    LOG(s_paused ? "[gpio-test] paused" : "[gpio-test] resumed");
}

static void gpio_out_set(int gpio_n, int val)
{
    REG_GPIO->direction.u32[0] |= GPIO_BIT(gpio_n);
    if (val)
        REG_GPIO->data_output.u32[0] |= GPIO_BIT(gpio_n);
    else
        REG_GPIO->data_output.u32[0] &= ~GPIO_BIT(gpio_n);
}

static void gpio_input_set(int gpio_n)
{
    REG_GPIO->direction.u32[0] &= ~GPIO_BIT(gpio_n);
}

static int gpio_read(int gpio_n)
{
    return (REG_GPIO->data_input.u32[0] & GPIO_BIT(gpio_n)) ? 1 : 0;
}

static void gpio_link_init(void)
{
    sysctl_clock_enable(SYSCTL_CLOCK_GPIO);

    /* Keep ESP enabled and in normal boot. These are schematic pull-up/down backed,
     * but drive them here so the test is deterministic after a K210 reset. */
    fpioa_set_function(PIN_ESP_EN, FUNC_GPIO0);
    fpioa_set_function(PIN_ESP_BOOT, FUNC_GPIO3);
    gpio_out_set(GPIO_ESP_EN, 1);
    gpio_out_set(GPIO_ESP_BOOT, 1);

    /* Test only the four WiFi SPI nets as plain GPIO, no SPI peripheral. */
    fpioa_set_function(PIN_ESP_SPI_CS, FUNC_GPIO_NUM(KGPIO_CS_OUT));
    fpioa_set_function(PIN_ESP_SPI_CLK, FUNC_GPIO_NUM(KGPIO_CLK_OUT));
    fpioa_set_function(PIN_ESP_SPI_MOSI, FUNC_GPIO_NUM(KGPIO_MOSI_OUT));
    fpioa_set_function(PIN_ESP_SPI_MISO, FUNC_GPIO_NUM(KGPIO_MISO_IN));

    gpio_out_set(KGPIO_CS_OUT, 0);
    gpio_out_set(KGPIO_CLK_OUT, 0);
    gpio_out_set(KGPIO_MOSI_OUT, 0);
    gpio_input_set(KGPIO_MISO_IN);

    LOG("[gpio-test] ready: K210 drives IO0/CS IO1/CLK IO3/MOSI, reads IO2/MISO");
    LOG("[gpio-test] expected ESP: drives GPIO12/MISO, reads GPIO15/CS GPIO14/CLK GPIO13/MOSI");
    diag_line(3, "GPIO link test");
}

static void update_miso_stats(int miso)
{
    if (miso)
        s_miso_seen1++;
    else
        s_miso_seen0++;

    if (s_last_miso < 0) {
        s_last_miso = miso;
    } else if (s_last_miso != miso) {
        s_miso_changes++;
        s_last_miso = miso;
    }
}

static void maybe_log_result(void)
{
    if (s_result_logged || s_step < 32)
        return;
    s_result_logged = 1;
    if (s_miso_changes >= 4 && s_miso_seen0 && s_miso_seen1) {
        LOGF("[gpio-test] RESULT K210_SEES_ESP_MISO_OK changes=%lu seen0=%lu seen1=%lu",
             (unsigned long)s_miso_changes,
             (unsigned long)s_miso_seen0,
             (unsigned long)s_miso_seen1);
        diag_line(4, "MISO GPIO OK");
    } else {
        LOGF("[gpio-test] RESULT K210_MISO_GPIO_FAIL changes=%lu seen0=%lu seen1=%lu",
             (unsigned long)s_miso_changes,
             (unsigned long)s_miso_seen0,
             (unsigned long)s_miso_seen1);
        LOG("[gpio-test] If ESP logs drive_miso toggling but K210 sees no change, IO2/MISO path is broken");
        diag_line(4, "MISO GPIO FAIL");
    }
}

void esp_spi_link_run_forever(void)
{
    amp_set(false);
    gpio_link_init();

    LOG("[gpio-test] waiting ESP UART marker: kesp: spi slave ready");
    while (!esp_uart_log_spi_ready())
        vTaskDelay(pdMS_TO_TICKS(20));
    LOG("[gpio-test] ESP GPIO-ready marker detected, start pin toggles");

    s_last_log_tick = xTaskGetTickCount();
    for (;;) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        s_step++;
        int cs = (s_step & 1) ? 1 : 0;
        int clk = (s_step & 2) ? 1 : 0;
        int mosi = (s_step & 4) ? 1 : 0;
        gpio_out_set(KGPIO_CS_OUT, cs);
        gpio_out_set(KGPIO_CLK_OUT, clk);
        gpio_out_set(KGPIO_MOSI_OUT, mosi);

        int miso = gpio_read(KGPIO_MISO_IN);
        update_miso_stats(miso);

        TickType_t now = xTaskGetTickCount();
        if (now - s_last_log_tick >= pdMS_TO_TICKS(1000)) {
            s_last_log_tick = now;
            LOGF("[gpio-test] drive cs=%d clk=%d mosi=%d read_miso=%d step=%lu miso_chg=%lu seen0=%lu seen1=%lu",
                 cs, clk, mosi, miso,
                 (unsigned long)s_step,
                 (unsigned long)s_miso_changes,
                 (unsigned long)s_miso_seen0,
                 (unsigned long)s_miso_seen1);
            maybe_log_result();
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
