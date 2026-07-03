#include "esp_spi_link.h"
#include "pinout.h"
#include "log.h"
#include "diag_screen.h"
#include "amp.h"
#include "esp_uart_log.h"

#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <filesystem.h>
#include <fpioa.h>
#include <stdio.h>
#include <string.h>

#define FRAME_MAGIC 0x5053454bu
#define FRAME_BYTES 32
#define SPI_WIRE_BYTES 34
#define DATA_BYTES 20
#define BAD_DUMP_LIMIT 8

/* Bring-up speed. Do not free-run the Arduino ESP8266 SPISlave when the queue
 * is empty: continuous idle polling crashes it. Poll only after ESP reports an
 * incoming TCP PUT on UART; then K210 drains frames into SD. */
#define ESP_SPI_HZ 1000000.0
#define BAD_DELAY_AFTER_FRAMES 16u
#define NO_GOOD_ABORT_FRAMES 512u

enum { FT_IDLE = 0, FT_BEGIN = 1, FT_DATA = 2, FT_END = 3, FT_INFO = 4 };

typedef struct {
    uint32_t magic;
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    uint32_t value;
    uint8_t data[DATA_BYTES];
} __attribute__((packed)) kframe_t;

static handle_t s_dev;
static handle_t s_file;
static uint32_t s_expected, s_written, s_total, s_last_total;
static uint32_t s_good_frames, s_bad_frames, s_last_good, s_last_bad;
static uint32_t s_bad_dumps;
static volatile int s_paused;
static volatile int s_started;
static TickType_t s_last_tick;
static char s_name[64];

static void close_file(void)
{
    if (s_file) {
        filesystem_file_close(s_file);
        s_file = 0;
    }
}

void esp_spi_link_pause(int pause)
{
    int new_state = pause ? 1 : 0;
    if (s_paused == new_state)
        return;
    s_paused = new_state;
    if (s_paused) {
        close_file();
        LOG("[wifi-spi] paused");
        diag_line(12, "WiFi/SPI paused");
    } else {
        LOG("[wifi-spi] resumed");
        diag_line(12, "WiFi/SPI receiver");
    }
}

static int safe_name(const char *s)
{
    return s[0] && s[0] != '/' && s[0] != '\\' && !strstr(s, "..");
}

static uint32_t rd32le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void dump_bad_frame(const uint8_t *rx, int r)
{
    if (s_bad_dumps >= BAD_DUMP_LIMIT && (s_bad_frames & 0x3ffu) != 0)
        return;

    s_bad_dumps++;
    LOGF("[wifi-spi] bad raw r=%d rx=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x m0=%08lx m1=%08lx m2=%08lx",
         r,
         rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7],
         rx[8], rx[9], rx[10], rx[11],
         (unsigned long)rd32le(rx + 0),
         (unsigned long)rd32le(rx + 1),
         (unsigned long)rd32le(rx + 2));
}

static int read_frame(kframe_t *fr)
{
    uint8_t tx[SPI_WIRE_BYTES] = { 3, 0 };
    uint8_t rx[SPI_WIRE_BYTES] = { 0 };
    int r = spi_dev_transfer_full_duplex(s_dev, tx, sizeof(tx), rx, sizeof(rx));
    if (r < (int)sizeof(rx)) {
        s_bad_frames++;
        dump_bad_frame(rx, r);
        return 0;
    }

    for (int off = 0; off <= 2; off++) {
        if (rd32le(rx + off) == FRAME_MAGIC) {
            memcpy(fr, rx + off, sizeof(*fr));
            s_good_frames++;
            if (off != 2)
                LOGF("[wifi-spi] frame magic offset=%d", off);
            return 1;
        }
    }

    s_bad_frames++;
    dump_bad_frame(rx, r);
    return 0;
}

