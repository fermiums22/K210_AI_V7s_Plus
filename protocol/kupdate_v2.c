#include "kupdate_v2.h"

#include <string.h>

uint32_t kupdate_v2_crc32(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void kupdate_v2_open_finalize(kupdate_v2_open_t *open)
{
    if (!open) return;
    open->magic = KUPDATE_V2_MAGIC;
    open->version = KUPDATE_V2_VERSION;
    open->header_bytes = sizeof(*open);
    open->header_crc32 = kupdate_v2_crc32(open, offsetof(kupdate_v2_open_t, header_crc32));
}

int kupdate_v2_open_valid(const kupdate_v2_open_t *open, uint32_t max_image_size)
{
    if (!open || open->magic != KUPDATE_V2_MAGIC || open->version != KUPDATE_V2_VERSION ||
        open->header_bytes != sizeof(*open) || open->image_size == 0u ||
        open->image_size > max_image_size) return 0;
    return open->header_crc32 == kupdate_v2_crc32(open, offsetof(kupdate_v2_open_t, header_crc32));
}

void kupdate_v2_status_finalize(kupdate_v2_status_t *status)
{
    if (!status) return;
    status->magic = KUPDATE_V2_MAGIC;
    status->status_crc32 = kupdate_v2_crc32(status, offsetof(kupdate_v2_status_t, status_crc32));
}

int kupdate_v2_status_valid(const kupdate_v2_status_t *status)
{
    return status && status->magic == KUPDATE_V2_MAGIC &&
        status->status_crc32 == kupdate_v2_crc32(status, offsetof(kupdate_v2_status_t, status_crc32));
}
