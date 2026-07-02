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
        if (!esp_flash_marker_present())
            sd_uart_receive_window(120000);
        amp_set(false);
        esp_flash_run_if_requested();
    } else {
        LOG("[main] sd failed");
        diag_line(3, "SD FAILED");
    }
    LOG("[main] sd step done");

    amp_set(false);
    LOG("[main] diagnostics idle");
    diag_line(12, "Diagnostics idle. Show disabled.");

    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
