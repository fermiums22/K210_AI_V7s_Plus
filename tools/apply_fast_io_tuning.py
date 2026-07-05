#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_one(text: str, pattern: str, repl: str, label: str) -> str:
    new, n = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
    if n != 1:
        raise SystemExit(f"patch failed: {label}")
    return new


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

    patch_file(
        ROOT / "src" / "sd_uart.c",
        [
            (r"^#define\s+UART_SD_BUF\s+\d+u?\s*$", f"#define UART_SD_BUF   {args.ksd_buf}u", "UART_SD_BUF"),
            (r'xTaskCreate\(sd_uart_task, "ksd", \d+, NULL, tskIDLE_PRIORITY \+ 2, NULL\);',
             f'xTaskCreate(sd_uart_task, "ksd", {args.ksd_stack}, NULL, tskIDLE_PRIORITY + 2, NULL);',
             "ksd task stack"),
        ],
    )
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
        f"esp_baud={args.esp_baud} esp_block={args.esp_block}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
