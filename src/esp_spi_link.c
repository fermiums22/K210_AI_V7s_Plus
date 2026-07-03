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
    if (s_started)
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
    gpiohs_cs_write(1);
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

static void setup_pins(int swap_d0_d1, int manual_cs)
{
    if (manual_cs)
        gpiohs_cs_init();
    else
        fpioa_set_function(PIN_ESP_SPI_CS, FUNC_SPI0_SS0);

    fpioa_set_function(PIN_ESP_SPI_CLK, FUNC_SPI0_SCLK);
    fpioa_set_function(PIN_ESP_SPI_MOSI, swap_d0_d1 ? FUNC_SPI0_D1 : FUNC_SPI0_D0);
    fpioa_set_function(PIN_ESP_SPI_MISO, swap_d0_d1 ? FUNC_SPI0_D0 : FUNC_SPI0_D1);
}

static int spi_open_config(int mode, uint32_t hz)
{
    if (!s_spi)
        s_spi = io_open("/dev/spi0");
    if (!s_spi) {
        LOG("[pure-spi] open /dev/spi0 failed");
        return 0;
    }

    s_dev = spi_get_device(s_spi, spi_mode_value(mode), SPI_FF_STANDARD, 1u, 8);
    if (!s_dev) {
        LOG("[pure-spi] get device failed");
        return 0;
    }

    spi_dev_set_clock_rate(s_dev, hz);
    return 1;
}

static void do_transfer(spi_case_t *c)
{
    int len = c->wire_len;
    memset(s_tx, 0xA5, sizeof(s_tx));
    memset(s_rx, 0x00, sizeof(s_rx));
    s_tx[0] = 0x11;
    s_tx[1] = 0x22;
    s_tx[2] = 0x33;
    s_tx[3] = 0x44;

    if (!spi_open_config(c->mode, c->hz))
        return;

    if (c->manual_cs)
        gpiohs_cs_write(0);
    int r = spi_dev_transfer_full_duplex(s_dev, s_tx, len, s_rx, len);
    if (c->manual_cs)
        gpiohs_cs_write(1);

    int off = find_magic(s_rx, r > len ? len : r);
    if (r >= len && off >= 0) {
        c->good++;
        c->last_magic_off = off;
    } else {
        c->bad++;
    }
}

static int better_case(const spi_case_t *a, const spi_case_t *b)
{
    if (a->good != b->good)
        return a->good > b->good;
    if (a->bad != b->bad)
        return a->bad < b->bad;
    return a->hz > b->hz;
}

static void run_scan_once(void)
{
    static const int modes[] = {0, 1, 2, 3};
    static const uint32_t speeds[] = {100000, 500000, 1000000};
    static const int swaps[] = {0, 1};
    static const int manual_cs_values[] = {0, 1};
    static const int wire_lens[] = {32, 34};

    memset(&s_best, 0, sizeof(s_best));
    s_best.bad = 0xffffffffu;
    s_have_best = 0;

    for (unsigned ci = 0; ci < sizeof(manual_cs_values)/sizeof(manual_cs_values[0]); ci++) {
        for (unsigned swi = 0; swi < sizeof(swaps)/sizeof(swaps[0]); swi++) {
            for (unsigned si = 0; si < sizeof(speeds)/sizeof(speeds[0]); si++) {
                for (unsigned mi = 0; mi < sizeof(modes)/sizeof(modes[0]); mi++) {
                    for (unsigned li = 0; li < sizeof(wire_lens)/sizeof(wire_lens[0]); li++) {
                        spi_case_t c;
                        memset(&c, 0, sizeof(c));
                        c.mode = modes[mi];
                        c.hz = speeds[si];
                        c.swap_d0_d1 = swaps[swi];
                        c.manual_cs = manual_cs_values[ci];
                        c.wire_len = wire_lens[li];

                        setup_pins(c.swap_d0_d1, c.manual_cs);

                        for (uint32_t n = 0; n < TEST_WARMUP; n++)
                            do_transfer(&c);
                        c.good = 0;
                        c.bad = 0;
                        c.last_magic_off = -1;
                        for (uint32_t n = 0; n < TEST_FRAMES; n++)
                            do_transfer(&c);

                        if (!s_have_best || better_case(&c, &s_best)) {
                            s_best = c;
                            s_have_best = 1;
                            LOGF("[pure-spi] best mode=%d hz=%lu map=%s cs=%s len=%d good=%lu bad=%lu off=%d",
                                 c.mode, (unsigned long)c.hz, map_name(c.swap_d0_d1), cs_name(c.manual_cs),
                                 c.wire_len, (unsigned long)c.good, (unsigned long)c.bad, c.last_magic_off);
                        }

                        if (wait_if_paused("scan"))
                            return;
                    }
                }
            }
        }
    }
}

static void esp_spi_task(void *arg)
{
    (void)arg;
    LOG("[pure-spi] scanner task started");
    for (;;) {
        LOG("[pure-spi] waiting ESP UART marker: kesp: spi slave ready");
        while (!esp_uart_log_spi_ready()) {
            if (wait_if_paused("wait-ready"))
                goto next_cycle;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        LOG("[pure-spi] ESP ready marker seen; scanning modes");
        run_scan_once();
next_cycle:
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void esp_spi_link_start(void)
{
    if (s_started)
        return;
    s_started = 1;
    xTaskCreate(esp_spi_task, "esp_spi", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}
