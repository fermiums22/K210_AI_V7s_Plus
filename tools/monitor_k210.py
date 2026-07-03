#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import sys
import time
from pathlib import Path

try:
    import serial
except Exception as exc:
    print(f"ERROR: pyserial is not installed or failed to import: {exc}")
    print("Run: py -3 -m pip install pyserial")
    raise SystemExit(2)

ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"


def open_serial_with_retry(port: str, baud: int, timeout_s: float) -> serial.Serial:
    deadline = time.monotonic() + timeout_s
    last_exc: Exception | None = None
    while time.monotonic() < deadline:
        try:
            ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
            # Do not intentionally drive K210 boot/reset lines from the monitor.
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            return ser
        except Exception as exc:  # COM can still be released by kflash for a moment.
            last_exc = exc
            time.sleep(0.2)
    raise SystemExit(f"ERROR: cannot open {port} @ {baud}: {last_exc}")


def monitor(port: str, baud: int, timeout_s: float, duration_s: float | None) -> int:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = LOG_DIR / f"k210_monitor_{stamp}.log"

    print(f"=== K210 monitor ===")
    print(f"Port: {port}")
    print(f"Baud: {baud}")
    print(f"Log : {log_path}")
    print("Press Ctrl+C to stop.")
    print()

    ser = open_serial_with_retry(port, baud, timeout_s)
    started = time.monotonic()
    try:
        with log_path.open("wb") as f:
            while True:
                if duration_s is not None and (time.monotonic() - started) >= duration_s:
                    print()
                    print(f"Monitor duration reached: {duration_s:.1f}s")
                    return 0
                data = ser.read(512)
                if not data:
                    continue
                f.write(data)
                f.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        print()
        print(f"Stopped. Log saved: {log_path}")
        return 130
    finally:
        try:
            ser.close()
        except Exception:
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description="K210 UART monitor with file logging")
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--open-timeout", type=float, default=10.0)
    ap.add_argument("--duration", type=float, default=None, help="Optional auto-stop duration in seconds")
    args = ap.parse_args()
    return monitor(args.port, args.baud, args.open_timeout, args.duration)


if __name__ == "__main__":
    raise SystemExit(main())
