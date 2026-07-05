#pragma once
#include <stdint.h>

/*
 * Clean K210 SD low-level bring-up probe.
 *
 * This is intentionally separate from the SDK C++ storage wrapper.  It follows
 * the MaixPy / Kendryte standalone order for SPI1 + GPIOHS CS just far enough
 * to prove the first SD SPI-mode milestone:
 *
 *   CS high -> 80 dummy clocks -> CS low -> CMD0 -> R1 response.
 *
 * Expected first good milestone:
 *   [sdcard] CMD0 r=01
 */
uint8_t k210_maixpy_sd_probe_cmd0(void);
