/*
 * DVP camera - Maix Dock FPC, sensor GC0328 (chip id reg 0xF0 = 0x9d).
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
 *   - Output index 0 is the AI path, index 1 is the RGB565 display path.
 *   - LCD uses SPI0 octal on the same DVP data pads. Before camera capture
 *     switch the internal mux back to DVP; after capture switch it back to LCD.
 *   - Do NOT force DVP_CFG_YUV_FORMAT here. Commit 4ac595a used the SDK RGB
 *     path. Forcing YUV produced a valid-sized but flat 0x0420 frame.
 */
#include "camera.h"
#include "lcd.h"
#include "gc0328_regs.h"
#include <devices.h>
#include <dvp.h>
#include <gpiohs.h>
#include <platform.h>
#include <fpioa.h>
#include <sysctl.h>
#include <FreeRTOS.h>
#include <task.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#define GC0328_ADDR  0x42      /* This FreeRTOS SCCB wrapper reads GC0328 with the 8-bit address form. */
#define CAM_W        320
#define CAM_H        240

#define PROBE_VSYNC_GPIOHS 20
#define PROBE_HREF_GPIOHS  21
#define PROBE_PCLK_GPIOHS  22

static handle_t s_dvp, s_sccb;
static handle_t s_gc;
static uint32_t s_fb[CAM_W * CAM_H / 2] __attribute__((aligned(64)));  /* RGB565 display */
static uint32_t s_snap[CAM_W * CAM_H / 2] __attribute__((aligned(64)));/* stable RGB565 snapshot */
static uint8_t  s_ai[CAM_W * CAM_H * 3] __attribute__((aligned(64)));  /* RGB24 planar (AI) */
static volatile int s_frame_started;
static volatile int s_frame_done;
static int s_started;
static int s_chip_id = -1;
static char s_cam_name[32];
static int s_signal_probe_done;

static void cam_route_dvp_data(int to_camera)
{
    /* 1 routes SPI0 octal to the LCD on DVP data pads; 0 returns the pads to
     * the DVP camera peripheral. The LCD driver enables 1 during lcd_init(). */
    sysctl_set_spi0_dvp_data(to_camera ? 0 : 1);
    usleep(2 * 1000);
}

static void cam_set_cmos_pins(void)
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

static void cam_pins(void)
{
    /* Maix Dock routes the camera/LCD FPC banks at 1.8 V in the vendor demos. */
    sysctl_set_power_mode(SYSCTL_POWER_BANK6, SYSCTL_POWER_V18);
    sysctl_set_power_mode(SYSCTL_POWER_BANK7, SYSCTL_POWER_V18);

    cam_route_dvp_data(1);
    cam_set_cmos_pins();
}

static void on_frame(dvp_frame_event_t event, void *userdata)
{
    (void)userdata;
    if (event == VIDEO_FE_BEGIN)
        s_frame_started = 1;
    if (event == VIDEO_FE_END)
        s_frame_done = 1;
}

static void cam_log_stream_regs(const char *tag)
{
    if (!s_gc)
        return;

    sccb_dev_write_byte(s_gc, 0xfe, 0x00);
    uint8_t id = sccb_dev_read_byte(s_gc, 0xF0);
    uint8_t f1 = sccb_dev_read_byte(s_gc, 0xF1);
    uint8_t f2 = sccb_dev_read_byte(s_gc, 0xF2);
    uint8_t r4f = sccb_dev_read_byte(s_gc, 0x4F);
    uint8_t r50 = sccb_dev_read_byte(s_gc, 0x50);
    printf("[cam] %s regs id=%02x f1=%02x f2=%02x 4f=%02x 50=%02x\n", tag, id, f1, f2, r4f, r50);
}

