#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

if len(sys.argv) != 2:
    print("usage: print_slot0_header.py <k210_app_slot0.bin>", file=sys.stderr)
    raise SystemExit(2)

p = Path(sys.argv[1])
b = p.read_bytes()
if len(b) < 32:
    print(f"ERROR: file too small: {p} size={len(b)}", file=sys.stderr)
    raise SystemExit(1)

magic, magic_inv, load, entry, image_size, crc32, flags, reserved = struct.unpack_from("<8I", b, 0)
print(f"APP_SLOT0_BIN_SIZE {len(b)}")
print(
    "APP_SLOT0_HDR "
    f"magic=0x{magic:08x} inv=0x{magic_inv:08x} "
    f"load=0x{load:08x} entry=0x{entry:08x} "
    f"image_size={image_size} crc32=0x{crc32:08x} flags=0x{flags:08x}"
)

if magic != 0x4B323130 or magic_inv != 0xB4CDCEDF:
    print("ERROR: bad slot0 header magic", file=sys.stderr)
    raise SystemExit(1)
if image_size == 0 or image_size > len(b):
    print(f"ERROR: invalid image_size={image_size} for file size={len(b)}", file=sys.stderr)
    raise SystemExit(1)
