#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
src = ROOT / "lds" / "kendryte.ld"
dst = ROOT / "lds" / "kendryte_app_slot0.ld"

s = src.read_text(encoding="utf-8")
s = s.replace("ORIGIN = 0x80000000, LENGTH = (6 * 1024 * 1024)",
              "ORIGIN = 0x80100000, LENGTH = (5 * 1024 * 1024)")
s = s.replace("SECTIONS\n{\n  .text.start :", "SECTIONS\n{\n  .app_header :\n  {\n    KEEP( *(.app_header) )\n  } > ram : DATA\n\n  .text.start :")
s = s.replace("PROVIDE( _heap_end = _ram_end );", "PROVIDE( _heap_end = _ram_end );\n  PROVIDE( _app_image_size = ABSOLUTE(_end) - ORIGIN(ram) );")
dst.write_text(s, encoding="utf-8", newline="\n")
print(f"APP_SLOT0_LD_OK {dst}")
