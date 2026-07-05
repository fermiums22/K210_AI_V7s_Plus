#include "diag_screen.h"
#include "lcd.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DIAG_COLS 40
#define DIAG_LINE_H 16

static int s_diag_lcd_enabled;

void diag_screen_set_enabled(int enabled)
{
    s_diag_lcd_enabled = enabled ? 1 : 0;
}

static void fit_line(char out[DIAG_COLS + 1], const char *text)
{
    int i = 0;
    if (text) {
        for (; i < DIAG_COLS && text[i]; i++)
            out[i] = text[i];
    }
    for (; i < DIAG_COLS; i++)
        out[i] = ' ';
    out[DIAG_COLS] = 0;
}

void diag_clear(const char *title)
{
    if (!s_diag_lcd_enabled)
        return;
    lcd_clear(BLACK);
    diag_line(0, title ? title : "Diagnostics");
}

void diag_line(int line, const char *text)
{
    if (!s_diag_lcd_enabled)
        return;
    if (line < 0 || line >= 15)
        return;

    char buf[DIAG_COLS + 1];
    fit_line(buf, text);
    lcd_draw_string_bg(0, (uint16_t)(line * DIAG_LINE_H), buf,
                       line == 0 ? YELLOW : WHITE, BLACK);
}

void diag_printf(int line, const char *fmt, ...)
{
    if (!s_diag_lcd_enabled)
        return;

    char msg[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = 0;

    diag_line(line, msg);
}
