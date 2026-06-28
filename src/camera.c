/*
 * DVP camera — Maix Dock FPC, sensor GC0328 (chip id reg 0xF0 = 0x9d).
 *
 * Pin map (verified by sweep on this board):
 *   IO40 SCCB_SDA   IO41 SCCB_SCLK
 *   IO42 CMOS_RST   IO43 CMOS_VSYNC  IO44 CMOS_PWDN
 *   IO45 CMOS_HREF  IO46 CMOS_XCLK   IO47 CMOS_PCLK
 * DVP data D0-D7 are dedicated pads, routed by the DVP peripheral.
 *
 * Gotchas paid for in debugging:
 *   - XCLK only starts inside dvp_config() (it sets CMOS_CLK_ENABLE).
 *   - GC0328 needs ~100 ms after reset; id reads 0x00 until it wakes.
 *
 * The register init here is intentionally minimal (reset, RGB565 output,
 * output enable, auto-exposure). It produces a live image; full image-quality
 * tuning (AWB/gamma/lens) is a later refinement.
 */
#include "camera.h"
#include "lcd.h"
#include "gc0328_regs.h"
#include <devices.h>
#include <dvp.h>
#include <platform.h>
#include <fpioa.h>
#include <FreeRTOS.h>
#include <task.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define GC0328_ADDR  0x42      /* device-manager SCCB read works at 0x42 (8-bit) */
#define CAM_W        320
#define CAM_H        240

static handle_t s_dvp, s_sccb;
static uint32_t s_fb[CAM_W * CAM_H / 2] __attribute__((aligned(64)));  /* RGB565 display */
static uint8_t  s_ai[CAM_W * CAM_H * 3] __attribute__((aligned(64)));  /* RGB24 planar (AI) */
static volatile int s_frame_done;

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

static void on_frame(dvp_frame_event_t event, void *userdata)
{
    if (event == VIDEO_FE_END)
        s_frame_done = 1;
}


int cam_start(char *name_out, int len)
{
    cam_pins();

    s_dvp = io_open("/dev/dvp0");
    s_sccb = io_open("/dev/sccb0");
    if (!s_dvp || !s_sccb) {
        snprintf(name_out, len, "no DVP/SCCB");
        return -1;
    }

    dvp_xclk_set_clock_rate(s_dvp, 24000000);
    dvp_config(s_dvp, CAM_W, CAM_H, true);          /* auto_enable: continuous */

    dvp_set_signal(s_dvp, DVP_SIG_POWER_DOWN, false);
    usleep(20 * 1000);

    handle_t gc = sccb_get_device(s_sccb, GC0328_ADDR, 8);

    /* The GC0328 start-up is flaky after an ISP soft-reboot (not a real power
     * cycle): id can read 0xFF (no comms) or 0x00 (still in reset). Re-pulse
     * reset a few times and poll until the id register settles. */
    uint8_t id = 0;
    for (int attempt = 0; attempt < 4 && id != 0x9d && id != 0x9b; attempt++) {
        dvp_set_signal(s_dvp, DVP_SIG_RESET, false);
        usleep(20 * 1000);
        dvp_set_signal(s_dvp, DVP_SIG_RESET, true);
        usleep(150 * 1000);
        for (int i = 0; i < 12; i++) {
            id = sccb_dev_read_byte(gc, 0xF0);
            if (id == 0x9d || id == 0x9b) break;
            usleep(30 * 1000);
        }
        printf("[cam] attempt %d: id=0x%02x\n", attempt, id);
    }
    if (id != 0x9d && id != 0x9b) {
        snprintf(name_out, len, "no sensor (0x%02x)", id);
        return -1;
    }

    /* Full vendor register init, then QVGA 320x240 windowing. The default
     * table ends with output-enable (0xf1=0x07, 0xf2=0x01); 0xff in the reg
     * column means "delay N ms".
     *
     * KNOWN BLOCKER: with this vendored SDK, device-manager SCCB *reads* work
     * at 8-bit addr 0x42 (id reads 0x9d) but *writes* do not take effect — a
     * write-then-readback returns 0x00/0xff and even corrupts the next read.
     * So this init never reaches the sensor, output stays disabled, the DVP
     * sees no VSYNC and never completes a frame (verified: sts FRAME_FINISH
     * never sets). Fixing the SCCB write path (correct addr/timing, likely a
     * direct-register impl like the standalone dvp_sccb_send_data) is the
     * remaining work to get a live image. Register tables in gc0328_regs.h. */
    for (int i = 0; sensor_default_regs[i][0]; i++) {
        if (sensor_default_regs[i][0] == 0xff) { usleep(sensor_default_regs[i][1] * 1000); continue; }
        sccb_dev_write_byte(gc, sensor_default_regs[i][0], sensor_default_regs[i][1]);
    }
    for (int i = 0; qvga_config[i][0]; i++)
        sccb_dev_write_byte(gc, qvga_config[i][0], qvga_config[i][1]);

    /* DVP capture pipeline → s_fb. */
    /* Output index 1 is the display path (RGB565 → rgb_addr); index 0 is the
     * AI path and only accepts RGB24-planar (asserts otherwise → hang). */
    /* Configure BOTH outputs like the working demo: 0=AI (RGB24 planar),
     * 1=display (RGB565). */
    dvp_set_output_attributes(s_dvp, 0, VIDEO_FMT_RGB24_PLANAR, s_ai);
    dvp_set_output_enable(s_dvp, 0, true);
    dvp_set_output_attributes(s_dvp, 1, VIDEO_FMT_RGB565, s_fb);
    dvp_set_output_enable(s_dvp, 1, true);

    dvp_set_on_frame_event(s_dvp, on_frame, NULL);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_BEGIN, true);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_END, true);
    dvp_enable_frame(s_dvp);                         /* kick off continuous capture */

    snprintf(name_out, len, "GC0328 id=0x%02x", id);
    return id;
}

/* Wait for the next finished frame in s_fb (continuous/auto capture is already
 * running from cam_start). Returns 1 if a frame completed, 0 on timeout. */
static int cam_grab(void)
{
    s_frame_done = 0;
    int t = 0;
    while (!s_frame_done && t++ < 300)
        vTaskDelay(pdMS_TO_TICKS(1));
    return s_frame_done;
}

void cam_preview_forever(void)
{
    /* Confirm the sensor is actually streaming before committing the screen to
     * a preview — with only a minimal GC0328 init the DVP never sees a frame
     * (no VSYNC), so we'd otherwise just show black forever. */
    int streaming = 0;
    for (int i = 0; i < 10 && !streaming; i++)
        streaming = cam_grab();

    if (!streaming) {
        printf("[cam] no frames — GC0328 not streaming (needs full register init)\n");
        return;   /* leave the status screen up instead of a black preview */
    }

    for (;;) {
        cam_grab();
        lcd_draw_picture(0, 0, CAM_W, CAM_H, s_fb);
    }
}
