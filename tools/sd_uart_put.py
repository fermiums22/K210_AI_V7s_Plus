#!/usr/bin/env python3
"""Upload files to the K210 SD card over UARTHS.

The firmware waits briefly after SD mount and accepts:
  KSD1\n
  PUT <relative/path> <size>\n
  <raw bytes>
  DONE\n

Example:
  py tools/sd_uart_put.py COM12 sdcard/esp_boot.bin esp_boot.bin sdcard/esp_irom.bin esp_irom.bin sdcard/flash.json flash.json
"""
from pathlib import Path
import sys
import time

import serial


BAUD = 115200


def reset_to_boot_dan(ser: serial.Serial) -> None:
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    ser.setDTR(False)
    time.sleep(0.1)


def read_until(ser: serial.Serial, token: bytes, timeout: float) -> bytes:
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            buf += data
            if token in buf:
                return buf
    raise TimeoutError(f"timeout waiting for {token!r}; tail={buf[-200:]!r}")


def send_file(ser: serial.Serial, local: Path, remote: str, wait_cmd: bool) -> None:
    data = local.read_bytes()
    header = f"PUT {remote} {len(data)}\n".encode("ascii")
    print(f"put {local} -> {remote} ({len(data)} bytes)", flush=True)
    if wait_cmd:
        read_until(ser, b"KSD:CMD\n", 10)
    ser.write(header)
    ser.flush()
    read_until(ser, b"KSD:GO\n", 10)

    block = 512
    sent = 0
    for off in range(0, len(data), block):
        chunk = data[off:off + block]
        ser.write(chunk)
        ser.flush()
        read_until(ser, b"KSD:B\n", 10)
        sent += len(chunk)
        if sent % (256 * 1024) == 0 or sent == len(data):
            print(f"  {sent}/{len(data)}", flush=True)
    ser.flush()
    read_until(ser, b"KSD:OK\n", 30)


def main(argv: list[str]) -> int:
    tail = "--tail" in argv
    argv = [a for a in argv if a != "--tail"]
    if len(argv) < 4 or (len(argv) - 2) % 2:
        print(__doc__)
        return 1

    port = argv[1]
    pairs = [(Path(argv[i]), argv[i + 1].replace("\\", "/")) for i in range(2, len(argv), 2)]

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.05
    ser.write_timeout = 5
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False

    try:
        reset_to_boot_dan(ser)
        print("waiting for KSD:READY...", flush=True)
        read_until(ser, b"KSD:READY\n", 20)
        ser.write(b"KSD1\n")
        ser.flush()
        read_until(ser, b"KSD:HELLO\n", 5)

        first = True
        for local, remote in pairs:
            send_file(ser, local, remote, first)
            first = False

        ser.write(b"DONE\n")
        ser.flush()
        read_until(ser, b"KSD:DONE\n", 5)
        print("upload complete; board should continue to ESP flashing", flush=True)

        if tail:
            end = time.time() + 120
            while time.time() < end:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
