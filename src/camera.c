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
#include <stdint.h>

#define GC0328_ADDR  0x42
#define CAM_W        320
#define CAM_H        240
#define CAM_PIXELS   (CAM_W * CAM_H)
#define CAM_FB_WORDS (CAM_PIXELS / 2)

static handle_t s_dvp, s_sccb, s_gc;
static uint32_t s_fb[CAM_FB_WORDS] __attribute__((aligned(128)));
static uint32_t s_snap[CAM_FB_WORDS] __attribute__((aligned(128)));
static uint8_t  s_ai[CAM_PIXELS * 3] __attribute__((aligned(64)));
static volatile uint32_t s_begin_irq;
static volatile uint32_t s_end_irq;
static volatile uint32_t s_dvp_finish_flag;
static int s_started;
static int s_chip_id = -1;
static char s_cam_name[32];

typedef struct {
    uint32_t changes;
    uint32_t nonzero;
    uint32_t rows_nonzero;
    int first_nonzero_row;
    int last_nonzero_row;
    uint16_t first;
    uint16_t last;
} cam_rgb565_stats_t;

typedef struct { uint32_t changes, nonzero; uint8_t first, last; } cam_u8_stats_t;

static void cam_route_dvp_data(int to_camera)
{
    sysctl_set_spi0_dvp_data(to_camera ? 1 : 0);
    usleep(2000);
}

static void cam_set_cmos_pins(void)
{
    fpioa_io_config_t cfg;
    fpioa_set_function(40, FUNC_SCCB_SDA);
    fpioa_set_function(41, FUNC_SCCB_SCLK);
    fpioa_set_function(42, FUNC_CMOS_RST);
    fpioa_set_function(43, FUNC_CMOS_VSYNC);
    fpioa_set_function(44, FUNC_CMOS_PWDN);
    fpioa_set_function(45, FUNC_CMOS_HREF);
    fpioa_set_function(46, FUNC_CMOS_XCLK);
    fpioa_set_function(47, FUNC_CMOS_PCLK);
    if (fpioa_get_io(43, &cfg) == 0) {
        cfg.di_inv = 1;
        fpioa_set_io(43, &cfg);
    }
}

static void cam_pins(void)
{
    sysctl_set_power_mode(SYSCTL_POWER_BANK6, SYSCTL_POWER_V18);
    sysctl_set_power_mode(SYSCTL_POWER_BANK7, SYSCTL_POWER_V18);
    cam_route_dvp_data(1);
    cam_set_cmos_pins();
}

static void dvp_start_convert_isr(void)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    dvp->sts = DVP_STS_DVP_EN | DVP_STS_DVP_EN_WE;
}

static void cam_clear_dvp_frame_flags(void)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    dvp->sts = DVP_STS_FRAME_START | DVP_STS_FRAME_START_WE |
               DVP_STS_FRAME_FINISH | DVP_STS_FRAME_FINISH_WE;
}

static void cam_poll_maixpy_irq(void)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    uint32_t sts = dvp->sts;

    /* Same rule as MaixPy sensor_irq(), but polled as a safety net because this
     * BSP sometimes leaves FRAME_START/FRAME_FINISH pending while callback
     * counters do not advance in the current wait window.
     */
    if (sts & DVP_STS_FRAME_FINISH) {
        cam_clear_dvp_frame_flags();
        s_end_irq++;
        s_dvp_finish_flag = 1;
    } else if (sts & DVP_STS_FRAME_START) {
        if (s_dvp_finish_flag == 0)
            dvp_start_convert_isr();
        dvp->sts = DVP_STS_FRAME_START | DVP_STS_FRAME_START_WE;
        s_begin_irq++;
    }
}

static void on_frame(dvp_frame_event_t event, void *userdata)
{
    (void)userdata;

    if (event == VIDEO_FE_BEGIN) {
        s_begin_irq++;
        if (s_dvp_finish_flag == 0)
            dvp_start_convert_isr();
    } else if (event == VIDEO_FE_END) {
        s_end_irq++;
        s_dvp_finish_flag = 1;
    }
}

static void cam_log_stream_regs(const char *tag)
{
    if (!s_gc) return;
    sccb_dev_write_byte(s_gc, 0xfe, 0x00);
    uint8_t id = sccb_dev_read_byte(s_gc, 0xF0);
    uint8_t f1 = sccb_dev_read_byte(s_gc, 0xF1);
    uint8_t f2 = sccb_dev_read_byte(s_gc, 0xF2);
    uint8_t r17 = sccb_dev_read_byte(s_gc, 0x17);
    uint8_t r50 = sccb_dev_read_byte(s_gc, 0x50);
    uint8_t r59 = sccb_dev_read_byte(s_gc, 0x59);
    uint8_t r5a = sccb_dev_read_byte(s_gc, 0x5A);
    printf("[cam] %s regs id=%02x f1=%02x f2=%02x 17=%02x 50=%02x 59=%02x 5a=%02x\n", tag, id, f1, f2, r17, r50, r59, r5a);
}

