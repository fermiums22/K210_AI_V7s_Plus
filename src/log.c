#include "log.h"
#include "pinout.h"
#include <fpioa.h>
#include <platform.h>
#include <stdarg.h>
#include <stdio.h>
#include <sysctl.h>
#include <uarths.h>

static volatile uarths_t *const REG_UARTHS = (volatile uarths_t *)UARTHS_BASE_ADDR;

static void uarths_set_baud(uint32_t baud)
{
    uint32_t freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
    uint32_t div = freq / baud;
    if (div > 0)
        div--;
    if (div > 0xffffu)
        div = 0xffffu;
    REG_UARTHS->div.div = (uint16_t)div;
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
