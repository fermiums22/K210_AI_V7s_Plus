#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESP_FLASH_CONFIG_PATH    "/fs/0/flash.json"
#define ESP_FLASH_DEFAULT_IMAGE  "/fs/0/esp8285_at.bin"
#define ESP_FLASH_DEFAULT_OFFSET 0x00000000u
#define ESP_FLASH_RTOS_BOOT      "/fs/0/esp_boot.bin"
#define ESP_FLASH_RTOS_IROM      "/fs/0/esp_irom.bin"
#define ESP_FLASH_RTOS_INIT      "/fs/0/esp_init.bin"
#define ESP_FLASH_RTOS_BLANK     "/fs/0/esp_blank.bin"
#define ESP_FLASH_RTOS_BOOT_OFF  0x00000000u
#define ESP_FLASH_RTOS_IROM_OFF  0x00020000u
#define ESP_FLASH_RTOS_INIT_OFF  0x000fc000u
#define ESP_FLASH_RTOS_BLANK_OFF 0x000fe000u

bool esp_flash_file(const char *path, uint32_t offset);
bool esp_flash_run_if_requested(void);
