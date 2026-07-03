#include "esp_spi_link.h"
#include "pinout.h"
#include "log.h"
#include "amp.h"
#include "esp_uart_log.h"

#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <fpioa.h>
#include <platform.h>
#include <stdint.h>
#include <string.h>

#define FRAME_MAGIC     0x5053454bu /* bytes on wire: 4b 45 53 50 == "KESP" */
#define RX_MAX          64
#define TEST_WARMUP     3u
#define TEST_FRAMES     64u
#define GPIOHS_CS       0
#define GPIOHS_BIT(n)   (1u << (n))
#define FUNC_GPIOHS_NUM(n) ((fpioa_function_t)(24 + (n)))

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
    LOGF("[pure-spi] pause=%d", s_pause);
}

static int wait_if_paused(const char *where)
{
    if (!s_pause)
        return 0;

    LOGF("[pure-spi] paused at %s", where);
    s_restart_after_pause = 1;
    while (s_pause) {
        amp_set(false);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    LOG("[pure-spi] resumed; restart from ESP-ready wait");
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
    gpiohs_cs_write(1); /* ESP8266 SPISlave CS is active-low. */
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

static void log_sample(const spi_case_t *c, int r, int off)
{
    LOGF("[pure-spi] sample mode=%d hz=%lu cs=%s map=%s len=%d r=%d off=%d rx=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x m0=%08lx m1=%08lx m2=%08lx",
         c->mode,
         (unsigned long)c->hz,
         cs_name(c->manual_cs),
         map_name(c->swap_d0_d1),
         c->wire_len,
         r,
         off,
         s_rx[0], s_rx[1], s_rx[2], s_rx[3],
         s_rx[4], s_rx[5], s_rx[6], s_rx[7],
         s_rx[8], s_rx[9], s_rx[10], s_rx[11],
         (unsigned long)rd32le(s_rx + 0),
         (unsigned long)rd32le(s_rx + 1),
         (unsigned long)rd32le(s_rx + 2));
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
    open_spi_case(&c);
    c.good = 0;
    c.bad = 0;
    c.last_magic_off = -1;

    LOGF("[pure-spi] case start mode=%d hz=%lu cs=%s map=%s len=%d",
         c.mode,
         (unsigned long)c.hz,
         cs_name(c.manual_cs),
         map_name(c.swap_d0_d1),
         c.wire_len);

    for (uint32_t i = 0; i < TEST_WARMUP + TEST_FRAMES; i++) {
        if (wait_if_paused("case"))
            return c;

        amp_set(false);
        int r = transfer_once(&c, i);
        int off = find_magic(s_rx, r > c.wire_len ? c.wire_len : r);

        if (i == TEST_WARMUP)
            log_sample(&c, r, off);

        if (i >= TEST_WARMUP) {
            if (r >= c.wire_len && off >= 0) {
                c.good++;
                c.last_magic_off = off;
            } else {
                c.bad++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    LOGF("[pure-spi] case done mode=%d hz=%lu cs=%s map=%s len=%d good=%lu bad=%lu last_off=%d",
         c.mode,
         (unsigned long)c.hz,
         cs_name(c.manual_cs),
         map_name(c.swap_d0_d1),
         c.wire_len,
         (unsigned long)c.good,
         (unsigned long)c.bad,
         c.last_magic_off);

    if (c.good)
        remember_best(&c);

    vTaskDelay(pdMS_TO_TICKS(20));
    return c;
}

static void run_scan(void)
{
    static const uint32_t hz_list[] = { 100000u, 500000u, 1000000u };
    static const int len_list[] = { 32, 34 };

    s_have_best = 0;
    memset(&s_best, 0, sizeof(s_best));
    LOG("[pure-spi] scan start: mode 0/1/2/3, normal/swap D0D1, hw/gpiohs CS, 32/34-byte transfers");

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
        LOGF("[pure-spi] VERDICT SPI_OK best mode=%d hz=%lu cs=%s map=%s len=%d good=%lu bad=%lu off=%d",
             s_best.mode,
             (unsigned long)s_best.hz,
             cs_name(s_best.manual_cs),
             map_name(s_best.swap_d0_d1),
             s_best.wire_len,
             (unsigned long)s_best.good,
             (unsigned long)s_best.bad,
             s_best.last_magic_off);
    } else {
        LOG("[pure-spi] VERDICT SPI_FAIL no KESP magic in any tested mode/map/cs/length");
        LOG("[pure-spi] If ESP UART rx counter increases but K210 sees no magic, suspect ESP8266 SPISlave protocol/alignment, not physical pins");
    }
}

static void run_best_forever(void)
{
    uint32_t good = 0;
    uint32_t bad = 0;
    TickType_t last = xTaskGetTickCount();

    open_spi_case(&s_best);
    LOG("[pure-spi] stability loop on best case");

    for (;;) {
        if (wait_if_paused("stable-loop"))
            return;

        amp_set(false);
        int r = transfer_once(&s_best, good + bad);
        int off = find_magic(s_rx, r > s_best.wire_len ? s_best.wire_len : r);
        if (r >= s_best.wire_len && off >= 0)
            good++;
        else
            bad++;

        TickType_t now = xTaskGetTickCount();
        if (now - last >= pdMS_TO_TICKS(1000)) {
            last = now;
            LOGF("[pure-spi] stable mode=%d hz=%lu cs=%s map=%s len=%d good=%lu bad=%lu last_off=%d rx0=%02x %02x %02x %02x",
                 s_best.mode,
                 (unsigned long)s_best.hz,
                 cs_name(s_best.manual_cs),
                 map_name(s_best.swap_d0_d1),
                 s_best.wire_len,
                 (unsigned long)good,
                 (unsigned long)bad,
                 off,
                 s_rx[0], s_rx[1], s_rx[2], s_rx[3]);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void esp_spi_link_run_forever(void)
{
    for (;;) {
        amp_set(false);
        s_restart_after_pause = 0;

        LOG("[pure-spi] waiting ESP UART marker: kesp: spi slave ready");
        while (!esp_uart_log_spi_ready()) {
            wait_if_paused("wait-ready");
            amp_set(false);
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (s_restart_after_pause)
            continue;

        LOG("[pure-spi] ESP marker detected, start pure SPI peripheral scan");
        run_scan();

        if (s_restart_after_pause)
            continue;

        if (s_have_best)
            run_best_forever();
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
        LOG("[pure-spi] scanner task already running");
        return;
    }

    s_started = 1;
    xTaskCreate(esp_spi_link_task, "esp_spi", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    LOG("[pure-spi] scanner task started");
}
