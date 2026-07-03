/*
 * DVP camera - Maix Dock FPC, sensor GC0328 (chip id reg 0xF0 = 0x9d).
 *
 * Snapshot flow is modeled after MaixPy-v1 sensor.c:
 *   - GC0328 uses DVP YUV input format even when the API asks RGB565.
 *   - DVP auto mode is disabled.
 *   - On FRAME_START we explicitly start one conversion.
 *   - On FRAME_FINISH we publish the frame.
 */
#include "camera.h"
#include "lcd.h"
#include "gc0328_regs.h"
#include <devices.h>
#include <dvp.h>
#include <platform.h>
#include <fpioa.h>
#include <sysctl.h>
#include <FreeRTOS.h>
#include <task.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define GC0328_ADDR  0x42
#define CAM_W        320
#define CAM_H        240

static handle_t s_dvp, s_sccb;
static handle_t s_gc;
static uint32_t s_fb[CAM_W * CAM_H / 2] __attribute__((aligned(64)));   /* display RGB565 path */
static uint32_t s_snap[CAM_W * CAM_H / 2] __attribute__((aligned(64))); /* stable copy for SD/UART */
static uint8_t  s_ai[CAM_W * CAM_H * 3] __attribute__((aligned(64)));   /* AI planar path */
static volatile int s_frame_done;
static volatile int s_frame_started;
static int s_started;
static int s_chip_id = -1;
static char s_cam_name[32];

static void cam_route_dvp_data(int to_camera)
{
    /* sysctl_set_spi0_dvp_data(1) routes the shared SPI0_D0-D7/DVP_D0-D7 pads to DVP data.
     * sysctl_set_spi0_dvp_data(0) gives the pads back to SPI0/LCD. */
    sysctl_set_spi0_dvp_data(to_camera ? 1 : 0);
    usleep(2 * 1000);
}

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

static void dvp_start_convert(void)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    dvp->sts = DVP_STS_DVP_EN | DVP_STS_DVP_EN_WE;
}

static void dvp_clear_start(void)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    dvp->sts = DVP_STS_FRAME_START | DVP_STS_FRAME_START_WE;
}

static void dvp_clear_finish(void)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    dvp->sts = DVP_STS_FRAME_FINISH | DVP_STS_FRAME_FINISH_WE;
}

static void dvp_clear_frame_flags(void)
{
    dvp_clear_start();
    dvp_clear_finish();
}

static void dvp_maixpy_config(void)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;

    /* MaixPy: dvp_set_image_format(DVP_CFG_YUV_FORMAT),
     * dvp_disable_burst(), dvp_disable_auto(). */
    dvp->dvp_cfg &= ~(DVP_CFG_FORMAT_MASK | DVP_CFG_AUTO_ENABLE | DVP_CFG_BURST_SIZE_4BEATS);
    dvp->dvp_cfg |= DVP_CFG_YUV_FORMAT;
}

static void on_frame(dvp_frame_event_t event, void *userdata)
{
    (void)userdata;

    if (event == VIDEO_FE_BEGIN) {
        s_frame_started = 1;
        if (!s_frame_done)
            dvp_start_convert();
    }

    if (event == VIDEO_FE_END)
        s_frame_done = 1;
}

static uint8_t gc_read(uint8_t reg)
{
    if (!s_gc)
        return 0;
    return sccb_dev_read_byte(s_gc, reg);
}

