#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import time
from pathlib import Path

try:
    import serial
except Exception as exc:
    print(f"ERROR: pyserial import failed: {exc}")
    print("Run: py -3 -m pip install pyserial")
    raise SystemExit(2)

ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"


def log_line(f, line: str) -> None:
    print(line, flush=True)
    f.write((line + "\n").encode("utf-8", errors="replace"))
    f.flush()


def command_terminals(command: str) -> tuple[set[str], set[str]]:
    """Return KSD terminal response prefixes for a one-shot command."""
    cmd = command.strip().split(maxsplit=1)[0].upper()

    if cmd == "FORMAT_SD":
        return {"KSD:FORMAT_OK"}, {"KSD:FORMAT_FAIL"}
    if cmd == "CAM_CAPTURE":
        return {"KSD:CAPTURE_OK"}, {"KSD:CAPTURE_FAIL"}
    if cmd == "FLASH_ESP":
        return {"KSD:FLASH_OK"}, {"KSD:FLASH_FAIL"}
    if cmd == "RUN_SPI":
        return {"KSD:RUNSPI"}, set()
    if cmd == "RESET":
        return {"KSD:RESETTING"}, set()

    return {"KSD:OK", "KSD:DONE"}, {"KSD:ERR", "KSD:MISSING"}


def main() -> int:
    ap = argparse.ArgumentParser(description="Send one command to K210 KSD service")
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--command", required=True)
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    ok_prefixes, fail_prefixes = command_terminals(args.command)

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    safe_cmd = "_".join(args.command.strip().split())
    log_path = LOG_DIR / f"ksd_command_{safe_cmd}_{stamp}.log"

    with log_path.open("wb") as f:
        print("=== K210 KSD command ===")
        print(f"Port   : {args.port}")
        print(f"Baud   : {args.baud}")
        print(f"Command: {args.command}")
        print(f"Log    : {log_path}")
        print()

        ser = serial.Serial(args.port, args.baud, timeout=0.2)
        try:
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass

            deadline = time.monotonic() + args.timeout
            next_sync = 0.0
            got_hello = False
            sent_cmd = False

            while time.monotonic() < deadline:
                raw = ser.readline()
                if raw:
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    log_line(f, line)
                else:
                    line = ""

                now = time.monotonic()
                if not got_hello and ("KSD:READY" in line or now >= next_sync):
                    ser.write(b"KSD1\n")
                    ser.flush()
                    next_sync = now + 1.0
                    continue

                if "KSD:HELLO" in line:
                    got_hello = True
                    continue

                if got_hello and (not sent_cmd) and "KSD:CMD" in line:
                    ser.write((args.command + "\n").encode("ascii"))
                    ser.flush()
                    sent_cmd = True
                    continue

                if not sent_cmd or not line:
                    continue

                if any(line.startswith(prefix) for prefix in ok_prefixes):
                    ser.write(b"DONE\n")
                    ser.flush()
                    print(f"OK: {args.command} completed. Log: {log_path}")
                    return 0

                if any(line.startswith(prefix) for prefix in fail_prefixes):
                    ser.write(b"DONE\n")
                    ser.flush()
                    print(f"FAILED: {args.command}. Log: {log_path}")
                    return 1

            print(f"ERROR: timeout waiting for {args.command}. Log: {log_path}")
            return 3
        finally:
            ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
