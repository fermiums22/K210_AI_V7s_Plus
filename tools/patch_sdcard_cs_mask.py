#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
P = ROOT / "lib" / "drivers" / "src" / "storage" / "sdcard.cpp"

s = P.read_text(encoding="utf-8")
old = "spi->get_device(SPI_MODE_0, SPI_FF_STANDARD, 1, 8)"
new = "spi->get_device(SPI_MODE_0, SPI_FF_STANDARD, 2, 8)"
if old not in s and new not in s:
    raise SystemExit("patch failed: SD SPI get_device mask line not found")
if old in s:
    s = s.replace(old, new, 1)
    P.write_text(s, encoding="utf-8", newline="\n")
    print("patched:   lib/drivers/src/storage/sdcard.cpp")
else:
    print("unchanged: lib/drivers/src/storage/sdcard.cpp")
print("SDCARD_CS_MASK_PATCH_OK maixpy_sd_ss=SPI_CHIP_SELECT_1 mask=2")
