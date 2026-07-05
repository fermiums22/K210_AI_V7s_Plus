#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDCARD = ROOT / "lib" / "drivers" / "src" / "storage" / "sdcard.cpp"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"patch failed: {label}: expected 1 match, got {count}")
    return text.replace(old, new, 1)


def main() -> int:
    old = SDCARD.read_text(encoding="utf-8")
    new = old

    # The SDK driver used to keep a live handle after sd_init() returned 0xff.
    # That makes the filesystem mount a half-initialized device and produces
    # garbage capacity/block values. Make init failure fatal for the install path.
    new = replace_once(
        new,
        '''        spi8_dev_->set_clock_rate(SD_SPI_LOW_CLOCK_RATE);
        int init_rc = sd_init();
        init_ok_ = init_rc == 0;
        printf("[sdcard] init rc=%d capacity=%llu block=%lu\n",
               init_rc,
               (unsigned long long)card_info_.CardCapacity,
               (unsigned long)card_info_.CardBlockSize);
''',
        '''        memset(&card_info_, 0, sizeof(card_info_));
        spi8_dev_->set_clock_rate(SD_SPI_LOW_CLOCK_RATE);
        int init_rc = sd_init();
        init_ok_ = init_rc == 0;
        printf("[sdcard] init rc=%d capacity=%llu block=%lu\n",
               init_rc,
               (unsigned long long)card_info_.CardCapacity,
               (unsigned long)card_info_.CardBlockSize);
        if (!init_ok_)
            throw init_rc;
''',
        "fail-fast on sd_init failure",
    )

    # Align with MaixPy-v1 sdcard.c: CMD55 and ACMD41 are sent with CRC byte 1.
    new = replace_once(
        new,
        '''            sd_send_cmd(SD_CMD55, 0, 0);
            result = sd_get_response();
            sd_end_cmd();
            if (result != 0x01)
                return 0xFF;
            sd_send_cmd(SD_ACMD41, 0x40000000, 0);
''',
        '''            sd_send_cmd(SD_CMD55, 0, 1);
            result = sd_get_response();
            sd_end_cmd();
            if (result != 0x01)
                return 0xFF;
            sd_send_cmd(SD_ACMD41, 0x40000000, 1);
''',
        "MaixPy CMD55/ACMD41 CRC bytes",
    )

    # Align with MaixPy-v1: cards without OCR CCS bit are not rejected. They are
    # initialized as SDSC/SDv1 and forced to 512-byte blocks with CMD16.
    new = replace_once(
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
