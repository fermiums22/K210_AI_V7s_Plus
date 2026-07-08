#pragma once

#include <stdbool.h>

#define APP_FLASH_SLOT0_OFFSET 0x00100000u

bool app_flash_slot0_from_sd(const char *rel_path, bool verify);
