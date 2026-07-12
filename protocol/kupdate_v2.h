#pragma once

#include <stddef.h>
#include <stdint.h>

#define KUPDATE_V2_MAGIC             0x3250554bu
#define KUPDATE_V2_VERSION           2u
#define KUPDATE_V2_TCP_PORT          21002u
#define KUPDATE_V2_OPEN_BYTES        48u
#define KUPDATE_V2_DATA_BYTES        44u
#define KUPDATE_V2_SLOT_NONE         0xffu

enum kupdate_v2_state {
    KUPDATE_V2_STATE_READY = 1,
    KUPDATE_V2_STATE_RECEIVING = 2,
    KUPDATE_V2_STATE_VERIFYING = 3,
    KUPDATE_V2_STATE_VERIFIED = 4,
    KUPDATE_V2_STATE_COMMITTED = 5,
    KUPDATE_V2_STATE_BOOTING = 6,
    KUPDATE_V2_STATE_FAILED = 0xff,
};

enum kupdate_v2_error {
    KUPDATE_V2_OK = 0,
    KUPDATE_V2_ERR_PROTOCOL = 1,
    KUPDATE_V2_ERR_SIZE = 2,
    KUPDATE_V2_ERR_STATE = 3,
    KUPDATE_V2_ERR_OFFSET = 4,
    KUPDATE_V2_ERR_FLASH_ERASE = 5,
    KUPDATE_V2_ERR_FLASH_WRITE = 6,
    KUPDATE_V2_ERR_FLASH_READ = 7,
    KUPDATE_V2_ERR_HASH = 8,
    KUPDATE_V2_ERR_METADATA = 9,
    KUPDATE_V2_ERR_LINK = 10,
};

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define KUPDATE_PACKED
#else
#define KUPDATE_PACKED __attribute__((packed))
#endif

typedef struct KUPDATE_PACKED kupdate_v2_open {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t image_size;
    uint8_t image_sha256[32];
    uint32_t header_crc32;
} kupdate_v2_open_t;

typedef struct KUPDATE_PACKED kupdate_v2_data {
    uint32_t offset;
    uint8_t bytes[KUPDATE_V2_DATA_BYTES];
} kupdate_v2_data_t;

typedef struct KUPDATE_PACKED kupdate_v2_status {
    uint32_t magic;
    uint8_t state;
    uint8_t error;
    uint8_t target_slot;
    uint8_t reserved0;
    uint32_t offset;
    uint32_t image_size;
    uint32_t detail;
    uint32_t status_crc32;
} kupdate_v2_status_t;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(__cplusplus)
static_assert(sizeof(kupdate_v2_open_t) == KUPDATE_V2_OPEN_BYTES, "KUPDATE open size");
static_assert(sizeof(kupdate_v2_data_t) == 48u, "KUPDATE data size");
extern "C" {
#else
_Static_assert(sizeof(kupdate_v2_open_t) == KUPDATE_V2_OPEN_BYTES, "KUPDATE open size");
_Static_assert(sizeof(kupdate_v2_data_t) == 48u, "KUPDATE data size");
#endif

uint32_t kupdate_v2_crc32(const void *data, size_t size);
void kupdate_v2_open_finalize(kupdate_v2_open_t *open);
int kupdate_v2_open_valid(const kupdate_v2_open_t *open, uint32_t max_image_size);
void kupdate_v2_status_finalize(kupdate_v2_status_t *status);
int kupdate_v2_status_valid(const kupdate_v2_status_t *status);

#if defined(__cplusplus)
}
#endif

#undef KUPDATE_PACKED