static void cam_probe_sync_pins(void)
{
    if (s_signal_probe_done)
        return;
    s_signal_probe_done = 1;

    cam_route_dvp_data(1);

    fpioa_set_function(43, FUNC_GPIOHS0 + PROBE_VSYNC_GPIOHS);
    fpioa_set_function(45, FUNC_GPIOHS0 + PROBE_HREF_GPIOHS);
    fpioa_set_function(47, FUNC_GPIOHS0 + PROBE_PCLK_GPIOHS);

    volatile gpiohs_t *gpiohs = (volatile gpiohs_t *)GPIOHS_BASE_ADDR;
    const uint32_t mask = (1u << PROBE_VSYNC_GPIOHS) | (1u << PROBE_HREF_GPIOHS) | (1u << PROBE_PCLK_GPIOHS);
    gpiohs->output_en.u32[0] &= ~mask;
    gpiohs->input_en.u32[0] |= mask;

    uint32_t prev = gpiohs->input_val.u32[0];
    uint32_t v_edges = 0, h_edges = 0, p_edges = 0;
    uint32_t v_hi = 0, h_hi = 0, p_hi = 0;
    const int samples = 200000;
    for (int i = 0; i < samples; i++) {
        uint32_t cur = gpiohs->input_val.u32[0];
        uint32_t diff = cur ^ prev;
        if (diff & (1u << PROBE_VSYNC_GPIOHS)) v_edges++;
        if (diff & (1u << PROBE_HREF_GPIOHS))  h_edges++;
        if (diff & (1u << PROBE_PCLK_GPIOHS))  p_edges++;
        if (cur & (1u << PROBE_VSYNC_GPIOHS)) v_hi++;
        if (cur & (1u << PROBE_HREF_GPIOHS))  h_hi++;
        if (cur & (1u << PROBE_PCLK_GPIOHS))  p_hi++;
        prev = cur;
    }

    printf("[cam-sig] samples=%d vsync_edges=%lu href_edges=%lu pclk_edges=%lu hi=%lu/%lu/%lu last=0x%08lx\n",
           samples,
           (unsigned long)v_edges,
           (unsigned long)h_edges,
           (unsigned long)p_edges,
           (unsigned long)v_hi,
           (unsigned long)h_hi,
           (unsigned long)p_hi,
           (unsigned long)prev);

    cam_set_cmos_pins();
    usleep(2 * 1000);
}

static int cam_frame_has_content(uint16_t *first, uint16_t *last, uint32_t *changes)
{
    const uint16_t *p = (const uint16_t *)s_fb;
    const int n = CAM_W * CAM_H;
    uint16_t a = p[0];
    uint16_t z = p[n - 1];
    uint32_t diff = 0;

    /* Sparse scan is enough for a diagnostic guard. A real black scene still
     * has sensor noise and line variation; a bogus DMA buffer is one repeated
     * word such as 0x0420 over the entire frame. */
    uint16_t prev = p[0];
    for (int i = 0; i < n; i += 97) {
        uint16_t v = p[i];
        if (v != prev)
            diff++;
        prev = v;
    }

    if (first) *first = a;
    if (last) *last = z;
    if (changes) *changes = diff;
    return diff >= 4;
}

int cam_start(char *name_out, int len)
{
    if (s_started) {
        if (name_out && len > 0)
            snprintf(name_out, len, "%s", s_cam_name);
        return s_chip_id;
    }

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
    dvp_config(s_dvp, CAM_W, CAM_H, true);          /* auto_enable: continuous */

    dvp_set_signal(s_dvp, DVP_SIG_POWER_DOWN, false);
    usleep(20 * 1000);

    s_gc = sccb_get_device(s_sccb, GC0328_ADDR, 8);

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
            id = sccb_dev_read_byte(s_gc, 0xF0);
            if (id == 0x9d || id == 0x9b)
                break;
            usleep(30 * 1000);
        }
        printf("[cam] attempt %d: id=0x%02x\n", attempt, id);
    }
    if (id != 0x9d && id != 0x9b) {
        snprintf(s_cam_name, sizeof(s_cam_name), "no sensor (0x%02x)", id);
        if (name_out && len > 0)
            snprintf(name_out, len, "%s", s_cam_name);
        return -1;
    }

    /* Full vendor register init, then QVGA 320x240 windowing. The default
     * table ends with output-enable (0xf1=0x07, 0xf2=0x01); 0xff in the reg
     * column means "delay N ms". */
    sccb_dev_write_byte(s_gc, 0xfe, 0x00);
    printf("[cam] page0 id readback=0x%02x\n", sccb_dev_read_byte(s_gc, 0xF0));
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
    cam_log_stream_regs("after-init");

    /* Configure both outputs like the working demo: 0=AI RGB24 planar,
     * 1=display RGB565. The DMA pipeline may not run with display alone. */
    dvp_set_output_attributes(s_dvp, 0, VIDEO_FMT_RGB24_PLANAR, s_ai);
    dvp_set_output_enable(s_dvp, 0, true);
    dvp_set_output_attributes(s_dvp, 1, VIDEO_FMT_RGB565, s_fb);
    dvp_set_output_enable(s_dvp, 1, true);

    dvp_set_on_frame_event(s_dvp, on_frame, NULL);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_BEGIN, true);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_END, true);
    dvp_enable_frame(s_dvp);                         /* kick off continuous capture */

    s_chip_id = id;
    snprintf(s_cam_name, sizeof(s_cam_name), "GC0328 id=0x%02x", id);
    s_started = 1;
    if (name_out && len > 0)
        snprintf(name_out, len, "%s", s_cam_name);
    return id;
}

