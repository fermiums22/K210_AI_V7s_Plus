#include "app_flash.h"
#include "log.h"

#include <FreeRTOS.h>
#include <task.h>
#include <filesystem.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APP_FLASH_PAGE_SIZE       256u
#define APP_FLASH_SECTOR_SIZE     4096u
#define APP_FLASH_MAX_SLOT0_SIZE  0x00f00000u
#define APP_FLASH_PROGRESS_STEP   65536u

int app_spi3_sector_4k(uint32_t flash_offset);
int app_spi3_program_page(uint32_t flash_offset, const void *src, uint32_t len);
int app_spi3_read_raw(uint32_t flash_offset, void *dst, uint32_t len);

static uint8_t s_wr[APP_FLASH_PAGE_SIZE] __attribute__((aligned(64)));
static uint8_t s_rd[APP_FLASH_PAGE_SIZE] __attribute__((aligned(64)));

static bool make_fs_path(const char *rel_path, char *path, size_t path_size)
{
    if (!rel_path || !rel_path[0] || rel_path[0] == '/' || rel_path[0] == '\\' ||
        strstr(rel_path, ".."))
        return false;
    snprintf(path, path_size, "/fs/0/%s", rel_path);
    return true;
}

static bool erase_slot(uint32_t image_size)
{
    uint32_t erase_size = (image_size + APP_FLASH_SECTOR_SIZE - 1u) &
                          ~(APP_FLASH_SECTOR_SIZE - 1u);

    LOGF("[app-flash] erase slot0 off=0x%08lx size=%lu",
         (unsigned long)APP_FLASH_SLOT0_OFFSET, (unsigned long)erase_size);
    for (uint32_t off = 0; off < erase_size; off += APP_FLASH_SECTOR_SIZE) {
        int rc = app_spi3_sector_4k(APP_FLASH_SLOT0_OFFSET + off);
        if (rc != 0) {
            LOGF("[app-flash] erase failed off=0x%08lx rc=%d",
                 (unsigned long)(APP_FLASH_SLOT0_OFFSET + off), rc);
            return false;
        }
        if ((off % APP_FLASH_PROGRESS_STEP) == 0)
            LOGF("[app-flash] erase %lu/%lu", (unsigned long)off, (unsigned long)erase_size);
        taskYIELD();
    }
    return true;
}

static bool program_file(handle_t f, uint32_t image_size)
{
    uint32_t done = 0;
    LOGF("[app-flash] program slot0 size=%lu", (unsigned long)image_size);
    while (done < image_size) {
        uint32_t chunk = image_size - done;
        if (chunk > sizeof(s_wr))
            chunk = sizeof(s_wr);
        memset(s_wr, 0xff, sizeof(s_wr));
        int got = filesystem_file_read(f, s_wr, chunk);
        if (got != (int)chunk) {
            LOGF("[app-flash] read failed at %lu got=%d want=%lu",
                 (unsigned long)done, got, (unsigned long)chunk);
            return false;
        }
        int rc = app_spi3_program_page(APP_FLASH_SLOT0_OFFSET + done, s_wr, chunk);
        if (rc != 0) {
            LOGF("[app-flash] program failed off=0x%08lx rc=%d",
                 (unsigned long)(APP_FLASH_SLOT0_OFFSET + done), rc);
            return false;
        }
        done += chunk;
        if ((done % APP_FLASH_PROGRESS_STEP) == 0 || done == image_size)
            LOGF("[app-flash] program %lu/%lu", (unsigned long)done, (unsigned long)image_size);
        taskYIELD();
    }
    return true;
}

static bool verify_file(handle_t f, uint32_t image_size)
{
    uint32_t done = 0;
    LOGF("[app-flash] verify slot0 size=%lu", (unsigned long)image_size);
    while (done < image_size) {
        uint32_t chunk = image_size - done;
        if (chunk > sizeof(s_wr))
            chunk = sizeof(s_wr);
        int got = filesystem_file_read(f, s_wr, chunk);
        if (got != (int)chunk) {
            LOGF("[app-flash] verify file read failed at %lu", (unsigned long)done);
            return false;
        }
        int rc = app_spi3_read_raw(APP_FLASH_SLOT0_OFFSET + done, s_rd, chunk);
        if (rc != 0 || memcmp(s_wr, s_rd, chunk) != 0) {
            LOGF("[app-flash] verify failed off=0x%08lx rc=%d",
                 (unsigned long)(APP_FLASH_SLOT0_OFFSET + done), rc);
            return false;
        }
        done += chunk;
        if ((done % APP_FLASH_PROGRESS_STEP) == 0 || done == image_size)
            LOGF("[app-flash] verify %lu/%lu", (unsigned long)done, (unsigned long)image_size);
        taskYIELD();
    }
    return true;
}

bool app_flash_slot0_from_sd(const char *rel_path, bool verify)
{
    char path[160];
    if (!make_fs_path(rel_path, path, sizeof(path))) {
        LOGF("[app-flash] bad path: %s", rel_path ? rel_path : "<null>");
        return false;
    }

    handle_t f = filesystem_file_open(path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
    if (!f) {
        LOGF("[app-flash] missing file: %s", path);
        return false;
    }

    uint64_t size64 = filesystem_file_get_size(f);
    if (size64 == 0 || size64 > APP_FLASH_MAX_SLOT0_SIZE) {
        LOGF("[app-flash] bad size: %lu", (unsigned long)size64);
        filesystem_file_close(f);
        return false;
    }

    uint32_t image_size = (uint32_t)size64;
    bool ok = erase_slot(image_size) && program_file(f, image_size);
    filesystem_file_close(f);

    if (ok && verify) {
        f = filesystem_file_open(path, FILE_ACCESS_READ, FILE_MODE_OPEN_EXISTING);
        if (!f)
            return false;
        ok = verify_file(f, image_size);
        filesystem_file_close(f);
    }

    LOGF("[app-flash] slot0 %s %s %lu bytes", ok ? "OK" : "FAIL",
         rel_path, (unsigned long)image_size);
    return ok;
}
