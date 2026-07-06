#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

try:
    import serial  # type: ignore
except Exception as exc:
    print(f"ERROR: pyserial import failed: {exc}")
    print("Run: py -3 -m pip install pyserial")
    raise SystemExit(2)

KSD_MAGIC = b"KSD1\n"
DEFAULT_REMOTE = "ksd_console_probe.bin"


def make_payload(size: int) -> bytes:
    seed = b"K210-KSD-CONSOLE-RW\n"
    out = bytearray()
    counter = 0
    while len(out) < size:
        out.extend(hashlib.sha256(seed + counter.to_bytes(4, "little")).digest())
        counter += 1
    return bytes(out[:size])


class Ksd:
    def __init__(self, port: str, baud: int, max_lines: int):
        self.ser = serial.Serial(port=port, baudrate=baud, timeout=3.0, write_timeout=5.0)
        self.ser.dtr = False
        self.ser.rts = False
        self.max_lines = max_lines
        self.have_prompt = False

    def close(self) -> None:
        self.ser.close()

    def line(self, stage: str) -> str:
        raw = self.ser.readline()
        if not raw:
            raise SystemExit(f"{stage}: no line from K210")
        text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if text != "KSD:B":
            print(text)
        return text

    def connect(self) -> None:
        print("[ksd] strict connect: drain boot banner, send one magic when listener is visible")
        for _ in range(self.max_lines):
            raw = self.ser.readline()
            if not raw:
                break
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if text:
                print(text)
            if text.startswith("KSD:CMD"):
                self.have_prompt = True
                print("[ksd] connected: prompt already active")
                return
            if text.startswith("KSD:HELLO"):
                print("[ksd] connected")
                return
            if text.startswith("KSD:READY") or "PC UART KSD listener" in text:
                break
        self.ser.write(KSD_MAGIC)
        self.ser.flush()
        for _ in range(self.max_lines):
            line = self.line("connect")
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"connect: {line}")
            if line.startswith("KSD:HELLO"):
                print("[ksd] connected")
                return
            if line.startswith("KSD:CMD"):
                self.have_prompt = True
                print("[ksd] connected: prompt")
                return
        raise SystemExit("connect: line guard exceeded")

    def wait_prompt(self, stage: str) -> None:
        if self.have_prompt:
            self.have_prompt = False
            return
        for _ in range(self.max_lines):
            line = self.line(stage)
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"{stage}: {line}")
            if line.startswith("KSD:CMD"):
                return
        raise SystemExit(f"{stage}: line guard exceeded")

    def command(self, cmd: str, terminal: tuple[str, ...]) -> list[str]:
        self.wait_prompt(cmd + " prompt")
        print(f"[ksd] > {cmd}")
        self.ser.write((cmd + "\n").encode("ascii"))
        self.ser.flush()
        lines: list[str] = []
        for _ in range(self.max_lines):
            line = self.line(cmd)
            lines.append(line)
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"{cmd}: {line}")
            if line.startswith(terminal):
                return lines
        raise SystemExit(f"{cmd}: line guard exceeded")

    def help(self) -> None:
        lines = self.command("HELP", ("KSD:HELP_END",))
        required = ("GET <path>", "PUT <path> <size>", "SD_TEST")
        joined = "\n".join(lines)
        missing = [x for x in required if x not in joined]
        if missing:
            raise SystemExit("HELP missing required commands: " + ", ".join(missing))

    def sd_test(self) -> None:
        lines = self.command("SD_TEST", ("KSD:SD_OK", "KSD:SD_FAIL"))
        if not lines[-1].startswith("KSD:SD_OK"):
            raise SystemExit("SD_TEST failed: " + lines[-1])

    def put(self, remote: str, data: bytes) -> None:
        self.wait_prompt("PUT prompt")
        print(f"[ksd] > PUT {remote} {len(data)}")
        self.ser.write(f"PUT {remote} {len(data)}\n".encode("ascii"))
        self.ser.flush()
        chunk_size = 512
        for _ in range(self.max_lines):
            line = self.line("PUT GO")
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"PUT GO: {line}")
            m = re.match(r"KSD:GO(?:\s+(\d+))?", line)
            if m:
                if m.group(1):
                    chunk_size = int(m.group(1))
                break
        else:
            raise SystemExit("PUT GO: line guard exceeded")
        for _ in range(self.max_lines):
            line = self.line("PUT READYDATA")
            if line.startswith("KSD:READYDATA"):
                break
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"PUT READYDATA: {line}")
        else:
            raise SystemExit("PUT READYDATA: line guard exceeded")
        sent = 0
        acks = 0
        while sent < len(data):
            chunk = data[sent:sent + chunk_size]
            self.ser.write(chunk)
            self.ser.flush()
            sent += len(chunk)
            for _ in range(self.max_lines):
                line = self.line(f"PUT ack {sent}/{len(data)}")
                if line.startswith("KSD:B"):
                    acks += 1
                    break
                if line.startswith("KSD:ERR"):
                    raise SystemExit(f"PUT ack: {line}")
            else:
                raise SystemExit("PUT ack: line guard exceeded")
        for _ in range(self.max_lines):
            line = self.line("PUT final")
            if line.startswith("KSD:OK"):
                print(f"[ksd] PUT OK bytes={sent} chunk={chunk_size} acks={acks}")
                return
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"PUT final: {line}")
        raise SystemExit("PUT final: line guard exceeded")

    def get(self, remote: str) -> bytes:
        self.wait_prompt("GET prompt")
        print(f"[ksd] > GET {remote}")
        self.ser.write(("GET " + remote + "\n").encode("ascii"))
        self.ser.flush()
        size = None
        for _ in range(self.max_lines):
            line = self.line("GET size")
            if line.startswith("KSD:MISSING") or line.startswith("KSD:ERR"):
                raise SystemExit(f"GET size: {line}")
            m = re.match(r"KSD:SIZE (\d+)", line)
            if m:
                size = int(m.group(1))
                break
        if size is None:
            raise SystemExit("GET size: line guard exceeded")
        data = self.ser.read(size)
        if len(data) != size:
            raise SystemExit(f"GET data: short read {len(data)}/{size}")
        for _ in range(self.max_lines):
            line = self.line("GET final")
            if line.startswith("KSD:OK"):
                return data
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"GET final: {line}")
        raise SystemExit("GET final: line guard exceeded")

    def done(self) -> None:
        self.wait_prompt("DONE prompt")
        self.ser.write(b"DONE\n")
        self.ser.flush()
        for _ in range(self.max_lines):
            line = self.line("DONE")
            if line.startswith("KSD:DONE"):
                return


def main() -> int:
    ap = argparse.ArgumentParser(description="Strict K210 KSD command console + SD PUT/GET byte verifier; no automatic retries.")
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--size", type=int, default=4096)
    ap.add_argument("--remote", default=DEFAULT_REMOTE)
    ap.add_argument("--out", default="logs/ksd_console_probe.bin")
    ap.add_argument("--max-lines", type=int, default=500)
    args = ap.parse_args()

    if args.size <= 0:
        raise SystemExit("--size must be positive")

    payload = make_payload(args.size)
    want_sha = hashlib.sha256(payload).hexdigest()

    ksd = Ksd(args.port, args.baud, args.max_lines)
    try:
        ksd.connect()
        ksd.help()
        ksd.sd_test()
        ksd.put(args.remote, payload)
        got = ksd.get(args.remote)
        ksd.done()
    finally:
        ksd.close()

    got_sha = hashlib.sha256(got).hexdigest()
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(got)
    if got != payload:
        raise SystemExit(f"VERIFY_FAIL path={args.remote} size={len(got)} sha_tx={want_sha} sha_rx={got_sha}")
    print(f"PASS KSD_CONSOLE_RW path={args.remote} size={len(got)} sha256={want_sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
