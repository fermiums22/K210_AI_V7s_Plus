#include "log.h"
#include "pinout.h"
#include <fpioa.h>
#include <platform.h>
#include <stdarg.h>
#include <stdio.h>
#include <sysctl.h>
#include <uarths.h>

static volatile uarths_t *const REG_UARTHS = (volatile uarths_t *)UARTHS_BASE_ADDR;

/*
 * After bootloader jump the SDK clock registers can report the IN0 26 MHz path
 * even while the UARTHS divider is effectively driven from the normal K210 CPU
 * clock domain.  Treat low reported CPU clocks as unreliable and use the normal
 * Maix/K210 390 MHz CPU clock for the debug/service UARTHS divider.
 */
#define K210_APP_CPU_HZ_FALLBACK 390000000u
#define K210_APP_CPU_HZ_MIN_OK   100000000u

static uint32_t s_log_cpu_hz;
static uint32_t s_log_div;
static uint32_t s_log_reported_cpu_hz;
static uint32_t s_log_baud;

static uint32_t app_uart_clock_hz(void)
{
    uint32_t freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
    s_log_reported_cpu_hz = freq;
    if (freq < K210_APP_CPU_HZ_MIN_OK)
        freq = K210_APP_CPU_HZ_FALLBACK;
    s_log_cpu_hz = freq;
    return freq;
}

static void uarths_set_baud(uint32_t baud)
{
    uint32_t freq = app_uart_clock_hz();
    uint32_t div = freq / baud;
    if (div > 0)
        div--;
    if (div > 0xffffu)
        div = 0xffffu;
    s_log_baud = baud;
    s_log_div = div;
    REG_UARTHS->div.div = (uint16_t)div;
}

void log_set_baud(unsigned int baud)
{
    uarths_set_baud((uint32_t)baud);
}

void log_init(void)
{
    fpioa_set_function(PIN_DBG_RX, FUNC_UARTHS_RX);
    fpioa_set_function(PIN_DBG_TX, FUNC_UARTHS_TX);
    uarths_init();
    uarths_set_baud(APP_LOG_BAUD);
}

void log_puts(const char *s)
{
    if (!s)
        return;
    uarths_puts(s);
    uarths_puts("\r\n");
}

void log_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    buf[sizeof(buf) - 1] = 0;
    uarths_puts(buf);
    uarths_puts("\r\n");
}

void log_dump_uart_clock(void)
{
    log_printf("[log] baud=%lu reported_cpu=%lu used_cpu=%lu div=%lu",
               (unsigned long)s_log_baud,
               (unsigned long)s_log_reported_cpu_hz,
               (unsigned long)s_log_cpu_hz,
               (unsigned long)s_log_div);
}
