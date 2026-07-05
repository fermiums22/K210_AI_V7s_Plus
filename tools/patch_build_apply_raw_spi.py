#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
P = ROOT / "build_k210.bat"
text = P.read_text(encoding="utf-8")

blocks = [
    '''
if exist "tools\\patch_sdcard_raw_spi.py" (
  py -3 tools\\patch_sdcard_raw_spi.py
  if errorlevel 1 (
    echo ERROR: SD raw SPI patch failed
    exit /b 1
  )
)
''',
    '''
if exist "tools\\patch_sdcard_full_duplex_read.py" (
  py -3 tools\\patch_sdcard_full_duplex_read.py
  if errorlevel 1 (
    echo ERROR: SD command probe patch failed
    exit /b 1
  )
)
''',
    '''
if exist "tools\\patch_sdcard_cs_mask.py" (
  py -3 tools\\patch_sdcard_cs_mask.py
  if errorlevel 1 (
    echo ERROR: SD CS mask patch failed
    exit /b 1
  )
)
''',
]

changed = False
for block in blocks:
    if block in text:
        text = text.replace(block, "", 1)
        changed = True

if changed:
    P.write_text(text, encoding="utf-8", newline="\r\n")
    print("patched:   build_k210.bat")
else:
    print("unchanged: build_k210.bat")

print("BUILD_RAW_SPI_PATCH_DISABLED_OK")
print("BUILD_SDCARD_BUILD_PATCHES_DISABLED_OK")
