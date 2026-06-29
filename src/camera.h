#pragma once
#include <stdint.h>

/* DVP camera (GC0328) bring-up + capture.
 * cam_start(): power + minimal RGB565 init + DVP capture pipeline.
 *   Returns the chip id (>=0) and fills name_out, or -1 on failure.
 * cam_preview_forever(): capture frames and blit them to the LCD; never returns.
 */
int  cam_start(char *name_out, int len);
int  cam_capture_rgb565(const uint16_t **pixels, int *w, int *h);
void cam_preview_forever(void);
