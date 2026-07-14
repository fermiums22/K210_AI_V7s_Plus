#include "kupdate_task.h"

#include <FreeRTOS.h>
#include <string.h>
#include <task.h>
#include <uarths.h>

#include "k210_flash.h"
#include "kboot_meta_v2.h"
#include "kstream_slave.h"
#include "kstream_v2.h"
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

typedef enum update_state {
    UPDATE_WAIT_OPEN,
    UPDATE_RECEIVING,
    UPDATE_FAILED,
    UPDATE_COMMITTED,
} update_state_t;

static uint8_t s_page[4096] __attribute__((aligned(64)));
static uint8_t s_verify[4096] __attribute__((aligned(64)));
static uint8_t s_record_data[KSTREAM_V2_UPDATE_DATA_BYTES]
    __attribute__((aligned(4)));
static kupdate_v2_open_t s_open;
static sha256_stream_t s_hash;
static update_state_t s_state;
static uint32_t s_session;
static uint32_t s_base;
static uint32_t s_received;
static uint32_t s_programmed;
static uint32_t s_buffered;
static uint32_t s_open_bytes;
static uint8_t s_slot;

static void read_exact(void *data, size_t length)
{
    uint8_t *destination = (uint8_t *)data;
    while (length != 0u) {
        size_t count = kstream_update_read(destination, length);
        destination += count;
        length -= count;
    }
}

static void write_exact(const void *data, size_t length)
{
    if (kstream_update_write(data, length) != length) {
        uarths_puts("KUPDATE:FATAL status transport\r\n");
        vTaskDelete(NULL);
    }
}

static void send_status(uint8_t state, uint8_t error, uint32_t detail)
{
    kupdate_v2_status_t status;
    memset(&status, 0, sizeof(status));
    status.state = state;
    status.error = error;
    status.target_slot = s_slot;
    status.offset = s_received;
    status.image_size = s_open.image_size;
    status.detail = detail;
    kupdate_v2_status_finalize(&status);

    kstream_v2_update_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = KSTREAM_V2_UPDATE_RECORD_MAGIC;
    record.session = s_session;
    record.length = sizeof(status);
    record.type = KSTREAM_V2_UPDATE_STATUS;
    record.crc32 = kstream_v2_crc32(&record, offsetof(
        kstream_v2_update_record_t, crc32));
    write_exact(&record, sizeof(record));
    write_exact(&status, sizeof(status));
}

static void reset_session(uint32_t session)
{
    memset(&s_open, 0, sizeof(s_open));
    s_state = UPDATE_WAIT_OPEN;
    s_session = session;
    s_base = 0u;
    s_received = 0u;
    s_programmed = 0u;
    s_buffered = 0u;
    s_open_bytes = 0u;
    s_slot = KBOOT_SLOT_NONE;
}

static void fail_update(uint8_t error, uint32_t detail)
{
    if (s_state == UPDATE_FAILED)
        return;
    s_state = UPDATE_FAILED;
    send_status(KUPDATE_V2_STATE_FAILED, error, detail);
    uarths_puts("KUPDATE:FAILED\r\n");
}

static int flush_page(void)
{
    if (s_buffered == 0u)
        return 0;
    int rc = k210_flash_program(s_base + s_programmed, s_page, s_buffered);
    if (rc == 0) {
        s_programmed += s_buffered;
        s_buffered = 0u;
    }
    return rc;
}