static int cam_grab_once(int timeout_ms)
{
    if (!s_started || !s_dvp)
        return 0;

    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    s_frame_started = 0;
    s_frame_done = 0;

    cam_route_dvp_data(1);
    if (!s_signal_probe_done)
        cam_probe_sync_pins();
    cam_route_dvp_data(1);
    printf("[cam] route DVP data pads to CAMERA\n");

    dvp->sts |= DVP_STS_FRAME_START | DVP_STS_FRAME_START_WE |
                DVP_STS_FRAME_FINISH | DVP_STS_FRAME_FINISH_WE;
    dvp_enable_frame(s_dvp);

    for (int t = 0; t < timeout_ms; t++) {
        if (s_frame_started && s_frame_done) {
            uint16_t first, last;
            uint32_t changes;
            if (cam_frame_has_content(&first, &last, &changes)) {
                printf("[cam] frame ok first=%04x last=%04x changes=%lu sts=0x%08lx cfg=0x%08lx\n",
                       first, last, (unsigned long)changes,
                       (unsigned long)dvp->sts, (unsigned long)dvp->dvp_cfg);
                return 1;
            }
            printf("[cam] flat frame rejected first=%04x last=%04x changes=%lu sts=0x%08lx cfg=0x%08lx\n",
                   first, last, (unsigned long)changes,
                   (unsigned long)dvp->sts, (unsigned long)dvp->dvp_cfg);
            s_frame_started = 0;
            s_frame_done = 0;
            dvp_enable_frame(s_dvp);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    printf("[cam] frame timeout sts=0x%08lx cfg=0x%08lx cmos=0x%08lx started=%d done=%d\n",
           (unsigned long)dvp->sts,
           (unsigned long)dvp->dvp_cfg,
           (unsigned long)dvp->cmos_cfg,
           s_frame_started, s_frame_done);
    return 0;
}

/* Wait for the next valid finished frame in s_fb. Returns 1 if a frame completed. */
static int cam_grab(void)
{
    for (int i = 0; i < 5; i++) {
        if (cam_grab_once(350))
            return 1;
        cam_log_stream_regs("grab-timeout");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return 0;
}

int cam_capture_rgb565(const uint16_t **pixels, int *w, int *h)
{
    if (!s_started) {
        char name[32];
        if (cam_start(name, sizeof(name)) < 0)
            return 0;
    }

    int ok = cam_grab();
    if (!ok) {
        cam_route_dvp_data(0);
        printf("[cam] route DVP data pads back to LCD\n");
        printf("[cam] capture failed: no valid DVP frame\n");
        return 0;
    }

    memcpy(s_snap, s_fb, sizeof(s_snap));
    cam_route_dvp_data(0);
    printf("[cam] route DVP data pads back to LCD\n");
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
        printf("[cam] no valid frames - GC0328 not streaming\n");
        return;
    }

    for (;;) {
        cam_grab();
        memcpy(s_snap, s_fb, sizeof(s_snap));
        cam_route_dvp_data(0);
        lcd_draw_picture(0, 0, CAM_W, CAM_H, s_snap);
    }
}
