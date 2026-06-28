#pragma once
#include <stdint.h>

/* DVP camera bring-up.
 * cam_probe() powers the sensor (XCLK + reset), reads its chip ID over SCCB,
 * and writes a human name into name_out. Returns the id (>=0) or -1. */
int cam_probe(char *name_out, int len);
