#include <FreeRTOS.h>
#include <task.h>
#include "lcd.h"
#include "amp.h"
#include "audio.h"
#include "wifi.h"
#include "sd.h"
#include "show.h"
#include "log.h"
#include "esp_flasher.h"
#include "sd_uart.h"
#include "diag_screen.h"
#include "esp_spi_link.h"
#include "esp_uart_log.h"

int main(void)
{
    log_init();
    LOG("[main] start");

    amp_init();
    amp_set(false);

    lcd_init();
    lcd_set_direction(DIR_YX_RLUD);
    lcd_clear(BLACK);
    lcd_set_direction(DIR_YX_LRUD);
    lcd_clear(BLACK);
    diag_clear("ESP / SD DIAGNOSTICS");
    diag_line(1, "Amp: OFF");
    LOG("[main] lcd ok");

    audio_init();
    amp_set(false);
    diag_line(2, "Audio init OK, amp muted");
    LOG("[main] audio ok");

    LOG("[main] sd mount...");
    amp_set(false);
    diag_line(3, "SD mount...");
    int sd_ok = 0;
    if (sd_mount()) {
        sd_ok = 1;
        int n = sd_list_root();
        LOGF("[main] sd ok, root entries=%d", n);
        diag_printf(3, "SD OK, root entries=%d", n);
        amp_set(false);

        /* Initial short KSD window for a PC helper that catches the boot.
         * After boot, sd_uart_service_start() keeps the same command protocol
         * alive in the background, so the normal ESP-only workflow no longer
         * needs to reset K210 just to upload files or start ESP flashing. */
        sd_uart_receive_window(1500);

        esp_flash_run_if_requested();
        sd_uart_service_start();
    } else {
        LOG("[main] sd failed");
        diag_line(3, "SD FAILED");
    }
    LOG("[main] sd step done");

    amp_set(false);
    LOG("[main] entering WiFi/SPI receiver");
    diag_line(12, "WiFi/SPI receiver");
    esp_uart_log_start();
    esp_spi_link_run_forever();
}
