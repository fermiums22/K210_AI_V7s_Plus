#include "log.h"
#include "pinout.h"
#include <fpioa.h>
#include <platform.h>
#include <stdarg.h>
#include <stdio.h>
#include <uarths.h>

void log_init(void)
{
    fpioa_set_function(PIN_DBG_RX, FUNC_UARTHS_RX);
    fpioa_set_function(PIN_DBG_TX, FUNC_UARTHS_TX);
    uarths_init();
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
