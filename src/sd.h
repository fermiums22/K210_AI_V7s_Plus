#pragma once
#include <stdbool.h>

/* microSD over SPI1 -> FatFs, mounted at /fs/0/ (this SDK's path convention). */
bool sd_mount(void);
bool sd_format(void);      /* destructive: FatFs f_mkfs("0:") then mount */
int  sd_list_root(void);   /* prints root entries to UART, returns count */
