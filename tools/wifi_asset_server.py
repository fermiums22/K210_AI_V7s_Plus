#!/usr/bin/env python3
"""Serve a folder to the K210 WiFi pull bridge.

Protocol:
  PC waits for RDY\n from the board.
  <relative/path>\n
  <size>\n
  <raw bytes>
  ...
  EOF\n

Usage:
  py tools/wifi_asset_server.py sdcard 8888
"""
from pathlib import Path
import socket
import sys
import time

BLOCK = 2048
AFTER_ACK_DELAY = 0.005

def recv_line(conn: socket.socket, limit: int = 32) -> bytes:
    data = b""
    while not data.endswith(b"\n") and len(data) < limit:
        chunk = conn.recv(1)
        if not chunk:
            raise ConnectionError("board closed connection")
        data += chunk
    return data

root = Path(sys.argv[1] if len(sys.argv) > 1 else "sdcard").resolve()
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8888

files = []
for p in sorted(root.rglob("*")):
    if p.is_file() and not any(part.startswith(".") for part in p.relative_to(root).parts):
        files.append(p)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(1)
srv.settimeout(180)
print(f"serving {len(files)} files from {root} on :{port}", flush=True)

conn, addr = srv.accept()
print("client connected:", addr, flush=True)
with conn:
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.settimeout(20)
    hello = recv_line(conn)
    if hello != b"RDY\n":
        raise ConnectionError(f"unexpected board hello: {hello!r}")
    print("board ready", flush=True)
    time.sleep(AFTER_ACK_DELAY)
    conn.settimeout(None)

    for path in files:
        rel = path.relative_to(root).as_posix()
        data = path.read_bytes()
        padded_len = (len(data) + 511) & ~511
        header = (
            rel.encode("utf-8")
            + b"\n"
            + str(len(data)).encode("ascii")
            + b"\n"
            + str(padded_len).encode("ascii")
            + b"\n"
        )
        payload = data + bytes(padded_len - len(data))
        conn.sendall(header)
        go = recv_line(conn)
        if go != b"GO\n":
            raise ConnectionError(f"unexpected GO for {rel}: {go!r}")
        time.sleep(AFTER_ACK_DELAY)
        for off in range(0, len(payload), BLOCK):
            conn.sendall(payload[off:off + BLOCK])
            block_ack = recv_line(conn)
            if block_ack != b"B\n":
                raise ConnectionError(f"unexpected block ack for {rel}: {block_ack!r}")
            time.sleep(AFTER_ACK_DELAY)
            time.sleep(0.002)
        print(f"sent {rel:28s} {len(data)} bytes", flush=True)
        ack = recv_line(conn)
        if ack != b"OK\n":
            raise ConnectionError(f"unexpected ack for {rel}: {ack!r}")
        time.sleep(AFTER_ACK_DELAY)
    conn.sendall(b"EOF\n")
    try:
        print("board ack:", conn.recv(16), flush=True)
    except OSError:
        pass
srv.close()
print("ASSET PUSH DONE", flush=True)