static void begin_file(const kframe_t *fr)
{
    close_file();
    uint8_t n = fr->len < DATA_BYTES ? fr->len : DATA_BYTES;
    memcpy(s_name, fr->data, n);
    s_name[n] = 0;
    if (!safe_name(s_name)) {
        LOGF("[wifi-spi] bad file name: %s", s_name);
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "/fs/0/%s", s_name);
    s_file = filesystem_file_open(path, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
    s_expected = fr->value;
    s_written = 0;
    LOGF("[wifi-spi] BEGIN %s %lu", s_name, (unsigned long)s_expected);
    diag_printf(4, "WiFi %.20s", s_name);
    diag_printf(5, "%lu bytes", (unsigned long)s_expected);
    if (!s_file)
        LOGF("[wifi-spi] open failed: %s", path);
}

static void data_file(const kframe_t *fr)
{
    if (!s_file || fr->len == 0 || fr->len > DATA_BYTES)
        return;
    if (filesystem_file_write(s_file, fr->data, fr->len) == (int)fr->len) {
        s_written += fr->len;
        s_total += fr->len;
    }
}

static void end_file(void)
{
    close_file();
    LOGF("[wifi-spi] END %s %lu/%lu", s_name, (unsigned long)s_written, (unsigned long)s_expected);
    diag_printf(6, "Done %lu/%lu", (unsigned long)s_written, (unsigned long)s_expected);
    esp_uart_log_clear_put_active();
    LOG("[wifi-spi] PUT done, stop polling until next TCP PUT");
}

static void speed_tick(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t dt = now - s_last_tick;
    if (dt < pdMS_TO_TICKS(1000))
        return;
    uint32_t d = s_total - s_last_total;
    uint32_t dg = s_good_frames - s_last_good;
    uint32_t db = s_bad_frames - s_last_bad;
    uint32_t kbps = d * 1000u / 1024u / (dt * portTICK_PERIOD_MS);
    if (kbps || s_file || dg || db) {
        LOGF("[wifi-spi] %lu kB/s total=%lu file=%lu/%lu frames ok=%lu bad=%lu",
             (unsigned long)kbps, (unsigned long)s_total,
             (unsigned long)s_written, (unsigned long)s_expected,
             (unsigned long)dg, (unsigned long)db);
        diag_printf(7, "%lu kB/s", (unsigned long)kbps);
        diag_printf(8, "%lu/%lu", (unsigned long)s_written, (unsigned long)s_expected);
    }
    s_last_total = s_total;
    s_last_good = s_good_frames;
    s_last_bad = s_bad_frames;
    s_last_tick = now;
}

static void init_spi(void)
{
    fpioa_set_function(PIN_ESP_SPI_CS, FUNC_SPI0_SS0);
    fpioa_set_function(PIN_ESP_SPI_CLK, FUNC_SPI0_SCLK);
    fpioa_set_function(PIN_ESP_SPI_MOSI, FUNC_SPI0_D0);
    fpioa_set_function(PIN_ESP_SPI_MISO, FUNC_SPI0_D1);
    handle_t spi = io_open("/dev/spi0");
    configASSERT(spi);
    s_dev = spi_get_device(spi, SPI_MODE_1, SPI_FF_STANDARD, 1u, 8);
    configASSERT(s_dev);
    spi_dev_set_clock_rate(s_dev, ESP_SPI_HZ);
    LOG("[wifi-spi] ready hz=1000000 mode=1 gated-by-TCP-PUT");
    diag_line(3, "WiFi/SPI gated 1M");
}

void esp_spi_link_run_forever(void)
{
    amp_set(false);
    init_spi();
    s_last_tick = xTaskGetTickCount();
    int announced_ready = 0;
    int announced_wait = 0;
    uint32_t bad_frames = 0;
    uint32_t frames_since_good = 0;

    for (;;) {
        if (s_paused) {
            speed_tick();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!esp_uart_log_spi_ready()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!announced_ready) {
            announced_ready = 1;
            LOG("[wifi-spi] ESP SPI ready, waiting TCP PUT before polling");
            diag_line(3, "WiFi/SPI wait PUT");
        }
        if (!esp_uart_log_put_active()) {
            if (!announced_wait) {
                announced_wait = 1;
                LOG("[wifi-spi] idle, no SPI polling until TCP PUT");
            }
            speed_tick();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (announced_wait) {
            announced_wait = 0;
            bad_frames = 0;
            frames_since_good = 0;
            LOG("[wifi-spi] TCP PUT active, polling frames");
        }

        kframe_t fr;
        if (read_frame(&fr)) {
            bad_frames = 0;
            frames_since_good = 0;
            if (fr.type == FT_BEGIN) begin_file(&fr);
            else if (fr.type == FT_DATA) data_file(&fr);
            else if (fr.type == FT_END) end_file();
        } else {
            bad_frames++;
            frames_since_good++;
            if (!s_file && frames_since_good >= NO_GOOD_ABORT_FRAMES) {
                LOG("[wifi-spi] no valid frames after TCP PUT, stop polling to protect ESP");
                esp_uart_log_clear_put_active();
                frames_since_good = 0;
            }
        }

        speed_tick();

        if (s_file) {
            taskYIELD();
        } else if (bad_frames >= BAD_DELAY_AFTER_FRAMES) {
            bad_frames = 0;
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            taskYIELD();
        }
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
        LOG("[wifi-spi] receiver task already running");
        return;
    }
    s_started = 1;
    xTaskCreate(esp_spi_link_task, "wifi_spi", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    LOG("[wifi-spi] receiver task started");
}
