#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

PAD_LIMIT = 4096

if len(sys.argv) not in (2, 3):
    print("usage: print_slot0_header.py [--pad] <k210_app_slot0.bin>", file=sys.stderr)
    raise SystemExit(2)

pad = False
arg = sys.argv[1]
if arg == "--pad":
    pad = True
    arg = sys.argv[2]

p = Path(arg)
b = p.read_bytes()
if len(b) < 32:
    print(f"ERROR: file too small: {p} size={len(b)}", file=sys.stderr)
    raise SystemExit(1)

magic, magic_inv, load, entry, image_size, crc32, flags, reserved = struct.unpack_from("<8I", b, 0)

if magic != 0x4B323130 or magic_inv != 0xB4CDCEDF:
    print(f"APP_SLOT0_BIN_SIZE {len(b)}")
    print(
        "APP_SLOT0_HDR "
        f"magic=0x{magic:08x} inv=0x{magic_inv:08x} "
        f"load=0x{load:08x} entry=0x{entry:08x} "
        f"image_size={image_size} crc32=0x{crc32:08x} flags=0x{flags:08x}"
    )
    print("ERROR: bad slot0 header magic", file=sys.stderr)
    raise SystemExit(1)

if image_size == 0:
    print("ERROR: image_size is zero", file=sys.stderr)
    raise SystemExit(1)

if image_size > len(b):
    missing = image_size - len(b)
    if not pad or missing > PAD_LIMIT:
        print(f"APP_SLOT0_BIN_SIZE {len(b)}")
        print(
            "APP_SLOT0_HDR "
            f"magic=0x{magic:08x} inv=0x{magic_inv:08x} "
            f"load=0x{load:08x} entry=0x{entry:08x} "
            f"image_size={image_size} crc32=0x{crc32:08x} flags=0x{flags:08x}"
        )
        print(f"ERROR: invalid image_size={image_size} for file size={len(b)}", file=sys.stderr)
        raise SystemExit(1)
    with p.open("ab") as f:
        f.write(b"\x00" * missing)
    b += b"\x00" * missing
    print(f"APP_SLOT0_PADDED +{missing} bytes")

print(f"APP_SLOT0_BIN_SIZE {len(b)}")
print(
    "APP_SLOT0_HDR "
    f"magic=0x{magic:08x} inv=0x{magic_inv:08x} "
    f"load=0x{load:08x} entry=0x{entry:08x} "
    f"image_size={image_size} crc32=0x{crc32:08x} flags=0x{flags:08x}"
)

if image_size > len(b):
    print(f"ERROR: invalid image_size={image_size} for file size={len(b)}", file=sys.stderr)
    raise SystemExit(1)