static cam_rgb565_stats_t cam_stats_rgb565(const uint16_t *p)
{
    cam_rgb565_stats_t st;
    memset(&st, 0, sizeof(st));
    st.first = p[0];
    st.last = p[CAM_PIXELS - 1];
    st.first_nonzero_row = -1;
    st.last_nonzero_row = -1;
    uint16_t prev = p[0];
    for (int y = 0; y < CAM_H; y++) {
        int row_nz = 0;
        for (int x = 0; x < CAM_W; x++) {
            int i = y * CAM_W + x;
            uint16_t v = p[i];
            if (v) { st.nonzero++; row_nz = 1; }
            if (i && v != prev) st.changes++;
            prev = v;
        }
        if (row_nz) {
            st.rows_nonzero++;
            if (st.first_nonzero_row < 0) st.first_nonzero_row = y;
            st.last_nonzero_row = y;
        }
    }
    return st;
}

static cam_u8_stats_t cam_stats_u8(const uint8_t *p, int len)
{
    cam_u8_stats_t st; memset(&st, 0, sizeof(st));
    st.first = p[0]; st.last = p[len - 1];
    uint8_t prev = p[0];
    for (int i = 0; i < len; i++) {
        uint8_t v = p[i];
        if (v) st.nonzero++;
        if (i && v != prev) st.changes++;
        prev = v;
    }
    return st;
}

static uint32_t cam_frame_score(const cam_rgb565_stats_t *st)
{
    int last = st->last_nonzero_row;
    if (last < 0) last = 0;
    return (uint32_t)(last * 100000u + st->rows_nonzero * 1000u + st->changes + st->nonzero / 8u);
}

static void cam_reverse_u32pixel(uint32_t *addr, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++) {
        uint32_t data = addr[i];
        addr[i] = ((data & 0x000000FFu) << 24) |
                  ((data & 0x0000FF00u) << 8)  |
                  ((data & 0x00FF0000u) >> 8)  |
                  ((data & 0xFF000000u) >> 24);
    }
}

static void cam_ai_to_rgb565(uint16_t *dst)
{
    const uint8_t *r = s_ai;
    const uint8_t *g = s_ai + CAM_PIXELS;
    const uint8_t *b = s_ai + CAM_PIXELS * 2;
    for (int i = 0; i < CAM_PIXELS; i++)
        dst[i] = (uint16_t)(((uint16_t)(r[i] >> 3) << 11) | ((uint16_t)(g[i] >> 2) << 5) | ((uint16_t)(b[i] >> 3)));
}

static void cam_config_dvp(void)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    cam_route_dvp_data(1);
    cam_set_cmos_pins();

    dvp_config(s_dvp, CAM_W, CAM_H, false);
    dvp->dvp_cfg = (dvp->dvp_cfg & ~(DVP_CFG_FORMAT_MASK | DVP_CFG_AUTO_ENABLE)) | DVP_CFG_YUV_FORMAT;
    dvp_set_output_attributes(s_dvp, 0, VIDEO_FMT_RGB24_PLANAR, s_ai);
    dvp_set_output_enable(s_dvp, 0, true);
    dvp_set_output_attributes(s_dvp, 1, VIDEO_FMT_RGB565, s_fb);
    dvp_set_output_enable(s_dvp, 1, true);
    dvp_set_on_frame_event(s_dvp, on_frame, NULL);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_BEGIN, true);
    dvp_set_frame_event_enable(s_dvp, VIDEO_FE_END, true);
    cam_clear_dvp_frame_flags();
    s_begin_irq = 0;
    s_end_irq = 0;
    s_dvp_finish_flag = 0;
    printf("[cam] DVP cfg maixpy-poll DATA_MUX1 GC0328_QVGA_FIX sts=0x%08lx cfg=0x%08lx cmos=0x%08lx fb=%p snap=%p ai=%p fb_bytes=%u ai_bytes=%u\n",
           (unsigned long)dvp->sts, (unsigned long)dvp->dvp_cfg, (unsigned long)dvp->cmos_cfg,
           (void *)s_fb, (void *)s_snap, (void *)s_ai,
           (unsigned)(CAM_W * CAM_H * 2), (unsigned)(CAM_W * CAM_H * 3));
}

