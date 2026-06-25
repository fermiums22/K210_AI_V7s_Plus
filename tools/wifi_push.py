#!/usr/bin/env python3
"""WiFi file push: PC server that serves every file in <folder> to the K210
client (tools/probes/wifi_recv.py), which writes them to /sd.

Protocol (per file): "<name>\n" "<size>\n" <size bytes>. Ends with "EOF\n".

Usage: python tools/wifi_push.py <folder> [port]
"""
import socket, os, sys

folder = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8888
files = [f for f in sorted(os.listdir(folder))
         if os.path.isfile(os.path.join(folder, f)) and not f.startswith('.')]

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(1)
srv.settimeout(90)
print("serving %d files on :%d -> %s" % (len(files), port, files), flush=True)
conn, addr = srv.accept()
print("client connected:", addr, flush=True)
for name in files:
    data = open(os.path.join(folder, name), "rb").read()
    conn.sendall(name.encode() + b"\n")
    conn.sendall((str(len(data)) + "\n").encode())
    conn.sendall(data)
    print("sent %-18s %d bytes" % (name, len(data)), flush=True)
conn.sendall(b"EOF\n")
try:
    print("board ack:", conn.recv(16), flush=True)
except Exception:
    pass
conn.close(); srv.close()
print("PUSH DONE", flush=True)
