/*
 * DVP camera - Maix Dock FPC, sensor GC0328 (chip id reg 0xF0 = 0x9d).
 *
 * Restored from the working camera path in commit 4ac595a and kept minimal:
 * no GPIO signal probe, no forced YUV mode, no flat-frame spam.
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
static uint32_t s_fb[CAM_W * CAM_H / 2] __attribute__((aligned(64)));
static uint32_t s_snap[CAM_W * CAM_H / 2] __attribute__((aligned(64)));
static uint8_t  s_ai[CAM_W * CAM_H * 3] __attribute__((aligned(64)));
static volatile int s_frame_done;
static int s_started;
static int s_chip_id = -1;
static char s_cam_name[32];

static void cam_route_dvp_data(int to_camera)
{
    /* Current LCD driver enables SPI0-on-DVP-data-pads for the LCD bus.
     * Switch the mux back to camera for capture and back to LCD afterwards. */
    sysctl_set_spi0_dvp_data(to_camera ? 0 : 1);
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

static void on_frame(dvp_frame_event_t event, void *userdata)
{
    (void)userdata;
    if (event == VIDEO_FE_END)
        s_frame_done = 1;
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
    dvp_config(s_dvp, CAM_W, CAM_H, true);

    dvp_set_signal(s_dvp, DVP_SIG_POWER_DOWN, false);
    usleep(20 * 1000);

    handle_t gc = sccb_get_device(s_sccb, GC0328_ADDR, 8);

    uint8_t id = 0;
    for (int attempt = 0; attempt < 4 && id != 0x9d && id != 0x9b; attempt++) {
        dvp_set_signal(s_dvp, DVP_SIG_RESET, false);
        usleep(20 * 1000);
        dvp_set_signal(s_dvp, DVP_SIG_RESET, true);
        usleep(150 * 1000);
        for (int i = 0; i < 12; i++) {
            id = sccb_dev_read_byte(gc, 0xF0);
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
        sccb_dev_write_byte(gc, sensor_default_regs[i][0], sensor_default_regs[i][1]);
    }
    for (int i = 0; qvga_config[i][0]; i++)
        sccb_dev_write_byte(gc, qvga_config[i][0], qvga_config[i][1]);

    dvp_set_output_attributes(s_dvp, 0, VIDEO_FMT_RGB24_PLANAR, s_ai);
    dvp_set_output_enable(s_dvp, 0, true);
    dvp_set_output_attributes(s_dvp, 1, VIDEO_FMT_RGB565, s_fb);
    dvp_set_output_enable(s_dvp, 1, true);

    dvp_set_on_frame_event(s_dvp, on_frame, NULL);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_BEGIN, true);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_END, true);
    dvp_enable_frame(s_dvp);

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
    s_frame_done = 0;

    /* The old preview path used the IRQ callback. For command-driven capture,
     * also poll the hardware finish bit so a late/missed PLIC callback does not
     * turn a real frame into a timeout. */
    dvp->sts = DVP_STS_FRAME_START | DVP_STS_FRAME_START_WE |
               DVP_STS_FRAME_FINISH | DVP_STS_FRAME_FINISH_WE;
    dvp_enable_frame(s_dvp);

    int t = 0;
    while (t++ < 500) {
        if (s_frame_done)
            return 1;
        if (dvp->sts & DVP_STS_FRAME_FINISH) {
            dvp->sts = DVP_STS_FRAME_FINISH | DVP_STS_FRAME_FINISH_WE;
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    printf("[cam] capture timeout sts=0x%08lx cfg=0x%08lx\n",
           (unsigned long)dvp->sts, (unsigned long)dvp->dvp_cfg);
    return 0;
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
    int streaming = 0;
    for (int i = 0; i < 10 && !streaming; i++)
        streaming = cam_grab();

    if (!streaming) {
        cam_route_dvp_data(0);
        printf("[cam] no frames\n");
        return;
    }

    for (;;) {
        cam_grab();
        cam_route_dvp_data(0);
        lcd_draw_picture(0, 0, CAM_W, CAM_H, s_fb);
    }
}
