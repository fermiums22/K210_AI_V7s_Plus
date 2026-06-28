#include <stdio.h>
#include <FreeRTOS.h>
#include <task.h>
#include "lcd.h"
#include "amp.h"
#include "audio.h"
#include "wifi.h"

int main(void)
{
    printf("[main] start\n");
    amp_init();
    printf("[main] amp ok\n");

    lcd_init();
    lcd_clear(BLACK);
    lcd_draw_string(40, 40, "K210 LCD OK", WHITE);
    printf("[main] lcd ok\n");

    audio_init();
    lcd_draw_string(40, 70, "AUDIO: chime...", YELLOW);
    audio_test();
    lcd_draw_string(40, 70, "AUDIO: done     ", GREEN);
    printf("[main] audio ok\n");

    lcd_draw_string(40, 100, "WiFi: connecting...", YELLOW);
    char ip[20];
    if (wifi_connect(ip, sizeof(ip))) {
        char line[40];
        snprintf(line, sizeof(line), "WiFi OK  IP:%s", ip);
        lcd_draw_string(40, 100, "                          ", BLACK);
        lcd_draw_string(40, 100, line, GREEN);
        lcd_draw_string(40, 130, "SSID: Fermiums_2.4", WHITE);
    } else {
        lcd_draw_string(40, 100, "WiFi: FAILED         ", RED);
    }
    printf("[main] wifi step done\n");

    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
