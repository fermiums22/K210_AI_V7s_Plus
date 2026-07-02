#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESP_FLASH_DEFAULT_MARKER "/fs/0/flash_now"
#define ESP_FLASH_DEFAULT_IMAGE  "/fs/0/esp8285_at.bin"
#define ESP_FLASH_DEFAULT_OFFSET 0x00000000u

bool esp_flash_file(const char *path, uint32_t offset);
bool esp_flash_marker_present(void);
bool esp_flash_run_if_requested(void);
