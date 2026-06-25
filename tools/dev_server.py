#!/usr/bin/env python3
"""PC-side dev bridge. The board's dev_agent connects here (port 8890); local
commands come in on 127.0.0.1:8891 and are relayed to the board, whose reply is
returned. Run this persistently in the background, then use tools/dev_run.py.
"""
import socket, threading

BOARD_PORT = 8890
CMD_PORT = 8891
board_conn = None
board_lock = threading.Lock()


def recvn(s, n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise OSError("closed")
        b += d
    return b


def serve_board():
    global board_conn
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", BOARD_PORT))
    srv.listen(1)
    print("waiting for board on", BOARD_PORT, flush=True)
    while True:
        c, a = srv.accept()
        try:
            n = int.from_bytes(recvn(c, 4), "big")
            ready = recvn(c, n).decode()
        except Exception:
            c.close(); continue
        with board_lock:
            board_conn = c
        print("board connected:", a, ready, flush=True)


def serve_cmds():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", CMD_PORT))
    srv.listen(4)
    print("command port ready on", CMD_PORT, flush=True)
    while True:
        c, a = srv.accept()
        try:
            n = int.from_bytes(recvn(c, 4), "big")
            msg = recvn(c, n)
            with board_lock:
                bc = board_conn
            if not bc:
                c.sendall(_frame(b"[dev_server] no board connected"))
                c.close(); continue
            bc.sendall(len(msg).to_bytes(4, "big") + msg)
            # FILE commands stream the file bytes after the framed header
            line = msg.split(b"\n", 1)[0]
            if line.startswith(b"FILE "):
                size = int(line.split(b" ")[2])
                left = size
                while left > 0:
                    chunk = c.recv(65536 if left > 65536 else left)
                    if not chunk:
                        break
                    bc.sendall(chunk)
                    left -= len(chunk)
            rn = int.from_bytes(recvn(bc, 4), "big")
            resp = recvn(bc, rn)
            c.sendall(_frame(resp))
        except Exception as e:
            try:
                c.sendall(_frame(("[dev_server] error: %r" % e).encode()))
            except Exception:
                pass
        finally:
            c.close()


def _frame(data):
    return len(data).to_bytes(4, "big") + data


if __name__ == "__main__":
    threading.Thread(target=serve_board, daemon=True).start()
    serve_cmds()
