#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


RECEIVE_FILE_FATFS = r'''static bool receive_file(const char *rel_path, uint32_t size)
{
    char fs_path[160];
    char fat_path[160];
    if (!make_fs_path(rel_path, fs_path, sizeof(fs_path), fat_path, sizeof(fat_path))) {
        LOGF("[sd-uart] bad path: %s", rel_path);
        diag_printf(5, "UART bad path: %.24s", rel_path);
        host_puts("KSD:ERR bad-path\n");
        return false;
    }
    if (!sd_mount()) {
        LOG("[sd-uart] PUT SD mount failed");
        diag_line(5, "PUT SD mount fail");
        host_puts("KSD:ERR sd\n");
        return false;
    }
    make_parent_dirs(rel_path);
    f_unlink(fat_path);

    FIL f;
    FRESULT fr = f_open(&f, fat_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        LOGF("[sd-uart] open failed: %s rc=%d", fat_path, (int)fr);
        diag_printf(5, "UART open failed");
        host_puts("KSD:ERR open\n");
        return false;
    }

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "KSD:GO %lu\n", (unsigned long)sizeof(rx_buf));
    host_puts(hdr);
    host_puts("KSD:READYDATA\n");

    uint32_t got = 0;
    while (got < size) {
        uint32_t chunk = size - got;
        if (chunk > sizeof(rx_buf))
            chunk = sizeof(rx_buf);
        for (uint32_t i = 0; i < chunk; i++) {
            if (!read_byte_timeout(&rx_buf[i], UART_SD_DATA_TIMEOUT_MS)) {
                f_close(&f);
                LOGF("[sd-uart] short file: %s %lu+%lu/%lu", rel_path,
                     (unsigned long)got, (unsigned long)i, (unsigned long)size);
                diag_printf(5, "UART short %lu/%lu", (unsigned long)(got + i), (unsigned long)size);
                host_puts("KSD:ERR short\n");
                return false;
            }
        }
        UINT wr = 0;
        fr = f_write(&f, rx_buf, chunk, &wr);
        if (fr != FR_OK || wr != chunk) {
            f_close(&f);
            LOGF("[sd-uart] write failed: %s rc=%d wr=%u/%lu", rel_path,
                 (int)fr, (unsigned)wr, (unsigned long)chunk);
            diag_printf(5, "SD write failed");
            host_puts("KSD:ERR write\n");
            return false;
        }
        got += chunk;
        if ((got % (32u * 1024u)) == 0 || got == size)
            diag_printf(5, "UART %lu/%lu", (unsigned long)got, (unsigned long)size);
        host_puts("KSD:B\n");
    }

    fr = f_close(&f);
    if (fr != FR_OK) {
        LOGF("[sd-uart] close failed: %s rc=%d", rel_path, (int)fr);
        host_puts("KSD:ERR close\n");
        return false;
    }
    host_puts("KSD:OK\n");
    LOGF("[sd-uart] received %s %lu", rel_path, (unsigned long)size);
    return true;
}'''


SEND_FILE_RAW_FATFS = r'''static bool send_file_raw(const char *rel_path)
{
    char fs_path[160];
    char fat_path[160];
    if (!make_fs_path(rel_path, fs_path, sizeof(fs_path), fat_path, sizeof(fat_path))) {
        LOGF("[sd-uart] bad get path: %s", rel_path);
        host_puts("KSD:ERR bad-path\n");
        return false;
    }

    FIL f;
    FRESULT fr = f_open(&f, fat_path, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        LOGF("[sd-uart] get missing: %s rc=%d", fat_path, (int)fr);
        host_puts("KSD:MISSING\n");
        return true;
    }
    if (fr != FR_OK) {
        LOGF("[sd-uart] get open failed: %s rc=%d", fat_path, (int)fr);
        host_puts("KSD:ERR open\n");
        return false;
    }

    FSIZE_t size64 = f_size(&f);
    if (size64 > 0xffffffffu) {
        f_close(&f);
        host_puts("KSD:ERR too-large\n");
        return false;
    }
    uint32_t size = (uint32_t)size64;
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "KSD:SIZE %lu\n", (unsigned long)size);
    host_puts(hdr);

    uint32_t sent = 0;
    while (sent < size) {
        uint32_t chunk = size - sent;
        if (chunk > sizeof(rx_buf))
            chunk = sizeof(rx_buf);
        UINT br = 0;
        fr = f_read(&f, rx_buf, chunk, &br);
        if (fr != FR_OK || br == 0) {
            f_close(&f);
            LOGF("[sd-uart] read failed: %s rc=%d br=%u", rel_path, (int)fr, (unsigned)br);
            host_puts("KSD:ERR read\n");
            return false;
        }
        host_write(rx_buf, (uint32_t)br);
        sent += (uint32_t)br;
    }
    f_close(&f);
    host_puts("KSD:OK\n");
    LOGF("[sd-uart] sent %s %lu", rel_path, (unsigned long)size);
    return true;
}'''


