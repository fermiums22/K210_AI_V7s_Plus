#!/usr/bin/env python3
"""Drive the board over WiFi via dev_server.

  python tools/dev_run.py exec <script.py>        # run code on board, print output
  python tools/dev_run.py file <name> <local>     # push local file -> /sd/<name>
  python tools/dev_run.py code "<inline>"         # run an inline snippet
  python tools/dev_run.py bye                      # stop the agent
"""
import socket, sys, os

CMD = ("127.0.0.1", 8891)


def recvn(s, n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise OSError("closed")
        b += d
    return b


def send(msg, stream_path=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(120)
    s.connect(CMD)
    s.sendall(len(msg).to_bytes(4, "big") + msg)
    if stream_path:                       # FILE: stream the bytes after header
        with open(stream_path, "rb") as f:
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                s.sendall(chunk)
    n = int.from_bytes(recvn(s, 4), "big")
    out = recvn(s, n)
    s.close()
    return out


def main(argv):
    mode = argv[1]
    stream = None
    if mode == "exec":
        msg = b"EXEC\n" + open(argv[2], "rb").read()
    elif mode == "code":
        msg = b"EXEC\n" + argv[2].encode()
    elif mode == "file":
        name, local = argv[2], argv[3]
        size = os.path.getsize(local)
        msg = ("FILE %s %d\n" % (name, size)).encode()
        stream = local
    elif mode == "bye":
        msg = b"BYE\n"
    else:
        print(__doc__); return 1
    out = send(msg, stream)
    sys.stdout.buffer.write(out)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
