#include <FreeRTOS.h>
#include <task.h>

#include "amp.h"
#include "log.h"
#include "lcd.h"
#include "sd.h"
#include "sd_uart.h"

static int sd_mount_once(void)
{
    amp_set(false);
    if (sd_mount())
        return 1;
    amp_set(false);
    return 0;
}

int main(void)
{
    log_init();
    LOG("[cmd-main] start persistent KSD command service for ESP8285");

    amp_init();
    amp_set(false);
    LOG("[cmd-main] amp forced OFF");

    lcd_init();
    lcd_set_direction(DIR_YX_RLUD);
    lcd_clear(BLACK);
    lcd_set_direction(DIR_YX_LRUD);
    lcd_clear(BLACK);
    amp_set(false);
    LOG("[cmd-main] lcd ok, audio skipped");

    LOG("[cmd-main] sd mount for commandable ESP payload service...");
    int sd_ok = sd_mount_once();
    amp_set(false);

    if (sd_ok) {
        int n = sd_list_root();
        LOGF("[cmd-main] sd ok, root entries=%d", n);
        LOG("[cmd-main] KSD persistent service enabled: GET/PUT/FLASH_ESP/RUN_SPI, no boot window, no K210 reset required");
        sd_uart_service_start();
    } else {
        LOG("[cmd-main] sd mount failed; KSD ESP flashing service is disabled");
    }

    LOG("[cmd-main] idle; send KSD1 commands from PC at any time");
    for (;;) {
        amp_set(false);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
