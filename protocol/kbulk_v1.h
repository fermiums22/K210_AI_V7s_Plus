#pragma once

#include <stdint.h>

#define KBULK_V1_MAGIC         0x314b4c42u
#define KBULK_V1_FAULT_MAGIC   0x214b4c42u
#define KBULK_V1_BLOCK_BYTES   4096u

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define KBULK_PACKED
#else
#define KBULK_PACKED __attribute__((packed))
#endif

typedef struct KBULK_PACKED kbulk_v1_header {
    uint32_t magic;
    uint32_t sequence;
    uint32_t payload_length;
    uint32_t payload_crc32;
} kbulk_v1_header_t;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#define KBULK_V1_PAYLOAD_BYTES \
    (KBULK_V1_BLOCK_BYTES - (uint32_t)sizeof(kbulk_v1_header_t))

#undef KBULK_PACKED
