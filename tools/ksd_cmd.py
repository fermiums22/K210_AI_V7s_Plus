#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

try:
    import serial
except Exception as exc:
    print(f"ERROR: pyserial import failed: {exc}")
    print("Run: py -3 -m pip install pyserial")
    raise SystemExit(2)


def open_port(port: str, baud: int) -> serial.Serial:
    deadline = time.monotonic() + 10.0
    last = None
    while time.monotonic() < deadline:
        try:
            ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            return ser
        except Exception as exc:
            last = exc
            time.sleep(0.2)
    raise SystemExit(f"ERROR: cannot open {port}: {last}")


def read_line(ser: serial.Serial, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    buf = bytearray()
    while time.monotonic() < deadline:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        if b == b"\n":
            return bytes(buf)
    raise TimeoutError("line timeout")


def read_ksd_line(ser: serial.Serial, timeout: float = 10.0) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            raw = read_line(ser, max(0.2, deadline - time.monotonic()))
        except TimeoutError:
            break
        text = raw.decode("latin1", errors="replace").strip()
        if text:
            print(text)
        idx = text.find("KSD:")
        if idx >= 0:
            return text[idx:]
    raise TimeoutError("KSD line timeout")


def connect(ser: serial.Serial) -> None:
    ser.reset_input_buffer()
    ser.write(b"KSD1\n")
    ser.flush()
    while True:
        line = read_ksd_line(ser, 10.0)
        if line == "KSD:HELLO":
            return


def wait_cmd_prompt(ser: serial.Serial) -> None:
    while True:
        line = read_ksd_line(ser, 10.0)
        if line == "KSD:CMD":
            return


def send_done(ser: serial.Serial) -> None:
    wait_cmd_prompt(ser)
    ser.write(b"DONE\n")
    ser.flush()
    try:
        while True:
            line = read_ksd_line(ser, 2.0)
            if line == "KSD:DONE":
                return
    except TimeoutError:
        return


def run_simple_command(ser: serial.Serial, cmd: str) -> list[str]:
    wait_cmd_prompt(ser)
    ser.write((cmd + "\n").encode("ascii"))
    ser.flush()
    lines: list[str] = []
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        line = read_ksd_line(ser, max(0.2, deadline - time.monotonic()))
        lines.append(line)
        if line.startswith("KSD:CAPTURE_OK") or line.startswith("KSD:CAPTURE_FAIL"):
            return lines
        if line.startswith("KSD:FLASH_OK") or line.startswith("KSD:FLASH_FAIL"):
            return lines
        if line.startswith("KSD:FORMAT_OK") or line.startswith("KSD:FORMAT_FAIL"):
            return lines
        if line.startswith("KSD:ERR"):
            return lines
    raise TimeoutError(f"command timeout: {cmd}")


def get_file(ser: serial.Serial, remote: str, local: Path) -> int:
    wait_cmd_prompt(ser)
    ser.write(("GET " + remote + "\n").encode("ascii"))
    ser.flush()
    line = read_ksd_line(ser, 10.0)
    if line == "KSD:MISSING":
        raise SystemExit(f"ERROR: remote file missing: {remote}")
    m = re.match(r"KSD:SIZE (\d+)", line)
    if not m:
        raise SystemExit(f"ERROR: expected KSD:SIZE, got {line}")
    size = int(m.group(1))
    print(f"[get] {remote} -> {local} ({size} bytes)")
    data = ser.read(size)
    while len(data) < size:
        chunk = ser.read(size - len(data))
        if not chunk:
            raise SystemExit(f"ERROR: short read {len(data)}/{size}")
        data += chunk
    local.parent.mkdir(parents=True, exist_ok=True)
    local.write_bytes(data)
    ok = read_ksd_line(ser, 10.0)
    if ok != "KSD:OK":
        raise SystemExit(f"ERROR: expected KSD:OK after data, got {ok}")
    return size


def main() -> int:
    ap = argparse.ArgumentParser(description="K210 KSD UART command client")
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--cmd", required=True, help="Command, e.g. 'CAM_CAPTURE cam/capture.rgb565'")
    ap.add_argument("--get", dest="get_path", help="Optional remote file to GET after command")
    ap.add_argument("--out", default="logs/ksd_get.bin", help="Local output path for --get")
    args = ap.parse_args()

    ser = open_port(args.port, args.baud)
    try:
        connect(ser)
        lines = run_simple_command(ser, args.cmd)
        failed = any("FAIL" in x or x.startswith("KSD:ERR") for x in lines)
        if args.get_path and not failed:
            n = get_file(ser, args.get_path, Path(args.out))
            print(f"[get] OK {n} bytes")
        send_done(ser)
        return 1 if failed else 0
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