static int cam_wait_finish_flag(int timeout_ms, const char *tag, int reverse)
{
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    uint32_t begin0 = s_begin_irq;
    uint32_t end0 = s_end_irq;
    s_dvp_finish_flag = 0;
    for (int t = 0; t < timeout_ms * 20; t++) {
        cam_poll_maixpy_irq();
        if (s_dvp_finish_flag) {
            if (reverse)
                cam_reverse_u32pixel(s_fb, CAM_FB_WORDS);
            printf("[cam] %s ok begin_irq=%lu end_irq=%lu sts=0x%08lx cfg=0x%08lx\n",
                   tag,
                   (unsigned long)(s_begin_irq - begin0),
                   (unsigned long)(s_end_irq - end0),
                   (unsigned long)dvp->sts, (unsigned long)dvp->dvp_cfg);
            return 1;
        }
        usleep(50);
        if ((t % 20) == 19)
            taskYIELD();
    }
    printf("[cam] %s timeout begin_irq=%lu end_irq=%lu finish_flag=%lu sts=0x%08lx cfg=0x%08lx cmos=0x%08lx\n",
           tag,
           (unsigned long)(s_begin_irq - begin0), (unsigned long)(s_end_irq - end0),
           (unsigned long)s_dvp_finish_flag,
           (unsigned long)dvp->sts, (unsigned long)dvp->dvp_cfg, (unsigned long)dvp->cmos_cfg);
    return 0;
}

