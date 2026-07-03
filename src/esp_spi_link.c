#include "esp_spi_link.h"
#include "pinout.h"
#include "log.h"
#include "diag_screen.h"
#include "amp.h"

#include <FreeRTOS.h>
#include <task.h>
#include <devices.h>
#include <filesystem.h>
#include <fpioa.h>
#include <stdio.h>
#include <string.h>

#define FRAME_MAGIC 0x5053454bu
#define FRAME_BYTES 32
#define DATA_BYTES 20
#define ESP_SPI_HZ 10000000.0

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
static TickType_t s_last_tick;
static char s_name[64];

static void close_file(void)
{
    if (s_file) {
        filesystem_file_close(s_file);
        s_file = 0;
    }
}

static int safe_name(const char *s)
{
    return s[0] && s[0] != '/' && s[0] != '\\' && !strstr(s, "..");
}

static int read_frame(kframe_t *fr)
{
    uint8_t tx[34] = { 3, 0 };
    uint8_t rx[34] = { 0 };
    int r = spi_dev_transfer_full_duplex(s_dev, tx, sizeof(tx), rx, sizeof(rx));
    if (r < (int)sizeof(rx))
        return 0;
    memcpy(fr, rx + 2, sizeof(*fr));
    return fr->magic == FRAME_MAGIC;
}

static void begin_file(const kframe_t *fr)
{
    close_file();
    uint8_t n = fr->len < DATA_BYTES ? fr->len : DATA_BYTES;
    memcpy(s_name, fr->data, n);
    s_name[n] = 0;
    if (!safe_name(s_name))
        return;
    char path[96];
    snprintf(path, sizeof(path), "/fs/0/%s", s_name);
    s_file = filesystem_file_open(path, FILE_ACCESS_WRITE, FILE_MODE_CREATE_ALWAYS);
    s_expected = fr->value;
    s_written = 0;
    LOGF("[wifi-spi] BEGIN %s %lu", s_name, (unsigned long)s_expected);
    diag_printf(4, "WiFi %.20s", s_name);
    diag_printf(5, "%lu bytes", (unsigned long)s_expected);
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
}

static void speed_tick(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t dt = now - s_last_tick;
    if (dt < pdMS_TO_TICKS(1000))
        return;
    uint32_t d = s_total - s_last_total;
    uint32_t kbps = d * 1000u / 1024u / (dt * portTICK_PERIOD_MS);
    if (kbps || s_file) {
        LOGF("[wifi-spi] %lu kB/s total=%lu file=%lu/%lu",
             (unsigned long)kbps, (unsigned long)s_total,
             (unsigned long)s_written, (unsigned long)s_expected);
        diag_printf(7, "%lu kB/s", (unsigned long)kbps);
        diag_printf(8, "%lu/%lu", (unsigned long)s_written, (unsigned long)s_expected);
    }
    s_last_total = s_total;
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
    s_dev = spi_get_device(spi, SPI_MODE_0, SPI_FF_STANDARD, 1u, 8);
    configASSERT(s_dev);
    spi_dev_set_clock_rate(s_dev, ESP_SPI_HZ);
    LOG("[wifi-spi] ready");
    diag_line(3, "WiFi/SPI ready");
}

void esp_spi_link_run_forever(void)
{
    amp_set(false);
    init_spi();
    s_last_tick = xTaskGetTickCount();
    for (;;) {
        kframe_t fr;
        if (read_frame(&fr)) {
            if (fr.type == FT_BEGIN) begin_file(&fr);
            else if (fr.type == FT_DATA) data_file(&fr);
            else if (fr.type == FT_END) end_file();
        }
        speed_tick();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
