/*
 * microSD - SPI1 -> spi_sdcard driver -> FatFs, mounted at /fs/0/.
 *
 * Pins (pinout.h): MISO=IO26 (SPI1_D1), CLK=IO27, MOSI=IO28 (SPI1_D0),
 *                  CS=IO29 (GPIOHS29, software CS via /dev/gpio0).
 *
 * This intentionally stays close to the historically verified path from
 * commit 3f524d7 (SD mounted and listed root entries).  Do not touch
 * SPI0/DVP mux here: LCD uses its own init path, and SD must only force the
 * four SD FPIOA functions below before opening /dev/spi1.
 */
#include "sd.h"
#include "pinout.h"
#include "log.h"

#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <filesystem.h>
#include <storage/sdcard.h>
#include <fpioa.h>
#include <ff.h>
#include <string.h>

#define SD_MOUNT_ATTEMPTS        1u
#define SD_POWERUP_DELAY_MS      1500u
#define SD_MOUNT_REINIT_DELAY_MS 1u
#define SD_RAW_SPI_CLOCK_HZ      200000u

static handle_t s_spi;
static handle_t s_gpio;
static handle_t s_sd;
static int s_mounted;
static int s_powerup_wait_done;
static uint8_t s_mkfs_work[4096] __attribute__((aligned(64)));

static TickType_t ms_to_ticks_min(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0)
        ticks = 1;
    return ticks;
}

static void sd_powerup_wait_once(void)
{
    if (s_powerup_wait_done)
        return;
    s_powerup_wait_done = 1;
    LOGF("[sd] power-up settle %lu ms", (unsigned long)SD_POWERUP_DELAY_MS);
    vTaskDelay(ms_to_ticks_min(SD_POWERUP_DELAY_MS));
}

static void sd_pinmux(void)
{
    fpioa_set_function(PIN_SD_MISO, FUNC_SPI1_D1);
    fpioa_set_function(PIN_SD_CLK,  FUNC_SPI1_SCLK);
    fpioa_set_function(PIN_SD_MOSI, FUNC_SPI1_D0);
    fpioa_set_function(PIN_SD_CS,   FUNC_GPIOHS0 + GPIOHS_SD_CS);
    LOG("[sd] SD pinmux SPI1");
}

static void sd_driver_drop(void)
{
    s_mounted = 0;
    if (s_sd) {
        io_close(s_sd);
        s_sd = 0;
    }
    if (s_spi) {
        io_close(s_spi);
        s_spi = 0;
    }
    if (s_gpio) {
        io_close(s_gpio);
        s_gpio = 0;
    }
}

static uint8_t raw_xfer8(handle_t spi_dev, uint8_t tx)
{
    uint8_t rx = 0xff;
    int n = spi_dev_transfer_full_duplex(spi_dev, &tx, 1, &rx, 1);
    if (n != 1)
        return 0xfe;
    return rx;
}

int sd_raw_cmd0_probe(void)
{
    sd_powerup_wait_once();
    sd_pinmux();

    handle_t spi = io_open("/dev/spi1");
    handle_t gpio = io_open("/dev/gpio0");
    if (!spi || !gpio) {
        LOG("[sdraw] open spi1/gpio0 failed");
        if (spi) io_close(spi);
        if (gpio) io_close(gpio);
        return 0xfe;
    }

    gpio_set_drive_mode(gpio, GPIOHS_SD_CS, GPIO_DM_OUTPUT);
    gpio_set_pin_value(gpio, GPIOHS_SD_CS, GPIO_PV_HIGH);

    handle_t dev = spi_get_device(spi, SPI_MODE_0, SPI_FF_STANDARD, 1, 8);
    if (!dev) {
        LOG("[sdraw] spi_get_device failed");
        io_close(gpio);
        io_close(spi);
        return 0xfd;
    }
    spi_dev_set_clock_rate(dev, SD_RAW_SPI_CLOCK_HZ);

    LOG("[sdraw] raw full-duplex CMD0 probe begin");
    for (int i = 0; i < 10; i++)
        raw_xfer8(dev, 0xff);

    gpio_set_pin_value(gpio, GPIOHS_SD_CS, GPIO_PV_LOW);
    const uint8_t cmd0[6] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };
    uint8_t rx[6];
    memset(rx, 0xff, sizeof(rx));
    int n = spi_dev_transfer_full_duplex(dev, cmd0, sizeof(cmd0), rx, sizeof(rx));
    LOGF("[sdraw] CMD0 txrx n=%d rx=%02x %02x %02x %02x %02x %02x", n,
         rx[0], rx[1], rx[2], rx[3], rx[4], rx[5]);

    uint8_t r = 0xff;
    for (int i = 0; i < 16; i++) {
        r = raw_xfer8(dev, 0xff);
        if (r != 0xff)
            break;
    }
    raw_xfer8(dev, 0xff);
    gpio_set_pin_value(gpio, GPIOHS_SD_CS, GPIO_PV_HIGH);
    raw_xfer8(dev, 0xff);

    LOGF("[sdraw] CMD0 r=%02x", r);
    io_close(dev);
    io_close(gpio);
    io_close(spi);
    return (int)r;
}

