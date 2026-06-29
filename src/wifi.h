#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Bring up the ESP8285 (UART2 AT firmware) and join the configured AP.
 * On success writes the dotted-quad STA IP into ip_out and returns true. */
bool wifi_connect(char *ip_out, int ip_len);
bool wifi_push_bmp_snapshot(const char *host, int port, const uint16_t *rgb565, int w, int h);
bool wifi_serve_bmp_snapshot(const uint16_t *rgb565, int w, int h, int port);
