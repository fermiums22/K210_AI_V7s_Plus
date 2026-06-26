#include <stdio.h>
#include <stdint.h>
#include <FreeRTOS.h>
#include <task.h>

#include "pinout.h"
#include "lcd.h"
#include "amp.h"

static void main_task(void *arg)
{
    (void)arg;

    printf("[boot] amp init\n");
    amp_init();           /* IO9/IO10 LOW → amp silent from the very start    */

    printf("[boot] lcd init\n");
    lcd_init();           /* fills screen blue on success                      */
    printf("[boot] lcd OK\n");

    /* Pulse backlight 3× to prove GPIO works */
    for (int i = 0; i < 3; i++) {
        /* BL off */
        volatile gpio_t *gpio = (volatile gpio_t *)GPIO_BASE_ADDR;
        gpio->output_val.u32[0] &= ~(1u << GPIO_LCD_BL);
        vTaskDelay(pdMS_TO_TICKS(200));
        /* BL on */
        gpio->output_val.u32[0] |=  (1u << GPIO_LCD_BL);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Draw a colourful test pattern */
    lcd_fill_rect(0,   0,   LCD_W/2, LCD_H/2, rgb(255, 0,   0  ));  /* R */
    lcd_fill_rect(LCD_W/2, 0,   LCD_W/2, LCD_H/2, rgb(0,   255, 0  ));  /* G */
    lcd_fill_rect(0,   LCD_H/2, LCD_W/2, LCD_H/2, rgb(0,   0,   255));  /* B */
    lcd_fill_rect(LCD_W/2, LCD_H/2, LCD_W/2, LCD_H/2, rgb(255, 255, 0  ));  /* Y */

    printf("[boot] display test pattern drawn\n");
    printf("[boot] ready. amp stays silent (no audio task yet).\n");

    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

int main(void)
{
    xTaskCreate(main_task, "main", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    vTaskStartScheduler();
    while (1) ;
}
