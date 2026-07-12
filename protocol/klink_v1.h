#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KLINK_V1_MAGIC          0x4b4cu
#define KLINK_V1_VERSION        1u
#define KLINK_V1_CELL_BYTES     64u
#define KLINK_V1_PAYLOAD_BYTES  48u
#define KLINK_V1_CHANNELS       8u

#define KLINK_CHANNEL_TYPE(channel, type) \
    ((uint8_t)((((uint8_t)(channel) & 0x07u) << 5) | ((uint8_t)(type) & 0x1fu)))
#define KLINK_CHANNEL(value) ((uint8_t)(((uint8_t)(value) >> 5) & 0x07u))
#define KLINK_TYPE(value)    ((uint8_t)((uint8_t)(value) & 0x1fu))
#define KLINK_ACK_CREDIT(channel, credit) \
    ((uint8_t)((((uint8_t)(channel) & 0x07u) << 5) | ((uint8_t)(credit) & 0x1fu)))
#define KLINK_ACK_CHANNEL(value) ((uint8_t)(((uint8_t)(value) >> 5) & 0x07u))
#define KLINK_CREDIT(value)      ((uint8_t)((uint8_t)(value) & 0x1fu))

enum klink_v1_channel {
    KLINK_CH_CONTROL = 0,
    KLINK_CH_ROBOT = 1,
    KLINK_CH_BULK = 2,
    KLINK_CH_AUDIO_OUT = 3,
    KLINK_CH_AUDIO_IN = 4,
    KLINK_CH_CAMERA = 5,
    KLINK_CH_DIAG = 6,
    KLINK_CH_RESERVED = 7,
};

enum klink_v1_type {
    KLINK_T_IDLE = 0,
    KLINK_T_OPEN = 1,
    KLINK_T_DATA = 2,
    KLINK_T_CLOSE = 3,
    KLINK_T_ABORT = 4,
    KLINK_T_STATUS = 5,
    KLINK_T_FAULT = 6,
    KLINK_T_CAPABILITIES = 7,
};

enum klink_v1_flags {
    KLINK_F_ACK_VALID = 1u << 0,
    KLINK_F_RELIABLE = 1u << 1,
    KLINK_F_FIRST = 1u << 2,
    KLINK_F_LAST = 1u << 3,
    KLINK_F_URGENT = 1u << 4,
    KLINK_F_CREDIT_VALID = 1u << 5,
};

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define KLINK_PACKED
#else
#define KLINK_PACKED __attribute__((packed))
#endif

typedef struct KLINK_PACKED klink_v1_cell {
    uint16_t magic;
    uint8_t version;
    uint8_t channel_type;
    uint16_t sequence;
    uint16_t acknowledgement;
    uint8_t payload_length;
    uint8_t ack_channel_credit;
    uint16_t flags;
    uint8_t payload[KLINK_V1_PAYLOAD_BYTES];
    uint32_t crc32;
} klink_v1_cell_t;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(__cplusplus)
static_assert(sizeof(klink_v1_cell_t) == KLINK_V1_CELL_BYTES, "KLINK v1 cell must be 64 bytes");
extern "C" {
#else
_Static_assert(sizeof(klink_v1_cell_t) == KLINK_V1_CELL_BYTES, "KLINK v1 cell must be 64 bytes");
#endif

uint32_t klink_v1_crc32(const void *data, size_t size);
void klink_v1_cell_clear(klink_v1_cell_t *cell, uint8_t channel, uint8_t type);
bool klink_v1_cell_set_payload(klink_v1_cell_t *cell, const void *payload, size_t size);
void klink_v1_cell_finalize(klink_v1_cell_t *cell);
bool klink_v1_cell_validate(const klink_v1_cell_t *cell);

#if defined(__cplusplus)
}
#endif

#undef KLINK_PACKED
