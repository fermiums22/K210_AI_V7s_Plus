#include <stdio.h>
#include <FreeRTOS.h>
#include <task.h>
#include "lcd.h"
#include "amp.h"
#include "audio.h"
#include "wifi.h"
#include "sd.h"
#include "camera.h"

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
    /* Wipe stale GRAM in BOTH mirror orientations: an orientation change
     * between flashes (without a power cycle) shifts the visible window and
     * leaves ghost text the single-orientation clear can't reach. */
    lcd_set_direction(DIR_YX_RLUD);
    lcd_clear(BLACK);
    lcd_set_direction(DIR_YX_LRUD);
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

    line(116, "CAM  : start...", YELLOW);
    char cam[LW + 1];
    int cam_ok = cam_start(cam, sizeof(cam));
    {
        char b[LW + 1];
        snprintf(b, sizeof(b), "CAM  : %s", cam);
        line(116, b, cam_ok >= 0 ? GREEN : RED);
    }
    printf("[main] cam step done\n");

    line(140, "WiFi : connecting...", YELLOW);
    char ip[20];
    if (wifi_connect(ip, sizeof(ip))) {
        char b[LW + 1];
        line(140, "WiFi : OK", GREEN);
        snprintf(b, sizeof(b), "IP   : %s", ip);
        line(164, b, GREEN);
    } else {
        line(140, "WiFi : FAILED", RED);
    }
    printf("[main] wifi step done\n");

    /* If the camera streams, show a live preview (fills the screen). Returns
     * immediately if the sensor isn't streaming yet (minimal init) — then we
     * just keep the status screen up. */
    if (cam_ok >= 0) {
        printf("[main] camera preview\n");
        cam_preview_forever();
        line(116, "CAM  : GC0328 (no stream)", YELLOW);
    }

    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
