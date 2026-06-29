#include <FreeRTOS.h>
#include <task.h>
#include "lcd.h"
#include "amp.h"
#include "audio.h"
#include "wifi.h"
#include "sd.h"
#include "show.h"
#include "log.h"

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
    LOG("[main] lcd ok");

    audio_init();
    amp_set(false);
    LOG("[main] audio ok");

    LOG("[main] sd mount...");
    int sd_ok = 0;
    if (sd_mount()) {
        sd_ok = 1;
        int n = sd_list_root();
        LOGF("[main] sd ok, root entries=%d", n);
    } else {
        LOG("[main] sd failed");
    }
    LOG("[main] sd step done");

    if (sd_ok) {
        amp_set(false);
        LOG("[main] wifi connecting...");
        char ip[20];
        if (wifi_connect(ip, sizeof(ip))) {
            LOGF("[main] wifi ok ip=%s", ip);
            LOG("[main] sync from PC 192.168.0.10:8888...");
            if (wifi_pull_files("192.168.0.10", 8888))
                LOG("[main] sync ok");
            else
                LOG("[main] sync skipped/failed");
        } else {
            LOG("[main] wifi failed");
        }
    }

    amp_set(false);
    LOG("[main] show mode");
    show_run_forever();

    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
