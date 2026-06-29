import socket
import sys

host = "0.0.0.0"
port = int(sys.argv[1]) if len(sys.argv) > 1 else 9090
out_path = sys.argv[2] if len(sys.argv) > 2 else "snapshot_push.bmp"

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((host, port))
srv.listen(1)
print(f"listening on {host}:{port}", flush=True)

conn, addr = srv.accept()
print(f"client {addr}", flush=True)
with conn, open(out_path, "wb") as f:
    total = 0
    while True:
        data = conn.recv(4096)
        if not data:
            break
        f.write(data)
        total += len(data)
print(f"wrote {total} bytes to {out_path}", flush=True)
