#pragma once
#include <stdint.h>
int k210_flash_erase_4k(uint32_t offset);
int k210_flash_program(uint32_t offset, const void *data, uint32_t size);
int k210_flash_read(uint32_t offset, void *data, uint32_t size);
