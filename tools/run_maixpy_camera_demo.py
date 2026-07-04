#!/usr/bin/env python3
"""Send tools/maixpy_camera_demo.py to a MaixPy/MicroPython raw REPL.

Usage:
  py -3 tools\run_maixpy_camera_demo.py --port COM12
"""
import argparse
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("ERROR: pyserial is not installed. Run: py -3 -m pip install pyserial", file=sys.stderr)
    sys.exit(1)


def read_some(ser, seconds=1.0):
    end = time.time() + seconds
    out = bytearray()
    while time.time() < end:
        n = ser.in_waiting
        if n:
            out += ser.read(n)
            end = time.time() + 0.15
        else:
            time.sleep(0.02)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("script", nargs="?", default=str(Path(__file__).with_name("maixpy_camera_demo.py")))
    args = ap.parse_args()

    script = Path(args.script)
    code = script.read_text(encoding="utf-8")

    print(f"[maixpy] opening {args.port} @ {args.baud}")
    with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=2) as ser:
        time.sleep(0.4)
        ser.write(b"\x03\x03")  # Ctrl-C twice
        ser.flush()
        print(read_some(ser, 0.8).decode("utf-8", "replace"), end="")

        ser.write(b"\x01")  # raw REPL
        ser.flush()
        raw = read_some(ser, 1.0)
        print(raw.decode("utf-8", "replace"), end="")
        if b"raw REPL" not in raw and b">" not in raw:
            print("\nWARN: raw REPL banner not detected; continuing anyway")

        ser.write(code.encode("utf-8"))
        ser.write(b"\x04")  # execute
        ser.flush()
        print(read_some(ser, 2.0).decode("utf-8", "replace"), end="")

    print("\n[maixpy] demo sent. The LCD should show the live camera image if the vendor firmware supports this board.")


if __name__ == "__main__":
    main()
