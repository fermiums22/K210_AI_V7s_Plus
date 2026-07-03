#include <FreeRTOS.h>
#include <task.h>

#include <stdio.h>
#include <string.h>

#include "amp.h"
#include "log.h"
#include "lcd.h"
#include "sd.h"
#include "sd_uart.h"
#include "esp_uart_log.h"
#include "esp_spi_link.h"

#define SCREEN_ROWS 18
#define SCREEN_Y0   0
#define SCREEN_DY   16

static int s_screen_ready;
static int s_row;

static void screen_clear(void)
{
    lcd_clear(BLACK);
    lcd_draw_string_bg(0, 0, "K210 STACK DIAG", YELLOW, BLACK);
    lcd_draw_string_bg(0, 16, "UART: COM12 921600", CYAN, BLACK);
    s_row = 2;
}

static void screen_line_color(const char *s, uint16_t color)
{
    if (!s_screen_ready)
        return;

    if (s_row >= SCREEN_ROWS)
        screen_clear();

    char line[40];
    memset(line, ' ', sizeof(line));
    line[sizeof(line) - 1] = 0;
    size_t n = strlen(s);
    if (n > sizeof(line) - 1)
        n = sizeof(line) - 1;
    memcpy(line, s, n);

    lcd_draw_string_bg(0, SCREEN_Y0 + s_row * SCREEN_DY, line, color, BLACK);
    s_row++;
}

static void say(const char *s)
{
    LOG(s);
    screen_line_color(s, WHITE);
}

static void ok(const char *s)
{
    char b[96];
    snprintf(b, sizeof(b), "[OK] %s", s);
    LOG(b);
    screen_line_color(b, GREEN);
}

static void fail(const char *s)
{
    char b[96];
    snprintf(b, sizeof(b), "[FAIL] %s", s);
    LOG(b);
    screen_line_color(b, RED);
}

static void wait_status(const char *s)
{
    char b[96];
    snprintf(b, sizeof(b), "[WAIT] %s", s);
    LOG(b);
    screen_line_color(b, CYAN);
}

static void wip(const char *s)
{
    char b[96];
    snprintf(b, sizeof(b), "[WIP] %s", s);
    LOG(b);
    screen_line_color(b, ORANGE);
}

static int mount_sd_once(void)
{
    say("SD mount...");
    amp_set(false);
    int sd_ok = sd_mount();
    amp_set(false);

    if (!sd_ok) {
        fail("SD FAT mount rc=-1");
        return 0;
    }

    int n = sd_list_root();
    char b[64];
    snprintf(b, sizeof(b), "SD FAT root=%d", n);
    ok(b);
    return 1;
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t t = 0;
    for (;;) {
        amp_set(false);
        LOGF("[stack] alive tick=%lu", (unsigned long)t++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    log_init();
    LOG("[stack] K210 diagnostic stack boot");
    LOG("[stack] build target: K210 LCD/CAM/AUDIO/SD/UART/SPI bring-up checkpoint");
    ok("UART LOG PC");

    amp_init();
    amp_set(false);
    ok("Audio AMP control");

    lcd_init();
    lcd_set_direction(DIR_YX_LRUD);
    lcd_clear(BLACK);
    s_screen_ready = 1;
    screen_clear();
    ok("LCD init");
    ok("LCD log overlay");

    int sd_ok = mount_sd_once();

    say("UART services...");
    esp_uart_log_start();
    ok("UART ESP bridge");

    /* Start KSD even when SD mount failed. Otherwise FORMAT_SD would be impossible. */
    sd_uart_service_start();
    ok("PC UART command listener");
    ok("KSD command FORMAT_SD");

    if (sd_ok)
        ok("ESP fast loader cmd FLASH_ESP");
    else
        fail("ESP loader waits SD/FORMAT_SD");

    esp_spi_link_start();
    wait_status("SPI WIFI scanner");
    wait_status("NEW WIFI-SPI KESP proto");

    wip("Camera module restore next");
    wip("MIC/I2S restore next");
    wip("UART STM pin map needed");

    xTaskCreate(heartbeat_task, "stack_hb", 1024, NULL, tskIDLE_PRIORITY + 1, NULL);

    say("Stack diag idle");
    for (;;) {
        amp_set(false);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
