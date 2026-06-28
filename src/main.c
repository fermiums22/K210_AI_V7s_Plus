#include <stdio.h>
#include <FreeRTOS.h>
#include <task.h>
#include "lcd.h"
#include "amp.h"
#include "audio.h"
#include "wifi.h"
#include "sd.h"

#define LX 20            /* left margin */
#define LW 26            /* fixed line width (chars) — pads to clear old text */

/* Opaque, fixed-width status line: repaints the whole row so updates never
 * leave ghosts of the previous text. */
static void line(int y, const char *s, uint16_t color)
{
    char buf[LW + 1];
    int i = 0;
    for (; s[i] && i < LW; i++) buf[i] = s[i];
    for (; i < LW; i++)         buf[i] = ' ';
    buf[LW] = 0;
    lcd_draw_string_bg(LX, y, buf, color, BLACK);
}

int main(void)
{
    printf("[main] start\n");
    amp_init();

    lcd_init();
    lcd_clear(BLACK);
    line(20, "K210 V7s+  boot", WHITE);
    line(44, "LCD  : OK", GREEN);
    printf("[main] lcd ok\n");

    audio_init();
    line(68, "AUDIO: chime...", YELLOW);
    audio_test();
    line(68, "AUDIO: OK", GREEN);
    printf("[main] audio ok\n");

    line(92, "SD   : mount...", YELLOW);
    if (sd_mount()) {
        int n = sd_list_root();
        char b[LW + 1];
        snprintf(b, sizeof(b), "SD   : OK (%d files)", n);
        line(92, b, GREEN);
    } else {
        line(92, "SD   : FAILED", RED);
    }
    printf("[main] sd step done\n");

    line(116, "WiFi : connecting...", YELLOW);
    char ip[20];
    if (wifi_connect(ip, sizeof(ip))) {
        char b[LW + 1];
        line(116, "WiFi : OK", GREEN);
        snprintf(b, sizeof(b), "IP   : %s", ip);
        line(140, b, GREEN);
    } else {
        line(116, "WiFi : FAILED", RED);
    }
    printf("[main] wifi step done\n");

    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
