#!/usr/bin/env python3
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDCARD = ROOT / "lib" / "drivers" / "src" / "storage" / "sdcard.cpp"


ON_FIRST_OPEN = r'''    virtual void on_first_open() override
    {
        auto spi = make_accessor(spi_driver_);
        spi8_dev_ = make_accessor(spi->get_device(SPI_MODE_0, SPI_FF_STANDARD, 1, 8));

        cs_gpio_ = make_accessor(cs_gpio_driver_);
        cs_gpio_->set_drive_mode(cs_gpio_pin_, GPIO_DM_OUTPUT);
        cs_gpio_->set_pin_value(cs_gpio_pin_, GPIO_PV_HIGH);

        memset(&card_info_, 0, sizeof(card_info_));
        spi8_dev_->set_clock_rate(SD_SPI_LOW_CLOCK_RATE);
        int init_rc = sd_init();
        init_ok_ = init_rc == 0;
        printf("[sdcard] init rc=%d capacity=%llu block=%lu\n",
               init_rc,
               (unsigned long long)card_info_.CardCapacity,
               (unsigned long)card_info_.CardBlockSize);
        if (!init_ok_)
            throw init_rc;
    }'''


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


def replace_count(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"patch failed: {label}: expected 1 match, got {count}")
    return text.replace(old, new, 1)


def replace_regex_once(text: str, pattern: str, repl: str, label: str) -> str:
    new, count = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise SystemExit(f"patch failed: {label}: expected 1 match, got {count}")
    return new


def main() -> int:
    old = SDCARD.read_text(encoding="utf-8")
    new = old

    # The SDK driver must not keep a live block-storage handle after sd_init()
    # returned 0xff. Replace the whole method to avoid brittle whitespace matches.
    new = replace_function(new, "    virtual void on_first_open() override", ON_FIRST_OPEN)

    # Align with MaixPy-v1 sdcard.c: CMD55 and ACMD41 are sent with CRC byte 1.
    new = replace_regex_once(
        new,
        r"sd_send_cmd\(SD_CMD55,\s*0,\s*0\);",
        "sd_send_cmd(SD_CMD55, 0, 1);",
        "MaixPy CMD55 CRC byte",
    )
    new = replace_regex_once(
        new,
        r"sd_send_cmd\(SD_ACMD41,\s*0x40000000,\s*0\);",
        "sd_send_cmd(SD_ACMD41, 0x40000000, 1);",
        "MaixPy ACMD41 CRC byte",
    )

    # Align with MaixPy-v1: cards without OCR CCS bit are not rejected. They are
    # initialized as SDSC/SDv1 and forced to 512-byte blocks with CMD16.
    new = replace_count(
        new,
        '''        if ((frame[0] & 0x40) == 0)
            return 0xFF;

        spi8_dev_->set_clock_rate(SD_SPI_HIGH_CLOCK_RATE);
        return sd_get_cardinfo(&card_info_);
''',
        '''        if ((frame[0] & 0x40) == 0)
        {
            sd_send_cmd(SD_CMD16, 512, 1);
            if (sd_get_response() != 0x00)
            {
                sd_end_cmd();
                return 0xFF;
            }
            sd_end_cmd();
        }

        spi8_dev_->set_clock_rate(SD_SPI_HIGH_CLOCK_RATE);
        return sd_get_cardinfo(&card_info_);
''',
        "MaixPy SDSC CMD16 path",
    )

    if new == old:
        print("unchanged: lib/drivers/src/storage/sdcard.cpp")
    else:
        SDCARD.write_text(new, encoding="utf-8", newline="\n")
        print("patched:   lib/drivers/src/storage/sdcard.cpp")
    print("SDCARD_DRIVER_PATCH_OK maixpy_crc=1 sdsc_cmd16=1 fail_fast=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
