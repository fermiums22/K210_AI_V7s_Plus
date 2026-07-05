#include "esp_spi_link.h"
#include "pinout.h"
#include "log.h"
#include "amp.h"
#include "esp_uart_log.h"
#include "sd.h"

#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <fpioa.h>
#include <platform.h>
#include <filesystem.h>
#include <ff.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FRAME_MAGIC          0x5053454bu /* bytes on wire: 4b 45 53 50 == "KESP" */
#define RX_MAX               96
#define KESP_FRAME_BYTES     64
#define KESP_PAYLOAD_BYTES   40
#define TEST_WARMUP          2u
#define TEST_FRAMES          24u
#define GPIOHS_CS            0
#define GPIOHS_BIT(n)        (1u << (n))
#define FUNC_GPIOHS_NUM(n)   ((fpioa_function_t)(24 + (n)))

enum {
    KESP_F_IDLE  = 1,
    KESP_F_BEGIN = 2,
    KESP_F_DATA  = 3,
    KESP_F_END   = 4,
    KESP_F_ERROR = 5,
};

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t len;
    uint32_t seq;
    uint32_t total;
    uint32_t offset;
    uint32_t reserved;
    uint8_t payload[KESP_PAYLOAD_BYTES];
} kesp_frame_t;

typedef struct {
    volatile uint32_t input_val;
    volatile uint32_t input_en;
    volatile uint32_t output_en;
    volatile uint32_t output_val;
    volatile uint32_t pue;
    volatile uint32_t ds;
    volatile uint32_t rise_ie;
    volatile uint32_t rise_ip;
    volatile uint32_t fall_ie;
    volatile uint32_t fall_ip;
    volatile uint32_t high_ie;
    volatile uint32_t high_ip;
    volatile uint32_t low_ie;
    volatile uint32_t low_ip;
    volatile uint32_t iof_en;
    volatile uint32_t iof_sel;
    volatile uint32_t out_xor;
} gpiohs_regs_t;

typedef struct {
    int mode;
    uint32_t hz;
    int swap_d0_d1;
    int manual_cs;
    int wire_len;
    uint32_t good;
    uint32_t bad;
    int last_magic_off;
} spi_case_t;

typedef struct {
    handle_t f;
    char name[48];
    char rel_path[80];
    uint32_t expected;
    uint32_t written;
    uint32_t frames;
} file_rx_t;

static gpiohs_regs_t *const GPIOHS = (gpiohs_regs_t *)GPIOHS_BASE_ADDR;
static handle_t s_spi;
static handle_t s_dev;
static uint8_t s_tx[RX_MAX];
static uint8_t s_rx[RX_MAX];
static spi_case_t s_best;
static int s_have_best;
static volatile int s_pause;
static volatile int s_restart_after_pause;
static volatile int s_started;

void esp_spi_link_pause(int pause)
{
    s_pause = pause ? 1 : 0;
    if (s_pause)
        s_restart_after_pause = 1;
    LOGF("[wifi-sd] pause=%d", s_pause);
}

