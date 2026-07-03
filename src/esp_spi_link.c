#include "esp_spi_link.h"
#include "pinout.h"
#include "log.h"
#include "diag_screen.h"
#include "amp.h"
#include "esp_uart_log.h"

#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <fpioa.h>
#include <stdio.h>
#include <string.h>

#ifndef SPI_MODE_0
#define SPI_MODE_0 0
#endif
#ifndef SPI_MODE_1
#define SPI_MODE_1 1
#endif
#ifndef SPI_MODE_2
#define SPI_MODE_2 2
#endif
#ifndef SPI_MODE_3
#define SPI_MODE_3 3
#endif

#define FRAME_MAGIC 0x5053454bu
#define FRAME_BYTES 32
#define SPI_WIRE_BYTES 34
#define DATA_BYTES 20
#define MODE_COUNT 4
#define PROBE_FRAMES_PER_MODE 256u
#define SPI_TEST_HZ 200000.0
#define SPI_ZERO_VERDICT_BAD_MIN 4096u

enum { FT_INFO = 4 };

typedef struct {
    uint32_t magic;
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    uint32_t value;
    uint8_t data[DATA_BYTES];
} __attribute__((packed)) kframe_t;

static handle_t s_devs[MODE_COUNT];
static int s_mode;
static uint32_t s_good[MODE_COUNT];
static uint32_t s_bad[MODE_COUNT];
static uint32_t s_invalid[MODE_COUNT];
static uint32_t s_zero[MODE_COUNT];
static uint32_t s_mode_frames;
static uint32_t s_total_good;
static uint32_t s_total_bad;
static uint32_t s_last_total_good;
static uint32_t s_last_total_bad;
static uint32_t s_first_good_logged;
static uint32_t s_bad_dump_count;
static uint32_t s_result_logged;
static volatile int s_paused;
static TickType_t s_last_tick;

void esp_spi_link_pause(int pause)
{
    int new_state = pause ? 1 : 0;
    if (s_paused == new_state)
        return;
    s_paused = new_state;
    LOG(s_paused ? "[spi-test] paused" : "[spi-test] resumed");
}

static uint32_t rd32le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int all_zero(const uint8_t *rx, int n)
{
    for (int i = 0; i < n; i++) {
        if (rx[i] != 0)
            return 0;
    }
    return 1;
}

static void dump_bad_raw(int mode, const uint8_t *rx, int r)
{
    if (s_bad_dump_count >= 24 && ((s_total_bad & 0x3ffu) != 0))
        return;
    s_bad_dump_count++;
    LOGF("[spi-test] bad raw mode=%d r=%d rx=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x m0=%08lx m1=%08lx m2=%08lx",
         mode, r,
         rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7],
         rx[8], rx[9], rx[10], rx[11],
         (unsigned long)rd32le(rx + 0),
         (unsigned long)rd32le(rx + 1),
         (unsigned long)rd32le(rx + 2));
}

static int find_frame(const uint8_t *rx, kframe_t *fr, int *off_out)
{
    for (int off = 0; off <= 2; off++) {
        if (rd32le(rx + off) == FRAME_MAGIC) {
            memcpy(fr, rx + off, sizeof(*fr));
            *off_out = off;
            return 1;
        }
    }
    return 0;
}

static int validate_pattern(const kframe_t *fr)
{
    if (fr->type != FT_INFO || fr->len != DATA_BYTES)
        return 0;
    if (fr->seq != (uint8_t)(fr->value & 0xffu))
        return 0;
    for (uint8_t i = 0; i < DATA_BYTES; i++) {
        if (fr->data[i] != (uint8_t)((fr->value + i) & 0xffu))
            return 0;
    }
    return 1;
}

static int read_one_frame(kframe_t *fr, int *off_out, uint8_t *raw_out, int *r_out)
{
    uint8_t tx[SPI_WIRE_BYTES] = { 3, 0 };
    uint8_t rx[SPI_WIRE_BYTES] = { 0 };
    int r = spi_dev_transfer_full_duplex(s_devs[s_mode], tx, sizeof(tx), rx, sizeof(rx));
    if (raw_out)
        memcpy(raw_out, rx, sizeof(rx));
    if (r_out)
        *r_out = r;
    if (r < (int)sizeof(rx))
        return 0;
    return find_frame(rx, fr, off_out);
}

static void switch_mode(void)
{
    int old = s_mode;
    s_mode = (s_mode + 1) % MODE_COUNT;
    s_mode_frames = 0;
    LOGF("[spi-test] switch mode %d -> %d", old, s_mode);
    diag_printf(3, "SPI test mode %d", s_mode);
}

static void check_final_verdict(void)
{
    if (s_result_logged)
        return;
    if (s_total_good != 0 || s_total_bad < SPI_ZERO_VERDICT_BAD_MIN)
        return;

    uint32_t zero_sum = s_zero[0] + s_zero[1] + s_zero[2] + s_zero[3];
    uint32_t invalid_sum = s_invalid[0] + s_invalid[1] + s_invalid[2] + s_invalid[3];
    if (zero_sum >= s_total_bad && invalid_sum == 0) {
        s_result_logged = 1;
        LOG("[spi-test] RESULT: SPI_FAIL_ALL_ZERO");
        LOG("[spi-test] K210 clocks SPI but MISO reads only zeros in modes 0..3");
        LOG("[spi-test] Check ESP MISO -> K210 MISO pin, FPIOA mapping, and D0/D1 direction");
        diag_line(4, "SPI FAIL: all zero");
    }
}