static void cam_skip_frames_maixpy(void)
{
    int ok = 0;
    printf("[cam] maixpy skip_frames poll start time=2000ms\n");
    for (int i = 0; i < 8; i++) {
        if (cam_wait_finish_flag(500, "skip", 0))
            ok++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("[cam] maixpy skip_frames poll done ok=%d begin=%lu end=%lu\n",
           ok, (unsigned long)s_begin_irq, (unsigned long)s_end_irq);
}

int cam_start(char *name_out, int len)
{
    if (s_started) {
        if (name_out && len > 0) snprintf(name_out, len, "%s", s_cam_name);
        return s_chip_id;
    }
    cam_pins();
    s_dvp = io_open("/dev/dvp0");
    s_sccb = io_open("/dev/sccb0");
    if (!s_dvp || !s_sccb) {
        snprintf(s_cam_name, sizeof(s_cam_name), "no DVP/SCCB");
        if (name_out && len > 0) snprintf(name_out, len, "%s", s_cam_name);
        return -1;
    }

    dvp_xclk_set_clock_rate(s_dvp, 24000000);
    dvp_config(s_dvp, CAM_W, CAM_H, false);
    volatile dvp_t *dvp = (volatile dvp_t *)DVP_BASE_ADDR;
    dvp->dvp_cfg = (dvp->dvp_cfg & ~(DVP_CFG_FORMAT_MASK | DVP_CFG_AUTO_ENABLE)) | DVP_CFG_YUV_FORMAT;

    dvp_set_signal(s_dvp, DVP_SIG_POWER_DOWN, false);
    usleep(20000);
    s_gc = sccb_get_device(s_sccb, GC0328_ADDR, 8);
    uint8_t id = 0;
    for (int attempt = 0; attempt < 4 && id != 0x9d && id != 0x9b; attempt++) {
        dvp_set_signal(s_dvp, DVP_SIG_RESET, false);
        usleep(20000);
        dvp_set_signal(s_dvp, DVP_SIG_RESET, true);
        usleep(150000);
        for (int i = 0; i < 12; i++) {
            id = sccb_dev_read_byte(s_gc, 0xF0);
            if (id == 0x9d || id == 0x9b) break;
            usleep(30000);
        }
        printf("[cam] attempt %d: id=0x%02x\n", attempt, id);
    }
    if (id != 0x9d && id != 0x9b) {
        snprintf(s_cam_name, sizeof(s_cam_name), "no sensor (0x%02x)", id);
        if (name_out && len > 0) snprintf(name_out, len, "%s", s_cam_name);
        return -1;
    }

    sccb_dev_write_byte(s_gc, 0xfe, 0x00);
    printf("[cam] page0 id readback=0x%02x\n", sccb_dev_read_byte(s_gc, 0xF0));
    for (int i = 0; sensor_default_regs[i][0]; i++) {
        if (sensor_default_regs[i][0] == 0xff) { usleep(sensor_default_regs[i][1] * 1000); continue; }
        sccb_dev_write_byte(s_gc, sensor_default_regs[i][0], sensor_default_regs[i][1]);
        if (sensor_default_regs[i][0] == 0xfe && sensor_default_regs[i][1] == 0x80) usleep(50000);
        else usleep(1000);
    }
    for (int i = 0; qvga_config[i][0]; i++) {
        sccb_dev_write_byte(s_gc, qvga_config[i][0], qvga_config[i][1]);
        usleep(1000);
    }

    sccb_dev_write_byte(s_gc, 0xfe, 0x00);
    sccb_dev_write_byte(s_gc, 0x17, 0x14);
    sccb_dev_write_byte(s_gc, 0x50, 0x01);
    sccb_dev_write_byte(s_gc, 0x59, 0x22);
    sccb_dev_write_byte(s_gc, 0x5a, 0x03);
    usleep(30000);

    cam_log_stream_regs("after-qvga-fix");
    cam_config_dvp();
    cam_skip_frames_maixpy();
    s_chip_id = id;
    snprintf(s_cam_name, sizeof(s_cam_name), "GC0328 id=0x%02x", id);
    s_started = 1;
    if (name_out && len > 0) snprintf(name_out, len, "%s", s_cam_name);
    return id;
}

static int cam_grab_once(int timeout_ms)
{
    if (!s_started || !s_dvp) return 0;
    cam_route_dvp_data(1);
    cam_set_cmos_pins();
    return cam_wait_finish_flag(timeout_ms, "snapshot", 1);
}

static int cam_capture_best_from_burst(void)
{
    uint32_t best_score = 0;
    int best_frame = -1;

    memset(s_snap, 0, sizeof(s_snap));
    printf("[cam] maixpy-poll capture start frames=12\n");

    for (int frame = 0; frame < 12; frame++) {
        memset(s_fb, 0, sizeof(s_fb));
        memset(s_ai, 0, sizeof(s_ai));

        int ok = cam_grab_once(500);
        if (!ok) {
            cam_log_stream_regs("grab-timeout");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        cam_rgb565_stats_t fb = cam_stats_rgb565((const uint16_t *)s_fb);
        cam_u8_stats_t ai = cam_stats_u8(s_ai, CAM_PIXELS * 3);
        uint32_t score = cam_frame_score(&fb);

        printf("[cam] maixpy-poll frame %d score=%lu fb: rows=%lu firstrow=%d lastrow=%d changes=%lu nonzero=%lu first=%04x last=%04x ai: changes=%lu nonzero=%lu\n",
               frame, (unsigned long)score,
               (unsigned long)fb.rows_nonzero, fb.first_nonzero_row, fb.last_nonzero_row,
               (unsigned long)fb.changes, (unsigned long)fb.nonzero, fb.first, fb.last,
               (unsigned long)ai.changes, (unsigned long)ai.nonzero);

        if (score > best_score) {
            best_score = score;
            best_frame = frame;
            if (fb.changes < 16 && ai.changes >= 16)
                cam_ai_to_rgb565((uint16_t *)s_snap);
            else
                memcpy(s_snap, s_fb, sizeof(s_snap));
        }

        if (fb.last_nonzero_row >= 220 && fb.rows_nonzero >= 200) {
            printf("[cam] good enough full-ish frame at maixpy-poll frame %d\n", frame);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    printf("[cam] maixpy-poll capture stop best_frame=%d best_score=%lu begin=%lu end=%lu\n",
           best_frame, (unsigned long)best_score,
           (unsigned long)s_begin_irq, (unsigned long)s_end_irq);
    return best_frame >= 0;
}

int cam_capture_rgb565(const uint16_t **pixels, int *w, int *h)
{
    if (!s_started) {
        char name[32];
        if (cam_start(name, sizeof(name)) < 0) return 0;
    }

    int ok = cam_capture_best_from_burst();
    cam_route_dvp_data(0);
    printf("[cam] route DVP data pads back to LCD\n");
    if (!ok) {
        printf("[cam] capture failed: no DVP frame\n");
        return 0;
    }

    cam_rgb565_stats_t st = cam_stats_rgb565((const uint16_t *)s_snap);
    printf("[cam] selected snapshot rows=%lu firstrow=%d lastrow=%d changes=%lu nonzero=%lu first=%04x last=%04x\n",
           (unsigned long)st.rows_nonzero, st.first_nonzero_row, st.last_nonzero_row,
           (unsigned long)st.changes, (unsigned long)st.nonzero, st.first, st.last);

    *pixels = (const uint16_t *)s_snap;
    *w = CAM_W;
    *h = CAM_H;
    return 1;
}

void cam_preview_forever(void)
{
    if (cam_start(NULL, 0) < 0) return;
    for (;;) {
        if (cam_capture_best_from_burst()) {
            cam_route_dvp_data(0);
            lcd_draw_picture(0, 0, CAM_W, CAM_H, s_snap);
        }
    }
}
