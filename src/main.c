#include <FreeRTOS.h>
#include <task.h>

#include <stdio.h>
#include <string.h>

#include "amp.h"
#include "log.h"
#include "lcd.h"
#include "sd.h"
#include "sd_uart.h"

#define SCREEN_ROWS 18
#define SCREEN_Y0   0
#define SCREEN_DY   16

static int s_screen_ready;
static int s_row;

static void screen_clear(void)
{
    lcd_clear(BLACK);
    lcd_draw_string_bg(0, 0, "K210 CAM/SD TEST", YELLOW, BLACK);
    lcd_draw_string_bg(0, 16, "UART: COM8 921600", CYAN, BLACK);
    s_row = 2;
}

static void screen_line_color(const char *s, uint16_t color)
{
    if (!s_screen_ready)
        return;

    if (s_row >= SCREEN_ROWS)
        screen_clear();

    char line[40];
    memset(line, ' ', sizeof(line));
    line[sizeof(line) - 1] = 0;
    size_t n = strlen(s);
    if (n > sizeof(line) - 1)
        n = sizeof(line) - 1;
    memcpy(line, s, n);

    lcd_draw_string_bg(0, SCREEN_Y0 + s_row * SCREEN_DY, line, color, BLACK);
    s_row++;
}

static void say(const char *s)
{
    LOG(s);
    screen_line_color(s, WHITE);
}

static void ok(const char *s)
{
    char b[96];
    snprintf(b, sizeof(b), "[OK] %s", s);
    LOG(b);
    screen_line_color(b, GREEN);
}

int main(void)
{
    log_init();
    LOG("[stack] K210 camera/SD test boot");
    log_dump_uart_clock();
    ok("UART LOG PC");

    amp_init();
    amp_set(false);
    ok("Audio AMP off");

    /* SD and LCD share the IO28 area on this board.  For the KSD bring-up
     * stage, keep LCD uninitialized so SPI0/DVP cannot steal the SD bus before
     * SD_TEST.  Once SD is stable, LCD can be re-enabled after the SD mount. */
    ok("LCD deferred for SD bus");
    ok("SD mount deferred");
    ok("Camera lazy start");

    sd_uart_service_start();
    ok("PC UART KSD listener");
    say("Idle. Use CAM_CAPTURE capture.rgb565");

    for (;;) {
        amp_set(false);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
