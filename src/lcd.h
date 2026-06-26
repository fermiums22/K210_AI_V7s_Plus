#pragma once
#include <stdint.h>

void lcd_init(void);
void lcd_fill(uint16_t color);                          /* fill whole screen    */
void lcd_fill_rect(int x, int y, int w, int h,
                   uint16_t color);
void lcd_set_window(int x, int y, int w, int h);        /* prepare write window */
void lcd_write_pixels(const uint16_t *buf, int count);  /* stream RGB565 data   */

/* RGB888 → RGB565 helper */
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
