#include "knet_v2.h"

uint32_t knet_v2_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void knet_v2_header_finalize(knet_v2_header_t *header)
{
    header->magic = KNET_V2_MAGIC;
    header->version = KNET_V2_VERSION;
    header->header_bytes = KNET_V2_HEADER_BYTES;
    header->header_crc32 =
        knet_v2_crc32(header, offsetof(knet_v2_header_t, header_crc32));
}

bool knet_v2_header_valid(const knet_v2_header_t *header)
{
    return header->magic == KNET_V2_MAGIC &&
           header->version == KNET_V2_VERSION &&
           header->header_bytes == KNET_V2_HEADER_BYTES &&
           header->payload_length <= KNET_V2_MAX_PAYLOAD &&
           header->header_crc32 ==
               knet_v2_crc32(header, offsetof(knet_v2_header_t, header_crc32));
}
