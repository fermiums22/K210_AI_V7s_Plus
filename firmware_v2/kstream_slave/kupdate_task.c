#include "kupdate_task.h"

#include <FreeRTOS.h>
#include <task.h>
#include <string.h>
#include <sysctl.h>
#include <uarths.h>

#include "k210_flash.h"
#include "kboot_meta_v2.h"
#include "kstream_slave.h"
#include "kupdate_v2.h"
#include "sha256_stream.h"

#define APP_MAGIC     0x4b323130u
#define APP_MAGIC_INV 0xb4cdcedfu

typedef struct __attribute__((packed)) app_header {
    uint32_t magic;
    uint32_t magic_inv;
    uint32_t load_addr;
    uint32_t entry_addr;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t flags;
    uint32_t reserved;
} app_header_t;

static uint8_t s_page[4096] __attribute__((aligned(64)));
static uint8_t s_verify[4096] __attribute__((aligned(64)));

static void read_exact(void *data, size_t length)
{
    uint8_t *destination = (uint8_t *)data;
    while (length != 0u) {
        size_t count = kstream_update_read(destination, length);
        destination += count;
        length -= count;
    }
}

static void send_status(uint8_t state, uint8_t error, uint8_t slot,
                        uint32_t offset, uint32_t image_size, uint32_t detail)
{
    kupdate_v2_status_t status;
    memset(&status, 0, sizeof(status));
    status.state = state;
    status.error = error;
    status.target_slot = slot;
    status.offset = offset;
    status.image_size = image_size;
    status.detail = detail;
    kupdate_v2_status_finalize(&status);
    while (kstream_update_write(&status, sizeof(status)) != sizeof(status))
        vTaskDelay(1u);
}

static void update_task(void *arg)
{
    (void)arg;
    for (;;) {
        kupdate_v2_open_t open;
        read_exact(&open, sizeof(open));
        uarths_puts("KUPDATE:OPEN\r\n");
        if (!kupdate_v2_open_valid(&open, KBOOT_SLOT_BYTES)) {
            uarths_puts("KUPDATE:BAD_OPEN\r\n");
            send_status(KUPDATE_V2_STATE_FAILED, KUPDATE_V2_ERR_PROTOCOL,
                        KBOOT_SLOT_NONE, 0u, 0u, 0u);
            continue;
        }

        kboot_meta_v2_t meta;
        uarths_puts("KUPDATE:META_SCAN\r\n");
        int rc = kboot_meta_v2_load(&meta);
        uarths_puts("KUPDATE:META_DONE\r\n");
        if (rc < 0) {
            uarths_puts("KUPDATE:META_DEFAULT\r\n");
            kboot_meta_v2_default(&meta);
        }
        uint8_t slot = meta.pending_slot != KBOOT_SLOT_NONE
                           ? meta.pending_slot
                           : (uint8_t)(meta.confirmed_slot ^ 1u);
        uint32_t base = slot ? KBOOT_SLOT_B_OFFSET : KBOOT_SLOT_A_OFFSET;
        uint32_t erase = (open.image_size + 0xfffu) & ~0xfffu;
        uarths_puts("KUPDATE:ERASE\r\n");
        for (uint32_t offset = 0u; offset < erase; offset += 0x1000u) {
            rc = k210_flash_erase_4k(base + offset);
            if (rc != 0)
                break;
        }
        if (rc != 0) {
            send_status(KUPDATE_V2_STATE_FAILED, KUPDATE_V2_ERR_FLASH_ERASE,
                        slot, 0u, open.image_size, (uint32_t)-rc);
            continue;
        }
        send_status(KUPDATE_V2_STATE_READY, KUPDATE_V2_OK, slot, 0u,
                    open.image_size, 0u);
        uarths_puts("KUPDATE:READY\r\n");

        sha256_stream_t hash;
        sha256_stream_init(&hash);
        uint32_t received = 0u;
        while (received < open.image_size) {
            uint32_t count = open.image_size - received;
            if (count > sizeof(s_page))
                count = sizeof(s_page);
            read_exact(s_page, count);
            sha256_stream_update(&hash, s_page, count);
            rc = k210_flash_program(base + received, s_page, count);
            if (rc != 0)
                break;
            received += count;
        }
        if (rc != 0) {
            send_status(KUPDATE_V2_STATE_FAILED, KUPDATE_V2_ERR_FLASH_WRITE,
                        slot, received, open.image_size, (uint32_t)-rc);
            continue;
        }
        uarths_puts("KUPDATE:PROGRAMMED\r\n");

        uint8_t digest[32];
        sha256_stream_final(&hash, digest);
        if (memcmp(digest, open.image_sha256, sizeof(digest)) != 0) {
            send_status(KUPDATE_V2_STATE_FAILED, KUPDATE_V2_ERR_HASH, slot,
                        received, open.image_size, 1u);
            continue;
        }

        sha256_stream_init(&hash);
        for (uint32_t offset = 0u; offset < open.image_size;) {
            uint32_t count = open.image_size - offset;
            if (count > sizeof(s_verify))
                count = sizeof(s_verify);
            rc = k210_flash_read(base + offset, s_verify, count);
            if (rc != 0)
                break;
            sha256_stream_update(&hash, s_verify, count);
            offset += count;
        }
        sha256_stream_final(&hash, digest);
        app_header_t header;
        if (rc == 0)
            rc = k210_flash_read(base, &header, sizeof(header));
        if (rc != 0 || memcmp(digest, open.image_sha256, sizeof(digest)) != 0) {
            send_status(KUPDATE_V2_STATE_FAILED, KUPDATE_V2_ERR_FLASH_READ,
                        slot, received, open.image_size,
                        rc != 0 ? (uint32_t)-rc : 2u);
            continue;
        }
        uarths_puts("KUPDATE:VERIFIED\r\n");
        if (header.magic != APP_MAGIC || header.magic_inv != APP_MAGIC_INV ||
            header.load_addr != 0x80100000u ||
            header.image_size != open.image_size ||
            header.entry_addr < 0x80100000u || header.entry_addr >= 0x80600000u) {
            send_status(KUPDATE_V2_STATE_FAILED, KUPDATE_V2_ERR_PROTOCOL, slot,
                        received, open.image_size, 0x484452u);
            continue;
        }

        meta.pending_slot = slot;
        meta.boot_attempts = 0u;
        meta.image_size[slot] = open.image_size;
        memcpy(meta.image_sha256[slot], open.image_sha256, 32u);
        rc = kboot_meta_v2_append(&meta);
        if (rc != 0) {
            send_status(KUPDATE_V2_STATE_FAILED, KUPDATE_V2_ERR_METADATA, slot,
                        received, open.image_size, (uint32_t)-rc);
            continue;
        }
        send_status(KUPDATE_V2_STATE_COMMITTED, KUPDATE_V2_OK, slot, received,
                    open.image_size, meta.generation);
        uarths_puts("KUPDATE:COMMITTED\r\n");
    }
}

bool kupdate_task_start(void)
{
    kboot_meta_v2_t meta;
    if (kboot_meta_v2_load(&meta) == 0 && meta.pending_slot == meta.active_slot)
        (void)kboot_meta_v2_confirm_running(meta.active_slot);
    return xTaskCreate(update_task, "kupdate", 3072u, NULL, 3u, NULL) == pdPASS;
}
