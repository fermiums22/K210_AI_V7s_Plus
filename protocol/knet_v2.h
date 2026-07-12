#ifndef KNET_V2_H
#define KNET_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KNET_V2_MAGIC         0x32544e4bu
#define KNET_V2_VERSION       2u
#define KNET_V2_HEADER_BYTES  24u
#define KNET_V2_MAX_PAYLOAD   (64u * 1024u)

typedef enum knet_v2_channel {
    KNET_V2_CH_VIDEO = 1,
    KNET_V2_CH_MIC = 2,
    KNET_V2_CH_TELEMETRY = 3,
    KNET_V2_CH_LOG = 4,
    KNET_V2_CH_AUDIO = 16,
    KNET_V2_CH_CONTROL = 17,
    KNET_V2_CH_K210_FIRMWARE = 18,
    KNET_V2_CH_ESP_FIRMWARE = 19,
} knet_v2_channel_t;

typedef struct __attribute__((packed, aligned(4))) knet_v2_header {
    uint32_t magic;
    uint8_t version;
    uint8_t channel;
    uint8_t flags;
    uint8_t header_bytes;
    uint32_t sequence;
    uint32_t payload_length;
    uint32_t payload_crc32;
    uint32_t header_crc32;
} knet_v2_header_t;

_Static_assert(sizeof(knet_v2_header_t) == KNET_V2_HEADER_BYTES,
               "knet header must be 24 bytes");

uint32_t knet_v2_crc32(const void *data, size_t length);
void knet_v2_header_finalize(knet_v2_header_t *header);
bool knet_v2_header_valid(const knet_v2_header_t *header);

#endif
