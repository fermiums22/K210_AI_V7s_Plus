#pragma once

#include <stdint.h>

void diag_screen_set_enabled(int enabled);
void diag_clear(const char *title);
void diag_line(int line, const char *text);
void diag_printf(int line, const char *fmt, ...);
