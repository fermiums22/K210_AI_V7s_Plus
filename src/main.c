#include <FreeRTOS.h>
#include <task.h>
#include "amp.h"
#include "log.h"
#include "esp_spi_link.h"
#include "esp_uart_log.h"

int main(void)
{
    log_init();
    LOG("[pin-test-main] start minimal safe pin tester");

    amp_init();
    amp_set(false);
    LOG("[pin-test-main] amp forced OFF, LCD/audio/SD/KSD skipped");

    esp_uart_log_start();
    LOG("[pin-test-main] ESP UART bridge started");

    esp_spi_link_run_forever();
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