int cam_start(char *name_out, int len)
{
    if (s_started) {
        if (name_out && len > 0)
            snprintf(name_out, len, "%s", s_cam_name);
        return s_chip_id;
    }

    cam_route_dvp_data(1);
    cam_pins();

    s_dvp = io_open("/dev/dvp0");
    s_sccb = io_open("/dev/sccb0");
    if (!s_dvp || !s_sccb) {
        snprintf(s_cam_name, sizeof(s_cam_name), "no DVP/SCCB");
        if (name_out && len > 0)
            snprintf(name_out, len, "%s", s_cam_name);
        return -1;
    }

    dvp_xclk_set_clock_rate(s_dvp, 24000000);
    dvp_config(s_dvp, CAM_W, CAM_H, false); /* MaixPy-style: no auto capture */
    dvp_maixpy_config();

    dvp_set_signal(s_dvp, DVP_SIG_POWER_DOWN, false);
    usleep(10 * 1000);

    s_gc = sccb_get_device(s_sccb, GC0328_ADDR, 8);

    uint8_t id = 0;
    for (int attempt = 0; attempt < 4 && id != 0x9d && id != 0x9b; attempt++) {
        dvp_set_signal(s_dvp, DVP_SIG_RESET, false);
        usleep(10 * 1000);
        dvp_set_signal(s_dvp, DVP_SIG_RESET, true);
        usleep(150 * 1000);
        for (int i = 0; i < 12; i++) {
            id = gc_read(0xF0);
            if (id == 0x9d || id == 0x9b)
                break;
            usleep(30 * 1000);
        }
    }

    if (id != 0x9d && id != 0x9b) {
        snprintf(s_cam_name, sizeof(s_cam_name), "no sensor (0x%02x)", id);
        if (name_out && len > 0)
            snprintf(name_out, len, "%s", s_cam_name);
        return -1;
    }

    for (int i = 0; sensor_default_regs[i][0]; i++) {
        if (sensor_default_regs[i][0] == 0xff) {
            usleep(sensor_default_regs[i][1] * 1000);
            continue;
        }
        sccb_dev_write_byte(s_gc, sensor_default_regs[i][0], sensor_default_regs[i][1]);
        if (sensor_default_regs[i][0] == 0xfe && sensor_default_regs[i][1] == 0x80)
            usleep(50 * 1000);
        else
            usleep(1000);
    }
    for (int i = 0; qvga_config[i][0]; i++) {
        sccb_dev_write_byte(s_gc, qvga_config[i][0], qvga_config[i][1]);
        usleep(1000);
    }

    dvp_maixpy_config();
    dvp_set_output_attributes(s_dvp, 0, VIDEO_FMT_RGB24_PLANAR, s_ai);
    dvp_set_output_enable(s_dvp, 0, true);
    dvp_set_output_attributes(s_dvp, 1, VIDEO_FMT_RGB565, s_fb);
    dvp_set_output_enable(s_dvp, 1, true);

    dvp_set_on_frame_event(s_dvp, on_frame, NULL);
    dvp_clear_frame_flags();
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_BEGIN, true);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_END, true);

    s_chip_id = id;
    snprintf(s_cam_name, sizeof(s_cam_name), "GC0328 id=0x%02x", id);
    s_started = 1;
    if (name_out && len > 0)
        snprintf(name_out, len, "%s", s_cam_name);
    return id;
}

static int cam_grab(void)
{
    if (!s_started || !s_dvp)
        return 0;

    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;

    cam_route_dvp_data(1);
    s_frame_started = 0;
    s_frame_done = 0;
    dvp_clear_frame_flags();

    int t = 0;
    while (!s_frame_done && t++ < 1000) {
        uint32_t sts = dvp->sts;

        if ((sts & DVP_STS_FRAME_START) && !s_frame_started) {
            s_frame_started = 1;
            dvp_start_convert();
            dvp_clear_start();
        }

        if (sts & DVP_STS_FRAME_FINISH) {
            s_frame_done = 1;
            dvp_clear_finish();
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!s_frame_done) {
        printf("[cam] timeout start=%d sts=0x%08lx cfg=0x%08lx\n",
               s_frame_started, (unsigned long)dvp->sts, (unsigned long)dvp->dvp_cfg);
        return 0;
    }
    return 1;
}

int cam_capture_rgb565(const uint16_t **pixels, int *w, int *h)
{
    if (!s_started) {
        char name[32];
        if (cam_start(name, sizeof(name)) < 0)
            return 0;
    }

    if (!cam_grab()) {
        cam_route_dvp_data(0);
        return 0;
    }

    memcpy(s_snap, s_fb, sizeof(s_snap));
    cam_route_dvp_data(0);
    *pixels = (const uint16_t *)s_snap;
    *w = CAM_W;
    *h = CAM_H;
    return 1;
}

void cam_preview_forever(void)
{
    if (cam_start(NULL, 0) < 0)
        return;

    for (;;) {
        if (cam_grab()) {
            cam_route_dvp_data(0);
            lcd_draw_picture(0, 0, CAM_W, CAM_H, s_fb);
        }
    }
}
