#!/usr/bin/env python3
"""Minimal host-side raw-REPL driver for MaixPy (MicroPython) over USB-serial.

Usage:
    python tools/maix.py <COM> exec  "<one-liner>"
    python tools/maix.py <COM> run   <path-to-.py>      # paste a whole file, capture output
    python tools/maix.py <COM> put   <local.py> <remote.py>   # write file onto board fs
    python tools/maix.py <COM> reset                    # soft reset (Ctrl-D)

Talks the standard MicroPython raw-REPL protocol (Ctrl-A enter, Ctrl-D exec).
Default baud 115200 (MaixPy REPL on IO4/IO5). Close the MaixPy IDE first — it
holds the COM port.
"""
import sys, time, serial

BAUD = 115200


class Maix:
    def __init__(self, port, baud=BAUD):
        # On Maix Dock the CH340 DTR/RTS lines are wired to RESET/ISP. pyserial
        # asserts them on open, which kicks the K210 into the bootloader. Hold
        # both low so opening the port doesn't disturb the running firmware.
        s = serial.Serial()
        s.port = port
        s.baudrate = baud
        s.timeout = 0.2
        s.dtr = False
        s.rts = False
        s.open()
        s.dtr = False
        s.rts = False
        self.s = s
        time.sleep(0.1)

    def _read_until(self, token, timeout=10):
        token = token.encode() if isinstance(token, str) else token
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            n = self.s.in_waiting
            data = self.s.read(n if n else 1)
            if data:
                buf += data
                if token in buf:
                    return buf
        raise TimeoutError(f"timeout waiting for {token!r}; got: {buf[-200:]!r}")

    def _read_count(self, token, count, timeout=30):
        """Read until `token` appears `count` times; return whole buffer."""
        token = token.encode() if isinstance(token, str) else token
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            n = self.s.in_waiting
            data = self.s.read(n if n else 1)
            if data:
                buf += data
                t0 = time.time()
                if buf.count(token) >= count:
                    return buf
        raise TimeoutError(f"timeout waiting for {count}x {token!r}; got: {buf[-200:]!r}")

    def enter_raw(self):
        # Opening the port resets the board; it may still be booting / running
        # the demo main.py. Retry until the raw REPL prompt comes back.
        last = None
        for _ in range(12):
            self.s.write(b"\r\x03\x03")   # stop any running program
            time.sleep(0.25)
            self.s.reset_input_buffer()
            self.s.write(b"\r\x01")       # Ctrl-A: enter raw REPL
            try:
                self._read_until(b"raw REPL; CTRL-B to exit", 1.2)
                self._read_until(b">", 1.0)
                return
            except TimeoutError as e:
                last = e
                time.sleep(0.4)
        raise RuntimeError("could not enter raw REPL: %s" % last)

    def exit_raw(self):
        self.s.write(b"\r\x02")       # Ctrl-B: back to normal REPL

    def exec(self, code, timeout=30):
        if isinstance(code, str):
            code = code.encode()
        # Send in small chunks with brief pauses: MaixPy's raw REPL has no
        # flow control, so large bursts at 115200 overflow its input buffer
        # and corrupt the code (spurious SyntaxError).
        for i in range(0, len(code), 128):
            self.s.write(code[i:i + 128])
            self.s.flush()
            time.sleep(0.012)
        self.s.write(b"\x04")          # Ctrl-D: run
        ack = self._read_until(b"OK", 5)
        if b"OK" not in ack:
            raise RuntimeError("board did not accept code (no OK)")
        # Response is: stdout \x04 stderr \x04  (then '>')
        resp = self._read_count(b"\x04", 2, timeout)
        body = resp.split(b"OK", 1)[-1]
        out, err = body.split(b"\x04")[0], body.split(b"\x04")[1]
        if err:
            raise RuntimeError("board exception:\n" + err.decode(errors="replace"))
        return out.decode(errors="replace")

    def run_file(self, path, timeout=120):
        with open(path, "r", encoding="utf-8") as f:
            return self.exec(f.read(), timeout)

    def put_file(self, local, remote, timeout=60):
        """Stream a file to the board in base64 chunks (no giant literal)."""
        import base64
        with open(local, "rb") as f:
            data = f.read()
        self.exec("import ubinascii\nf=open(%r,'wb')" % remote)
        step = 1024  # raw bytes per chunk -> ~1368 b64 chars
        for i in range(0, len(data), step):
            b64 = base64.b64encode(data[i:i + step]).decode()
            self.exec("f.write(ubinascii.a2b_base64('%s'))" % b64)
        return self.exec(
            "f.close()\nprint('wrote',%d,'bytes to',%r)" % (len(data), remote))

    def put_text(self, local, remote, timeout=60):
        """Write a text file to the board in binary mode, chunked (robust).

        This SPIFFS build rejects text-mode 'w' (ENOENT); 'wb' works.
        """
        with open(local, "rb") as f:
            data = f.read()
        return self._write_bytes(remote, data)

    def write_text(self, remote, text, timeout=60):
        """Write an in-memory string to a file on the board (binary mode)."""
        return self._write_bytes(remote, text.encode("utf-8"))

    def _write_bytes(self, remote, data, step=160):
        # One-shot: build a single script (open + many short write lines +
        # close) and run it in ONE exec. Splitting open/write/close across
        # separate raw-REPL submissions hangs this MaixPy build; a single
        # script (as in on-board file generation) is reliable.
        parts = ["f=open(%r,'wb')" % remote]
        for i in range(0, len(data), step):
            parts.append("f.write(%r)" % data[i:i + step])
        parts.append("f.close()")
        parts.append("print('wrote',%d,'bytes to',%r)" % (len(data), remote))
        return self.exec("\n".join(parts))

    def soft_reset(self):
        self.s.write(b"\r\x03\x03")
        time.sleep(0.1)
        self.s.write(b"\x04")          # Ctrl-D in normal REPL = soft reset
        time.sleep(0.3)

    def close(self):
        try:
            self.exit_raw()
        except Exception:
            pass
        self.s.close()


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    port, cmd = argv[1], argv[2]
    m = Maix(port)
    try:
        m.enter_raw()
        if cmd == "exec":
            print(m.exec(argv[3]), end="")
        elif cmd == "run":
            print(m.run_file(argv[3]), end="")
        elif cmd == "put":
            print(m.put_file(argv[3], argv[4]), end="")
        elif cmd == "puttext":
            print(m.put_text(argv[3], argv[4]), end="")
        elif cmd == "writejson":
            # writejson <remote> <ssid> <passwd>
            import json
            print(m.write_text(argv[3],
                  json.dumps({"ssid": argv[4], "passwd": argv[5]})), end="")
        elif cmd == "reset":
            m.soft_reset()
        else:
            print("unknown cmd", cmd); return 1
    finally:
        m.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
