#include "klink_v1.h"

#include <string.h>

static const uint32_t s_crc32_nibble[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
};

uint32_t klink_v1_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;

    if (!bytes && size != 0u)
        return 0u;

    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ s_crc32_nibble[crc & 0x0fu];
        crc = (crc >> 4) ^ s_crc32_nibble[crc & 0x0fu];
    }
    return crc ^ 0xffffffffu;
}

void klink_v1_cell_clear(klink_v1_cell_t *cell, uint8_t channel, uint8_t type)
{
    if (!cell)
        return;
    memset(cell, 0, sizeof(*cell));
    cell->magic = KLINK_V1_MAGIC;
    cell->version = KLINK_V1_VERSION;
    cell->channel_type = KLINK_CHANNEL_TYPE(channel, type);
}

bool klink_v1_cell_set_payload(klink_v1_cell_t *cell, const void *payload, size_t size)
{
    if (!cell || size > KLINK_V1_PAYLOAD_BYTES || (!payload && size != 0u))
        return false;
    memset(cell->payload, 0, sizeof(cell->payload));
    if (size != 0u)
        memcpy(cell->payload, payload, size);
    cell->payload_length = (uint8_t)size;
    return true;
}

void klink_v1_cell_finalize(klink_v1_cell_t *cell)
{
    if (!cell)
        return;
    cell->crc32 = 0u;
    cell->crc32 = klink_v1_crc32(cell, offsetof(klink_v1_cell_t, crc32));
}

bool klink_v1_cell_validate(const klink_v1_cell_t *cell)
{
    if (!cell || cell->magic != KLINK_V1_MAGIC || cell->version != KLINK_V1_VERSION ||
        cell->payload_length > KLINK_V1_PAYLOAD_BYTES ||
        KLINK_CHANNEL(cell->channel_type) >= KLINK_V1_CHANNELS)
        return false;
    return klink_v1_crc32(cell, offsetof(klink_v1_cell_t, crc32)) == cell->crc32;
}