static void finish_update(void)
{
    int rc = flush_page();
    if (rc != 0) {
        fail_update(KUPDATE_V2_ERR_FLASH_WRITE, (uint32_t)-rc);
        return;
    }

    uint8_t digest[32];
    sha256_stream_final(&s_hash, digest);
    if (memcmp(digest, s_open.image_sha256, sizeof(digest)) != 0) {
        fail_update(KUPDATE_V2_ERR_HASH, 1u);
        return;
    }

    sha256_stream_t verify;
    sha256_stream_init(&verify);
    for (uint32_t offset = 0u; offset < s_open.image_size;) {
        uint32_t count = s_open.image_size - offset;
        if (count > sizeof(s_verify))
            count = sizeof(s_verify);
        rc = k210_flash_read(s_base + offset, s_verify, count);
        if (rc != 0) {
            fail_update(KUPDATE_V2_ERR_FLASH_READ, (uint32_t)-rc);
            return;
        }
        sha256_stream_update(&verify, s_verify, count);
        offset += count;
    }
    sha256_stream_final(&verify, digest);
    if (memcmp(digest, s_open.image_sha256, sizeof(digest)) != 0) {
        fail_update(KUPDATE_V2_ERR_HASH, 2u);
        return;
    }

    app_header_t header;
    rc = k210_flash_read(s_base, &header, sizeof(header));
    if (rc != 0) {
        fail_update(KUPDATE_V2_ERR_FLASH_READ, (uint32_t)-rc);
        return;
    }
    if (header.magic != APP_MAGIC || header.magic_inv != APP_MAGIC_INV ||
        header.load_addr != 0x80100000u ||
        header.image_size != s_open.image_size ||
        header.entry_addr < 0x80100000u || header.entry_addr >= 0x80600000u) {
        fail_update(KUPDATE_V2_ERR_PROTOCOL, 0x484452u);
        return;
    }

    kboot_meta_v2_t meta;
    rc = kboot_meta_v2_load(&meta);
    if (rc < 0) {
        fail_update(KUPDATE_V2_ERR_METADATA, (uint32_t)-rc);
        return;
    }
    if (rc > 0)
        kboot_meta_v2_default(&meta);
    meta.pending_slot = s_slot;
    meta.boot_attempts = 0u;
    meta.image_size[s_slot] = s_open.image_size;
    memcpy(meta.image_sha256[s_slot], s_open.image_sha256, 32u);
    rc = kboot_meta_v2_append(&meta);
    if (rc != 0) {
        fail_update(KUPDATE_V2_ERR_METADATA, (uint32_t)-rc);
        return;
    }
    s_state = UPDATE_COMMITTED;
    send_status(KUPDATE_V2_STATE_COMMITTED, KUPDATE_V2_OK, meta.generation);
    uarths_puts("KUPDATE:COMMITTED\r\n");
}

static void accept_image_data(const uint8_t *data, size_t length)
{
    if (s_state != UPDATE_RECEIVING) {
        fail_update(KUPDATE_V2_ERR_STATE, 2u);
        return;
    }
    if (length > s_open.image_size - s_received) {
        fail_update(KUPDATE_V2_ERR_SIZE, (uint32_t)length);
        return;
    }

    sha256_stream_update(&s_hash, data, length);
    s_received += (uint32_t)length;
    while (length != 0u) {
        size_t count = sizeof(s_page) - s_buffered;
        if (count > length)
            count = length;
        memcpy(s_page + s_buffered, data, count);
        s_buffered += (uint32_t)count;
        data += count;
        length -= count;
        if (s_buffered == sizeof(s_page)) {
            int rc = flush_page();
            if (rc != 0) {
                fail_update(KUPDATE_V2_ERR_FLASH_WRITE, (uint32_t)-rc);
                return;
            }
        }
    }
    if (s_received == s_open.image_size)
        finish_update();
}

