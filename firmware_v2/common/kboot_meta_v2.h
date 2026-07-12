#pragma once
#include <stdint.h>

#define KBOOT_META_MAGIC          0x324d424bu
#define KBOOT_META_VERSION        2u
#define KBOOT_META_COMMIT         0x54494d43u
#define KBOOT_META_RECORD_BYTES   128u
#define KBOOT_META_JOURNAL0       0x00fe0000u
#define KBOOT_META_JOURNAL1       0x00ff0000u
#define KBOOT_META_JOURNAL_BYTES  0x00010000u
#define KBOOT_SLOT_A_OFFSET       0x00100000u
#define KBOOT_SLOT_B_OFFSET       0x00600000u
#define KBOOT_SLOT_BYTES          0x00500000u
#define KBOOT_SLOT_NONE           0xffu

typedef struct __attribute__((packed)) kboot_meta_v2 {
    uint32_t magic;
    uint16_t version;
    uint16_t record_bytes;
    uint32_t generation;
    uint8_t active_slot;
    uint8_t confirmed_slot;
    uint8_t pending_slot;
    uint8_t boot_attempts;
    uint32_t image_size[2];
    uint8_t image_sha256[2][32];
    uint32_t flags;
    uint8_t reserved[28];
    uint32_t crc32;
    uint32_t commit;
} kboot_meta_v2_t;

_Static_assert(sizeof(kboot_meta_v2_t)==KBOOT_META_RECORD_BYTES,"metadata size");

void kboot_meta_v2_default(kboot_meta_v2_t *meta);
int kboot_meta_v2_load(kboot_meta_v2_t *meta);
int kboot_meta_v2_append(kboot_meta_v2_t *meta);
int kboot_meta_v2_confirm_running(uint8_t slot);
