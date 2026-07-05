#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
P = ROOT / "build_k210.bat"
text = P.read_text(encoding="utf-8")
needle = '''if exist "tools\\patch_sdcard_cs_mask.py" (
  py -3 tools\\patch_sdcard_cs_mask.py
  if errorlevel 1 (
    echo ERROR: SD CS mask patch failed
    exit /b 1
  )
)
'''
insert = needle + '''
if exist "tools\\patch_sdcard_raw_spi.py" (
  py -3 tools\\patch_sdcard_raw_spi.py
  if errorlevel 1 (
    echo ERROR: SD raw SPI patch failed
    exit /b 1
  )
)
'''
if "patch_sdcard_raw_spi.py" in text:
    print("unchanged: build_k210.bat")
elif needle in text:
    text = text.replace(needle, insert, 1)
    P.write_text(text, encoding="utf-8", newline="\r\n")
    print("patched:   build_k210.bat")
else:
    raise SystemExit("patch failed: CS mask block not found in build_k210.bat")
print("BUILD_RAW_SPI_PATCH_OK")
