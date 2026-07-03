#pragma once

void esp_uart_log_start(void);
int esp_uart_log_spi_ready(void);
int esp_uart_log_put_active(void);
void esp_uart_log_clear_put_active(void);
