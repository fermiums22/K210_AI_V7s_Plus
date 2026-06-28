#pragma once
#include <stdbool.h>

/* Bring up the ESP8285 (UART2 AT firmware) and join the configured AP.
 * On success writes the dotted-quad STA IP into ip_out and returns true. */
bool wifi_connect(char *ip_out, int ip_len);
