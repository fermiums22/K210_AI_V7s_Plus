#include "esp_spi_link.h"
#include "pinout.h"
#include "log.h"
#include "amp.h"
#include "esp_uart_log.h"

#include <FreeRTOS.h>
#include <task.h>
#include <fpioa.h>
#include <platform.h>
#include <stdio.h>

#define GPIOHS_CS       0
#define GPIOHS_CLK      1
#define GPIOHS_MOSI     2
#define GPIOHS_MISO     3
#define GPIOHS_BIT(n)   (1u << (n))
#define FUNC_GPIOHS_NUM(n) ((fpioa_function_t)(24 + (n)))

typedef struct {
    volatile uint32_t input_val;
    volatile uint32_t input_en;
    volatile uint32_t output_en;
    volatile uint32_t output_val;
    volatile uint32_t pue;
    volatile uint32_t ds;
    volatile uint32_t rise_ie;
    volatile uint32_t rise_ip;
    volatile uint32_t fall_ie;
    volatile uint32_t fall_ip;
    volatile uint32_t high_ie;
    volatile uint32_t high_ip;
    volatile uint32_t low_ie;
    volatile uint32_t low_ip;
    volatile uint32_t iof_en;
    volatile uint32_t iof_sel;
    volatile uint32_t out_xor;
} gpiohs_regs_t;

static gpiohs_regs_t *const GPIOHS = (gpiohs_regs_t *)GPIOHS_BASE_ADDR;

static uint32_t s_step;
static int s_last_miso = -1;
static uint32_t s_miso_changes;
static uint32_t s_miso_seen0;
static uint32_t s_miso_seen1;
static uint32_t s_result_logged;
static TickType_t s_last_log_tick;

void esp_spi_link_pause(int pause)
{
    (void)pause;
}

static void gpiohs_write(int pin, int val)
{
    uint32_t bit = GPIOHS_BIT(pin);
    if (val)
        GPIOHS->output_val |= bit;
    else
        GPIOHS->output_val &= ~bit;
}

static int gpiohs_read(int pin)
{
    return (GPIOHS->input_val & GPIOHS_BIT(pin)) ? 1 : 0;
}

static void gpiohs_link_init(void)
{
    uint32_t out_mask = GPIOHS_BIT(GPIOHS_CS) | GPIOHS_BIT(GPIOHS_CLK) | GPIOHS_BIT(GPIOHS_MOSI);
    uint32_t in_mask = GPIOHS_BIT(GPIOHS_MISO);
    uint32_t all_mask = out_mask | in_mask;

    fpioa_set_function(PIN_ESP_SPI_CS, FUNC_GPIOHS_NUM(GPIOHS_CS));
    fpioa_set_function(PIN_ESP_SPI_CLK, FUNC_GPIOHS_NUM(GPIOHS_CLK));
    fpioa_set_function(PIN_ESP_SPI_MOSI, FUNC_GPIOHS_NUM(GPIOHS_MOSI));
    fpioa_set_function(PIN_ESP_SPI_MISO, FUNC_GPIOHS_NUM(GPIOHS_MISO));

    GPIOHS->iof_en &= ~all_mask;
    GPIOHS->out_xor &= ~all_mask;
    GPIOHS->pue &= ~all_mask;
    GPIOHS->output_val &= ~out_mask;
    GPIOHS->output_en |= out_mask;
    GPIOHS->output_en &= ~in_mask;
    GPIOHS->input_en |= in_mask;

    LOG("[pin-test] GPIOHS ready, no SPI peripheral and no shared GPIO peripheral");
    LOG("[pin-test] K210 drives IO0/CS IO1/CLK IO3/MOSI via GPIOHS0..2");
    LOG("[pin-test] K210 reads IO2/MISO via GPIOHS3");
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
        LOGF("[pin-test] RESULT K210_SEES_ESP_MISO_OK changes=%lu seen0=%lu seen1=%lu",
             (unsigned long)s_miso_changes,
             (unsigned long)s_miso_seen0,
             (unsigned long)s_miso_seen1);
    } else {
        LOGF("[pin-test] RESULT K210_MISO_GPIO_FAIL changes=%lu seen0=%lu seen1=%lu",
             (unsigned long)s_miso_changes,
             (unsigned long)s_miso_seen0,
             (unsigned long)s_miso_seen1);
        LOG("[pin-test] If ESP drive_miso toggles but K210 sees no change, IO2/MISO path is broken");
    }
}

void esp_spi_link_run_forever(void)
{
    amp_set(false);
    gpiohs_link_init();

    LOG("[pin-test] waiting ESP UART marker: kesp: spi slave ready");
    while (!esp_uart_log_spi_ready()) {
        amp_set(false);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    LOG("[pin-test] ESP GPIO-ready marker detected, start pin toggles");

    s_last_log_tick = xTaskGetTickCount();
    for (;;) {
        amp_set(false);

        s_step++;
        int cs = (s_step & 1) ? 1 : 0;
        int clk = (s_step & 2) ? 1 : 0;
        int mosi = (s_step & 4) ? 1 : 0;
        gpiohs_write(GPIOHS_CS, cs);
        gpiohs_write(GPIOHS_CLK, clk);
        gpiohs_write(GPIOHS_MOSI, mosi);

        int miso = gpiohs_read(GPIOHS_MISO);
        update_miso_stats(miso);

        TickType_t now = xTaskGetTickCount();
        if (now - s_last_log_tick >= pdMS_TO_TICKS(1000)) {
            s_last_log_tick = now;
            LOGF("[pin-test] drive cs=%d clk=%d mosi=%d read_miso=%d step=%lu miso_chg=%lu seen0=%lu seen1=%lu",
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