static int wait_if_paused(const char *where)
{
    if (!s_pause)
        return 0;

    LOGF("[wifi-sd] paused at %s", where);
    s_restart_after_pause = 1;
    while (s_pause) {
        amp_set(false);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    LOG("[wifi-sd] resumed; restart from ESP-ready wait");
    return 1;
}

static uint32_t rd32le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int find_magic(const uint8_t *rx, int len)
{
    if (len < 4)
        return -1;
    for (int off = 0; off <= len - 4; off++) {
        if (rd32le(rx + off) == FRAME_MAGIC)
            return off;
    }
    return -1;
}

static int decode_frame(const uint8_t *rx, int len, kesp_frame_t *out, int *magic_off)
{
    int off = find_magic(rx, len);
    if (magic_off)
        *magic_off = off;
    if (off < 0 || off + (int)sizeof(kesp_frame_t) > len)
        return 0;
    memcpy(out, rx + off, sizeof(*out));
    if (out->magic != FRAME_MAGIC || out->version != 1 || out->len > KESP_PAYLOAD_BYTES)
        return 0;
    return 1;
}

static const char *cs_name(int manual_cs)
{
    return manual_cs ? "gpiohs" : "hwss0";
}

static const char *map_name(int swap_d0_d1)
{
    return swap_d0_d1 ? "swap_d0_d1" : "normal_d0_mosi_d1_miso";
}

static void gpiohs_cs_write(int val)
{
    uint32_t bit = GPIOHS_BIT(GPIOHS_CS);
    if (val)
        GPIOHS->output_val |= bit;
    else
        GPIOHS->output_val &= ~bit;
}

static void gpiohs_cs_init(void)
{
    uint32_t bit = GPIOHS_BIT(GPIOHS_CS);
    fpioa_set_function(PIN_ESP_SPI_CS, FUNC_GPIOHS_NUM(GPIOHS_CS));
    GPIOHS->iof_en &= ~bit;
    GPIOHS->out_xor &= ~bit;
    GPIOHS->pue &= ~bit;
    GPIOHS->input_en &= ~bit;
    GPIOHS->output_en |= bit;
    gpiohs_cs_write(1); /* ESP8266 HSPI slave CS is active-low. */
}

static void map_spi_pins(int swap_d0_d1, int manual_cs)
{
    if (manual_cs)
        gpiohs_cs_init();
    else
        fpioa_set_function(PIN_ESP_SPI_CS, FUNC_SPI0_SS0);

    fpioa_set_function(PIN_ESP_SPI_CLK, FUNC_SPI0_SCLK);

    if (!swap_d0_d1) {
        fpioa_set_function(PIN_ESP_SPI_MOSI, FUNC_SPI0_D0);
        fpioa_set_function(PIN_ESP_SPI_MISO, FUNC_SPI0_D1);
    } else {
        fpioa_set_function(PIN_ESP_SPI_MOSI, FUNC_SPI0_D1);
        fpioa_set_function(PIN_ESP_SPI_MISO, FUNC_SPI0_D0);
    }
}

static int spi_mode_value(int mode_no)
{
    switch (mode_no) {
    case 0: return SPI_MODE_0;
    case 1: return SPI_MODE_1;
    case 2: return SPI_MODE_2;
    default: return SPI_MODE_3;
    }
}

static void open_spi_case(const spi_case_t *c)
{
    if (!s_spi) {
        s_spi = io_open("/dev/spi0");
        configASSERT(s_spi);
    }

    map_spi_pins(c->swap_d0_d1, c->manual_cs);
    s_dev = spi_get_device(s_spi, spi_mode_value(c->mode), SPI_FF_STANDARD, 1u, 8);
    configASSERT(s_dev);
    spi_dev_set_clock_rate(s_dev, c->hz);
}

static void fill_tx(uint32_t seq, int len)
{
    memset(s_tx, 0, sizeof(s_tx));
    for (int i = 0; i < len && i < RX_MAX; i++)
        s_tx[i] = (uint8_t)(0x80u + ((seq + (uint32_t)i) & 0x3fu));
    if (len >= 4) {
        s_tx[0] = 0x11;
        s_tx[1] = 0x22;
        s_tx[2] = 0x33;
        s_tx[3] = 0x44;
    }
}

static int transfer_once(const spi_case_t *c, uint32_t seq)
{
    memset(s_rx, 0, sizeof(s_rx));
    fill_tx(seq, c->wire_len);

    if (c->manual_cs)
        gpiohs_cs_write(0);

    int r = spi_dev_transfer_full_duplex(s_dev, s_tx, c->wire_len, s_rx, c->wire_len);

    if (c->manual_cs)
        gpiohs_cs_write(1);

    return r;
}

static void remember_best(const spi_case_t *c)
{
    if (!s_have_best || c->good > s_best.good ||
        (c->good == s_best.good && c->bad < s_best.bad)) {
        s_best = *c;
        s_have_best = 1;
    }
}

static spi_case_t run_case(spi_case_t c)
{
    kesp_frame_t f;
    open_spi_case(&c);
    c.good = 0;
    c.bad = 0;
    c.last_magic_off = -1;

    LOGF("[wifi-sd] scan case mode=%d hz=%lu cs=%s map=%s len=%d",
         c.mode, (unsigned long)c.hz, cs_name(c.manual_cs), map_name(c.swap_d0_d1), c.wire_len);

    for (uint32_t i = 0; i < TEST_WARMUP + TEST_FRAMES; i++) {
        if (wait_if_paused("scan"))
            return c;

        amp_set(false);
        int r = transfer_once(&c, i);
        int off = -1;
        int ok = decode_frame(s_rx, r > c.wire_len ? c.wire_len : r, &f, &off);

        if (i == TEST_WARMUP) {
            LOGF("[wifi-sd] sample mode=%d hz=%lu cs=%s map=%s len=%d r=%d off=%d rx=%02x %02x %02x %02x type=%u seq=%lu",
                 c.mode, (unsigned long)c.hz, cs_name(c.manual_cs), map_name(c.swap_d0_d1),
                 c.wire_len, r, off, s_rx[0], s_rx[1], s_rx[2], s_rx[3], ok ? f.type : 0,
                 ok ? (unsigned long)f.seq : 0u);
        }

        if (i >= TEST_WARMUP) {
            if (r >= c.wire_len && ok) {
                c.good++;
                c.last_magic_off = off;
            } else {
                c.bad++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    LOGF("[wifi-sd] scan done mode=%d hz=%lu cs=%s map=%s len=%d good=%lu bad=%lu off=%d",
         c.mode, (unsigned long)c.hz, cs_name(c.manual_cs), map_name(c.swap_d0_d1), c.wire_len,
         (unsigned long)c.good, (unsigned long)c.bad, c.last_magic_off);

    if (c.good)
        remember_best(&c);

    vTaskDelay(pdMS_TO_TICKS(20));
    return c;
}

static void run_scan(void)
{
    static const uint32_t hz_list[] = { 500000u, 1000000u, 2000000u, 4000000u };
    static const int len_list[] = { 64, 66, 68, 72 };

    s_have_best = 0;
    memset(&s_best, 0, sizeof(s_best));
    LOG("[wifi-sd] scan start: ESP HSPI KESP frame, find mode/map/cs/length");

    for (int manual_cs = 0; manual_cs <= 1; manual_cs++) {
        for (int swap = 0; swap <= 1; swap++) {
            for (unsigned hi = 0; hi < sizeof(hz_list) / sizeof(hz_list[0]); hi++) {
                for (int mode = 0; mode < 4; mode++) {
                    for (unsigned li = 0; li < sizeof(len_list) / sizeof(len_list[0]); li++) {
                        if (s_restart_after_pause)
                            return;
                        spi_case_t c;
                        memset(&c, 0, sizeof(c));
                        c.mode = mode;
                        c.hz = hz_list[hi];
                        c.swap_d0_d1 = swap;
                        c.manual_cs = manual_cs;
                        c.wire_len = len_list[li];
                        (void)run_case(c);
                    }
                }
            }
        }
    }

    if (s_restart_after_pause)
        return;

    if (s_have_best) {
        LOGF("[wifi-sd] VERDICT SPI_OK best mode=%d hz=%lu cs=%s map=%s len=%d good=%lu bad=%lu off=%d",
             s_best.mode, (unsigned long)s_best.hz, cs_name(s_best.manual_cs), map_name(s_best.swap_d0_d1),
             s_best.wire_len, (unsigned long)s_best.good, (unsigned long)s_best.bad, s_best.last_magic_off);
    } else {
        LOG("[wifi-sd] VERDICT SPI_FAIL no full KESP frame in any tested mode/map/cs/length");
    }
}

static int safe_file_name(const char *s)
{
    if (!s[0])
        return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static void rx_close(file_rx_t *rx)
{
    if (rx->f) {
        filesystem_file_close(rx->f);
        rx->f = 0;
    }
}

static int rx_begin(file_rx_t *rx, const kesp_frame_t *f)
{
    char fs_path[128];
    char fat_path[128];
    char name[sizeof(rx->name)];
    uint32_t n = f->len;
    if (n >= sizeof(name))
        n = sizeof(name) - 1;
    memcpy(name, f->payload, n);
    name[n] = 0;

    rx_close(rx);
    memset(rx, 0, sizeof(*rx));

    if (!safe_file_name(name)) {
        LOGF("[wifi-sd] BEGIN bad filename '%s'", name);
        return 0;
    }
    if (!sd_mount()) {
        LOG("[wifi-sd] BEGIN sd_mount failed");
        return 0;
    }

    snprintf(rx->name, sizeof(rx->name), "%s", name);
    snprintf(rx->rel_path, sizeof(rx->rel_path), "wifi/%s", name);
    snprintf(fs_path, sizeof(fs_path), "/fs/0/%s", rx->rel_path);
    snprintf(fat_path, sizeof(fat_path), "0:/%s", rx->rel_path);
    f_mkdir("0:/wifi");
    f_unlink(fat_path);

    rx->f = filesystem_file_open(fs_path, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
    if (!rx->f) {
        LOGF("[wifi-sd] BEGIN open failed %s", fs_path);
        return 0;
    }
    rx->expected = f->total;
    rx->written = 0;
    rx->frames = 0;
    LOGF("[wifi-sd] BEGIN file=%s size=%lu seq=%lu", rx->rel_path,
         (unsigned long)rx->expected, (unsigned long)f->seq);
    return 1;
}

static int rx_data(file_rx_t *rx, const kesp_frame_t *f)
{
    if (!rx->f) {
        LOGF("[wifi-sd] DATA without open seq=%lu", (unsigned long)f->seq);
        return 0;
    }
    if (f->offset != rx->written) {
        LOGF("[wifi-sd] DATA offset mismatch got=%lu expected=%lu seq=%lu",
             (unsigned long)f->offset, (unsigned long)rx->written, (unsigned long)f->seq);
        rx_close(rx);
        return 0;
    }
    if (f->len == 0 || f->len > KESP_PAYLOAD_BYTES || rx->written + f->len > rx->expected) {
        LOGF("[wifi-sd] DATA bad len=%u written=%lu total=%lu seq=%lu", f->len,
             (unsigned long)rx->written, (unsigned long)rx->expected, (unsigned long)f->seq);
        rx_close(rx);
        return 0;
    }
    int wr = filesystem_file_write(rx->f, f->payload, f->len);
    if (wr != (int)f->len) {
        LOGF("[wifi-sd] DATA write failed len=%u wr=%d", f->len, wr);
        rx_close(rx);
        return 0;
    }
    rx->written += f->len;
    rx->frames++;
    if ((rx->written % (16u * 1024u)) == 0 || rx->written == rx->expected)
        LOGF("[wifi-sd] DATA progress %lu/%lu frames=%lu", (unsigned long)rx->written,
             (unsigned long)rx->expected, (unsigned long)rx->frames);
    return 1;
}

static int rx_end(file_rx_t *rx, const kesp_frame_t *f)
{
    (void)f;
    if (!rx->f) {
        LOG("[wifi-sd] END without open");
        return 0;
    }
    rx_close(rx);
    if (rx->written != rx->expected) {
        LOGF("[wifi-sd] WIFI_SD_FAIL %s written=%lu expected=%lu", rx->rel_path,
             (unsigned long)rx->written, (unsigned long)rx->expected);
        return 0;
    }
    LOGF("[wifi-sd] WIFI_SD_OK %s %lu bytes frames=%lu", rx->rel_path,
         (unsigned long)rx->written, (unsigned long)rx->frames);
    return 1;
}

static void run_receiver_forever(void)
{
    file_rx_t rx;
    uint32_t seq = 0;
    uint32_t frames = 0;
    uint32_t idle = 0;
    memset(&rx, 0, sizeof(rx));
    open_spi_case(&s_best);
    LOG("[wifi-sd] receiver loop start");

    for (;;) {
        if (wait_if_paused("receiver")) {
            rx_close(&rx);
            return;
        }

        amp_set(false);
        int r = transfer_once(&s_best, seq++);
        kesp_frame_t f;
        int off = -1;
        if (!decode_frame(s_rx, r > s_best.wire_len ? s_best.wire_len : r, &f, &off)) {
            idle++;
            if ((idle % 1000u) == 0)
                LOGF("[wifi-sd] receiver idle=%lu r=%d rx0=%02x %02x %02x %02x",
                     (unsigned long)idle, r, s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        idle = 0;
        frames++;
        if (f.type == KESP_F_BEGIN) {
            (void)rx_begin(&rx, &f);
        } else if (f.type == KESP_F_DATA) {
            (void)rx_data(&rx, &f);
        } else if (f.type == KESP_F_END) {
            (void)rx_end(&rx, &f);
        } else if (f.type == KESP_F_ERROR) {
            LOGF("[wifi-sd] ESP_ERROR seq=%lu msg=%.*s", (unsigned long)f.seq, f.len, f.payload);
            rx_close(&rx);
        } else if (f.type != KESP_F_IDLE) {
            LOGF("[wifi-sd] unknown frame type=%u seq=%lu off=%d", f.type, (unsigned long)f.seq, off);
        }
        if ((frames % 512u) == 0)
            LOGF("[wifi-sd] receiver frames=%lu last_type=%u", (unsigned long)frames, f.type);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void esp_spi_link_run_forever(void)
{
    for (;;) {
        amp_set(false);
        s_restart_after_pause = 0;

        LOG("[wifi-sd] waiting ESP UART marker: kesp: spi slave ready");
        while (!esp_uart_log_spi_ready()) {
            wait_if_paused("wait-ready");
            amp_set(false);
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (s_restart_after_pause)
            continue;

        LOG("[wifi-sd] ESP marker detected, scan SPI link");
        run_scan();

        if (s_restart_after_pause)
            continue;

        if (s_have_best)
            run_receiver_forever();
        else
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void esp_spi_link_task(void *arg)
{
    (void)arg;
    esp_spi_link_run_forever();
}

void esp_spi_link_start(void)
{
    if (s_started) {
        LOG("[wifi-sd] receiver task already running");
        return;
    }

    s_started = 1;
    xTaskCreate(esp_spi_link_task, "esp_spi", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    LOG("[wifi-sd] receiver task started");
}