static void stats_tick(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t dt = now - s_last_tick;
    if (dt < pdMS_TO_TICKS(1000))
        return;

    uint32_t dg = s_total_good - s_last_total_good;
    uint32_t db = s_total_bad - s_last_total_bad;
    s_last_total_good = s_total_good;
    s_last_total_bad = s_total_bad;
    s_last_tick = now;

    LOGF("[spi-test] stat active_mode=%d +good=%lu +bad=%lu total_good=%lu total_bad=%lu m0=%lu/%lu m1=%lu/%lu m2=%lu/%lu m3=%lu/%lu zero=%lu/%lu/%lu/%lu invalid=%lu/%lu/%lu/%lu",
         s_mode,
         (unsigned long)dg, (unsigned long)db,
         (unsigned long)s_total_good, (unsigned long)s_total_bad,
         (unsigned long)s_good[0], (unsigned long)s_bad[0],
         (unsigned long)s_good[1], (unsigned long)s_bad[1],
         (unsigned long)s_good[2], (unsigned long)s_bad[2],
         (unsigned long)s_good[3], (unsigned long)s_bad[3],
         (unsigned long)s_zero[0], (unsigned long)s_zero[1],
         (unsigned long)s_zero[2], (unsigned long)s_zero[3],
         (unsigned long)s_invalid[0], (unsigned long)s_invalid[1],
         (unsigned long)s_invalid[2], (unsigned long)s_invalid[3]);
    diag_printf(4, "good %lu bad %lu", (unsigned long)s_total_good, (unsigned long)s_total_bad);
    check_final_verdict();
}

static void init_spi(void)
{
    fpioa_set_function(PIN_ESP_SPI_CS, FUNC_SPI0_SS0);
    fpioa_set_function(PIN_ESP_SPI_CLK, FUNC_SPI0_SCLK);
    fpioa_set_function(PIN_ESP_SPI_MOSI, FUNC_SPI0_D0);
    fpioa_set_function(PIN_ESP_SPI_MISO, FUNC_SPI0_D1);

    handle_t spi = io_open("/dev/spi0");
    configASSERT(spi);

    const int modes[MODE_COUNT] = { SPI_MODE_0, SPI_MODE_1, SPI_MODE_2, SPI_MODE_3 };
    for (int i = 0; i < MODE_COUNT; i++) {
        s_devs[i] = spi_get_device(spi, modes[i], SPI_FF_STANDARD, 1u, 8);
        configASSERT(s_devs[i]);
        spi_dev_set_clock_rate(s_devs[i], SPI_TEST_HZ);
    }

    LOG("[spi-test] pure UART/SPI test ready hz=200000 modes=0..3 no WiFi no SD write");
    diag_line(3, "SPI UART test");
}

void esp_spi_link_run_forever(void)
{
    amp_set(false);
    init_spi();
    s_last_tick = xTaskGetTickCount();

    LOG("[spi-test] waiting ESP UART marker: kesp: spi slave ready");
    while (!esp_uart_log_spi_ready()) {
        stats_tick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    LOG("[spi-test] ESP SPI-ready marker detected, start mode sweep");

    for (;;) {
        if (s_result_logged) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_paused) {
            stats_tick();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        kframe_t fr;
        uint8_t raw[SPI_WIRE_BYTES];
        int off = -1;
        int r = 0;
        int got_magic = read_one_frame(&fr, &off, raw, &r);
        s_mode_frames++;

        if (got_magic && validate_pattern(&fr)) {
            s_good[s_mode]++;
            s_total_good++;
            if (!s_first_good_logged || (s_good[s_mode] <= 4)) {
                s_first_good_logged = 1;
                LOGF("[spi-test] GOOD mode=%d off=%d seq=%u value=%lu data=%02x %02x %02x %02x",
                     s_mode, off, fr.seq, (unsigned long)fr.value,
                     fr.data[0], fr.data[1], fr.data[2], fr.data[3]);
            }
            taskYIELD();
        } else {
            s_bad[s_mode]++;
            s_total_bad++;
            if (r >= (int)sizeof(raw) && all_zero(raw, sizeof(raw))) {
                s_zero[s_mode]++;
            } else if (got_magic) {
                s_invalid[s_mode]++;
                LOGF("[spi-test] invalid pattern mode=%d off=%d type=%u seq=%u len=%u value=%lu",
                     s_mode, off, fr.type, fr.seq, fr.len, (unsigned long)fr.value);
            } else {
                dump_bad_raw(s_mode, raw, r);
            }

            if (s_mode_frames >= PROBE_FRAMES_PER_MODE)
                switch_mode();
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        stats_tick();
    }
}