def replace_one(text: str, pattern: str, repl: str, label: str) -> str:
    new, n = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
    if n != 1:
        raise SystemExit(f"patch failed: {label}")
    return new


def find_function_span(text: str, signature: str) -> tuple[int, int]:
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"patch failed: function not found: {signature}")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"patch failed: function brace not found: {signature}")
    depth = 0
    for pos in range(brace, len(text)):
        c = text[pos]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return start, pos + 1
    raise SystemExit(f"patch failed: function end not found: {signature}")


def replace_function(text: str, signature: str, replacement: str) -> str:
    start, end = find_function_span(text, signature)
    return text[:start] + replacement + text[end:]


def patch_file(path: Path, edits: list[tuple[str, str, str]]) -> bool:
    old = path.read_text(encoding="utf-8")
    new = old
    for pattern, repl, label in edits:
        new = replace_one(new, pattern, repl, label)
    if new == old:
        print(f"unchanged: {path.relative_to(ROOT)}")
        return False
    path.write_text(new, encoding="utf-8", newline="\n")
    print(f"patched:   {path.relative_to(ROOT)}")
    return True


def patch_sd_uart(path: Path, ksd_buf: int, ksd_stack: int) -> None:
    old = path.read_text(encoding="utf-8")
    new = old
    new = replace_one(new, r"^#define\s+UART_SD_BUF\s+\d+u?\s*$", f"#define UART_SD_BUF   {ksd_buf}u", "UART_SD_BUF")
    new = replace_one(
        new,
        r'xTaskCreate\(sd_uart_task, "ksd", \d+, NULL, tskIDLE_PRIORITY \+ 2, NULL\);',
        f'xTaskCreate(sd_uart_task, "ksd", {ksd_stack}, NULL, tskIDLE_PRIORITY + 2, NULL);',
        "ksd task stack",
    )
    new = replace_function(new, "static bool receive_file(const char *rel_path, uint32_t size)", RECEIVE_FILE_FATFS)
    new = replace_function(new, "static bool send_file_raw(const char *rel_path)", SEND_FILE_RAW_FATFS)
    if new == old:
        print(f"unchanged: {path.relative_to(ROOT)}")
    else:
        path.write_text(new, encoding="utf-8", newline="\n")
        print(f"patched:   {path.relative_to(ROOT)}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Apply repeatable local K210 fast IO tuning after git reset and before build.")
    ap.add_argument("--ksd-buf", type=int, default=4096, help="KSD PUT/GET buffer and negotiated chunk size")
    ap.add_argument("--ksd-stack", type=int, default=12288, help="KSD FreeRTOS task stack after buffer increase")
    ap.add_argument("--esp-baud", type=int, default=230400, help="ESP serial flashing baud")
    ap.add_argument("--esp-block", type=int, default=4096, help="ESP serial flashing block size")
    args = ap.parse_args()

    if args.ksd_buf < 512 or args.ksd_buf > 16384 or (args.ksd_buf % 512) != 0:
        raise SystemExit("--ksd-buf must be 512..16384 and divisible by 512")
    if args.ksd_stack < 6144:
        raise SystemExit("--ksd-stack must be >= 6144")
    if args.esp_baud not in (115200, 230400, 460800, 921600):
        raise SystemExit("--esp-baud must be one of 115200, 230400, 460800, 921600")
    if args.esp_block < 1024 or args.esp_block > 8192 or (args.esp_block % 1024) != 0:
        raise SystemExit("--esp-block must be 1024..8192 and divisible by 1024")

    patch_sd_uart(ROOT / "src" / "sd_uart.c", args.ksd_buf, args.ksd_stack)
    patch_file(
        ROOT / "src" / "esp_flasher.c",
        [
            (r"^#define\s+ESP_FLASH_BAUD\s+\d+u?\s*$", f"#define ESP_FLASH_BAUD       {args.esp_baud}u", "ESP_FLASH_BAUD"),
            (r"^#define\s+ESP_FLASH_BLOCK\s+\d+u?\s*$", f"#define ESP_FLASH_BLOCK      {args.esp_block}u", "ESP_FLASH_BLOCK"),
        ],
    )
    print(
        "FAST_IO_TUNING_OK "
        f"ksd_buf={args.ksd_buf} ksd_stack={args.ksd_stack} "
        f"esp_baud={args.esp_baud} esp_block={args.esp_block} "
        "ksd_io=fatfs"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