static void begin_update(void)
{
    if (!kupdate_v2_open_valid(&s_open, KBOOT_SLOT_BYTES)) {
        fail_update(KUPDATE_V2_ERR_PROTOCOL, 0u);
        return;
    }

    kboot_meta_v2_t meta;
    int rc = kboot_meta_v2_load(&meta);
    if (rc < 0) {
        fail_update(KUPDATE_V2_ERR_METADATA, (uint32_t)-rc);
        return;
    }
    if (rc > 0)
        kboot_meta_v2_default(&meta);
    if (meta.pending_slot != KBOOT_SLOT_NONE) {
        fail_update(KUPDATE_V2_ERR_STATE, 0x50454e44u);
        return;
    }

    s_slot = (uint8_t)(meta.confirmed_slot ^ 1u);
    s_base = s_slot ? KBOOT_SLOT_B_OFFSET : KBOOT_SLOT_A_OFFSET;
    uint32_t erase = (s_open.image_size + 0xfffu) & ~0xfffu;
    for (uint32_t offset = 0u; offset < erase; offset += 0x1000u) {
        rc = k210_flash_erase_4k(s_base + offset);
        if (rc != 0) {
            fail_update(KUPDATE_V2_ERR_FLASH_ERASE, (uint32_t)-rc);
            return;
        }
    }
    sha256_stream_init(&s_hash);
    s_state = UPDATE_RECEIVING;
    send_status(KUPDATE_V2_STATE_READY, KUPDATE_V2_OK, 0u);
    uarths_puts("KUPDATE:READY\r\n");
}

static void accept_record_data(const uint8_t *data, size_t length)
{
    if (s_state == UPDATE_FAILED || s_state == UPDATE_COMMITTED)
        return;
    if (s_state == UPDATE_WAIT_OPEN) {
        size_t need = sizeof(s_open) - s_open_bytes;
        size_t count = length < need ? length : need;
        memcpy((uint8_t *)&s_open + s_open_bytes, data, count);
        s_open_bytes += (uint32_t)count;
        data += count;
        length -= count;
        if (s_open_bytes == sizeof(s_open))
            begin_update();
    }
    if (length != 0u)
        accept_image_data(data, length);
}

static bool record_valid(const kstream_v2_update_record_t *record)
{
    return record->magic == KSTREAM_V2_UPDATE_RECORD_MAGIC &&
           record->reserved == 0u &&
           record->length <= KSTREAM_V2_UPDATE_DATA_BYTES &&
           record->crc32 == kstream_v2_crc32(
               record, offsetof(kstream_v2_update_record_t, crc32));
}

static void update_task(void *arg)
{
    (void)arg;
    reset_session(0u);
    kstream_link_wait();

    kboot_meta_v2_t meta;
    int rc = kboot_meta_v2_load(&meta);
    if (rc == 0 && meta.pending_slot == meta.active_slot) {
        rc = kboot_meta_v2_confirm_running(meta.active_slot);
        if (rc < 0) {
            uarths_puts("KUPDATE:FATAL confirm failed\r\n");
            vTaskDelete(NULL);
        }
        uarths_puts("KUPDATE:RUNNING_SLOT_CONFIRMED\r\n");
    } else if (rc < 0) {
        uarths_puts("KUPDATE:FATAL metadata load\r\n");
        vTaskDelete(NULL);
    }

    for (;;) {
        kstream_v2_update_record_t record;
        read_exact(&record, sizeof(record));
        if (!record_valid(&record)) {
            uarths_puts("KUPDATE:FATAL bad record\r\n");
            vTaskDelete(NULL);
        }
        if (record.length != 0u)
            read_exact(s_record_data, record.length);

        if (record.type == KSTREAM_V2_UPDATE_BEGIN && record.length == 0u &&
            record.session != 0u) {
            reset_session(record.session);
            uarths_puts("KUPDATE:SESSION_BEGIN\r\n");
        } else if (record.type == KSTREAM_V2_UPDATE_ABORT &&
                   record.length == 0u && record.session == s_session) {
            reset_session(0u);
            uarths_puts("KUPDATE:SESSION_ABORT\r\n");
        } else if (record.type == KSTREAM_V2_UPDATE_DATA &&
                   record.session == s_session && s_session != 0u) {
            accept_record_data(s_record_data, record.length);
        } else {
            fail_update(KUPDATE_V2_ERR_PROTOCOL, record.type);
        }
    }
}

bool kupdate_task_start(void)
{
    return xTaskCreate(update_task, "kupdate", 3072u, NULL, 3u, NULL) == pdPASS;
}
