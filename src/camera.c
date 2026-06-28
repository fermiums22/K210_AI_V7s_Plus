/*
 * DVP camera — Maix Dock FPC. Sensor confirmed GC0328 (chip id reg 0xF0=0x9d).
 *
 * Pin map (verified by pin/reset sweep on this board):
 *   IO40 SCCB_SDA   IO41 SCCB_SCLK
 *   IO42 CMOS_RST   IO43 CMOS_VSYNC  IO44 CMOS_PWDN
 *   IO45 CMOS_HREF  IO46 CMOS_XCLK   IO47 CMOS_PCLK
 * DVP data D0-D7 are dedicated pads, routed by the DVP peripheral.
 *
 * Notes that cost real debugging time:
 *   - XCLK only starts inside dvp_config() (it sets CMOS_CLK_ENABLE) — the
 *     sensor's SCCB is dead without that master clock.
 *   - After reset release the GC0328 needs ~100 ms to boot; reading the id
 *     too early returns 0x00 even though the part ACKs at 0x42.
 *
 * Stage 1 (this file): bring up + confirm the sensor. Full register init and
 * streaming to the LCD is the next step.
 */
#include "camera.h"
#include <devices.h>
#include <fpioa.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define GC0328_ADDR  0x42      /* 0x21 << 1, 8-bit SCCB write address */

static void cam_pins(void)
{
    fpioa_set_function(40, FUNC_SCCB_SDA);
    fpioa_set_function(41, FUNC_SCCB_SCLK);
    fpioa_set_function(42, FUNC_CMOS_RST);
    fpioa_set_function(43, FUNC_CMOS_VSYNC);
    fpioa_set_function(44, FUNC_CMOS_PWDN);
    fpioa_set_function(45, FUNC_CMOS_HREF);
    fpioa_set_function(46, FUNC_CMOS_XCLK);
    fpioa_set_function(47, FUNC_CMOS_PCLK);
}

int cam_probe(char *name_out, int len)
{
    cam_pins();

    handle_t dvp = io_open("/dev/dvp0");
    handle_t sccb = io_open("/dev/sccb0");
    if (!dvp || !sccb) {
        printf("[cam] open dvp/sccb failed\n");
        snprintf(name_out, len, "no DVP/SCCB");
        return -1;
    }

    dvp_xclk_set_clock_rate(dvp, 24000000);
    dvp_config(dvp, 320, 240, false);          /* starts XCLK */

    dvp_set_signal(dvp, DVP_SIG_POWER_DOWN, false);
    usleep(20 * 1000);
    dvp_set_signal(dvp, DVP_SIG_RESET, false);  /* assert  */
    usleep(20 * 1000);
    dvp_set_signal(dvp, DVP_SIG_RESET, true);   /* release */
    usleep(120 * 1000);                         /* GC0328 boot time */

    handle_t gc = sccb_get_device(sccb, GC0328_ADDR, 8);
    /* The first reads after reset can come back 0x00 while the GC0328 is still
     * waking; poll until the id register settles (or we give up). */
    uint8_t id = 0;
    for (int i = 0; i < 12; i++) {
        id = sccb_dev_read_byte(gc, 0xF0);
        if (id == 0x9d || id == 0x9b) break;
        usleep(30 * 1000);
    }
    printf("[cam] GC0328 chip id = 0x%02x\n", id);

    if (id == 0x9d || id == 0x9b) {
        snprintf(name_out, len, "%s id=0x%02x",
                 id == 0x9d ? "GC0328" : "GC0308", id);
        return id;
    }

    snprintf(name_out, len, "unknown (0x%02x)", id);
    return -1;
}
