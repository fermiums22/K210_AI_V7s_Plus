#include <FreeRTOS.h>
#include <task.h>

#include "amp.h"
#include "log.h"
#include "lcd.h"
#include "sd.h"
#include "sd_uart.h"
#include "esp_flasher.h"
#include "esp_spi_link.h"
#include "esp_uart_log.h"

static int sd_mount_with_retries(void)
{
    const int attempts = 50;
    for (int i = 0; i < attempts; i++) {
        if (i > 0) {
            LOGF("[spi-loader-main] sd mount retry %d/%d", i + 1, attempts);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        amp_set(false);
        if (sd_mount())
            return 1;
        amp_set(false);
    }
    return 0;
}

int main(void)
{
    log_init();
    LOG("[spi-loader-main] start safe one-shot ESP loader + pure SPI scanner");

    /* Absolute first hardware action after logging: force the amplifier off. */
    amp_init();
    amp_set(false);
    LOG("[spi-loader-main] amp forced OFF");

    /* LCD is used only because the existing KSD/ESP flasher diagnostics call
     * diag_line()/diag_printf().  Audio is deliberately not initialized. */
    lcd_init();
    lcd_set_direction(DIR_YX_RLUD);
    lcd_clear(BLACK);
    lcd_set_direction(DIR_YX_LRUD);
    lcd_clear(BLACK);
    amp_set(false);
    LOG("[spi-loader-main] lcd ok, audio skipped");

    LOG("[spi-loader-main] sd mount for optional ESP payload upload...");
    int sd_ok = sd_mount_with_retries();
    amp_set(false);

    if (sd_ok) {
        int n = sd_list_root();
        LOGF("[spi-loader-main] sd ok, root entries=%d", n);

        /* One K210 firmware now does both jobs:
         *  - accept ESP payload over KSD and FLASH_ESP, if the PC helper connects;
         *  - then continue into the pure SPI peripheral scanner.
         * The SPI scanner itself does not use SD/KSD/Wi-Fi/TCP. */
        LOG("[spi-loader-main] KSD loader window 20000 ms; send RUN_SPI to start immediately");
        sd_uart_receive_window(20000);

        /* Compatibility path: if a helper only uploaded flash.json + DONE/RUN_SPI
         * without issuing FLASH_ESP, run a pending one-shot ESP job here. */
        esp_flash_run_if_requested();
    } else {
        LOG("[spi-loader-main] sd mount failed after retries; skip ESP upload window and test current ESP firmware");
    }

    amp_set(false);
    LOG("[spi-loader-main] starting ESP UART bridge and pure SPI scanner");
    esp_uart_log_start();
    esp_spi_link_run_forever();

    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