static handle_t sd_driver_open_once(void)
{
    sd_powerup_wait_once();
    sd_pinmux();

    s_spi = io_open("/dev/spi1");
    s_gpio = io_open("/dev/gpio0");
    if (!s_spi || !s_gpio) {
        LOG("[sd] open spi1/gpio0 failed");
        sd_driver_drop();
        return 0;
    }

    s_sd = spi_sdcard_driver_install(s_spi, s_gpio, GPIOHS_SD_CS);
    if (!s_sd) {
        LOG("[sd] card init failed (inserted? wiring?)");
        sd_driver_drop();
        return 0;
    }

    LOG("[sd] card driver handle ready");
    return s_sd;
}

static handle_t sd_driver(void)
{
    if (s_sd)
        return s_sd;
    return sd_driver_open_once();
}

bool sd_mount(void)
{
    if (s_mounted) {
        LOG("[sd] already mounted at /fs/0/");
        return true;
    }

    for (uint32_t attempt = 1; attempt <= SD_MOUNT_ATTEMPTS; attempt++) {
        handle_t sd = sd_driver();
        if (!sd) {
            LOGF("[sd] driver unavailable attempt %lu/%lu", (unsigned long)attempt,
                 (unsigned long)SD_MOUNT_ATTEMPTS);
            sd_driver_drop();
            vTaskDelay(ms_to_ticks_min(SD_MOUNT_REINIT_DELAY_MS));
            continue;
        }

        /* This SDK's filesystem layer requires the /fs/<n>/ naming convention. */
        int r = filesystem_mount("/fs/0/", sd);
        if (r == 0) {
            s_mounted = 1;
            LOG("[sd] mounted at /fs/0/");
            return true;
        }

        LOGF("[sd] mount rc=%d attempt %lu/%lu", r, (unsigned long)attempt,
             (unsigned long)SD_MOUNT_ATTEMPTS);
        sd_driver_drop();
        vTaskDelay(ms_to_ticks_min(SD_MOUNT_REINIT_DELAY_MS));
    }

    LOG("[sd] mount failed after reinit attempts");
    return false;
}

bool sd_format(void)
{
    if (!sd_mount())
        return false;

    LOG("[sd] FORMAT_SD requested: destructive FatFs format");

    memset(s_mkfs_work, 0, sizeof(s_mkfs_work));

    FRESULT fr = f_mkfs("0:", FM_ANY, 0, s_mkfs_work, sizeof(s_mkfs_work));
    if (fr != FR_OK) {
        LOGF("[sd] f_mkfs failed fr=%d", (int)fr);
        return false;
    }

    LOG("[sd] f_mkfs OK, mounting again");
    sd_list_root();
    return true;
}

int sd_list_root(void)
{
    find_find_data_t fd;
    handle_t h = filesystem_find_first("/fs/0/", "*", &fd);
    if (!h) {
        LOG("[sd] root empty or find failed");
        return 0;
    }

    int n = 0;
    do {
        LOGF("[sd]   %s", fd.filename);
        n++;
    } while (filesystem_find_next(h, &fd));
    filesystem_find_close(h);

    LOGF("[sd] %d entries in root", n);
    return n;
}
