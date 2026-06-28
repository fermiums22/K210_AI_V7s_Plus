#include <stdio.h>
#include <FreeRTOS.h>
#include <task.h>
#include "lcd.h"
#include "amp.h"

int main(void)
{
    printf("[main] start\n");
    amp_init();
    printf("[main] amp ok\n");

    lcd_init();
    lcd_clear(BLACK);
    lcd_draw_string(40, 100, "K210 LCD OK", WHITE);
    lcd_draw_string(40, 130, "SDK device-manager driver", GREEN);
    printf("[main] lcd ok\n");

    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
