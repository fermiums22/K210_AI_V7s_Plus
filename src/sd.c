/*
 * microSD — SPI1 → spi_sdcard driver → FatFs, mounted at /sd.
 *
 * Pins (pinout.h):  MISO=IO26 (SPI1_D1), CLK=IO27, MOSI=IO28 (SPI1_D0),
 *                   CS=IO29 (GPIOHS29, software CS via /dev/gpio0).
 * IO28 is free for SPI1 — the LCD drives its data over the DVP octet
 * (IO18-25), not IO28, despite the historical pinout comment.
 */
#include "sd.h"
#include "pinout.h"
#include <devices.h>
#include <filesystem.h>
#include <storage/sdcard.h>
#include <fpioa.h>
#include <stdio.h>

bool sd_mount(void)
{
    fpioa_set_function(PIN_SD_MISO, FUNC_SPI1_D1);
    fpioa_set_function(PIN_SD_CLK,  FUNC_SPI1_SCLK);
    fpioa_set_function(PIN_SD_MOSI, FUNC_SPI1_D0);
    fpioa_set_function(PIN_SD_CS,   FUNC_GPIOHS0 + GPIOHS_SD_CS);

    handle_t spi  = io_open("/dev/spi1");
    handle_t gpio = io_open("/dev/gpio0");
    if (!spi || !gpio) {
        printf("[sd] open spi1/gpio0 failed\n");
        return false;
    }

    handle_t sd = spi_sdcard_driver_install(spi, gpio, GPIOHS_SD_CS);
    if (!sd) {
        printf("[sd] card init failed (inserted? wiring?)\n");
        return false;
    }

    /* This SDK's filesystem layer requires the "/fs/<n>/" naming convention
     * (paths are normalized on the "/fs/" marker); "/sd" throws -> -1. */
    int r = filesystem_mount("/fs/0/", sd);
    if (r != 0) {
        printf("[sd] mount rc=%d\n", r);
        return false;
    }
    printf("[sd] mounted at /fs/0/\n");
    return true;
}

int sd_list_root(void)
{
    find_find_data_t fd;
    handle_t h = filesystem_find_first("/fs/0/", "*", &fd);
    if (!h) {
        printf("[sd] root empty or find failed\n");
        return 0;
    }

    int n = 0;
    do {
        printf("[sd]   %s\n", fd.filename);
        n++;
    } while (filesystem_find_next(h, &fd));
    filesystem_find_close(h);

    printf("[sd] %d entries in root\n", n);
    return n;
}
