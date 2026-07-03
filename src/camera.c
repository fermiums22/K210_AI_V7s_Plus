#include "camera.h"
#include <stdio.h>

int cam_start(char *name_out, int len)
{
    if (name_out && len > 0)
        snprintf(name_out, len, "camera disabled");
    return -1;
}

int cam_capture_rgb565(const uint16_t **pixels, int *w, int *h)
{
    (void)pixels;
    (void)w;
    (void)h;
    printf("[cam] disabled: rescue boot-safe build\n");
    return 0;
}

void cam_preview_forever(void)
{
    printf("[cam] disabled: rescue boot-safe build\n");
    for (;;) {
    }
}
