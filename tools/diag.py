import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM20"
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.3
# Do NOT let pyserial toggle the auto-reset lines on open
s.dtr = False
s.rts = False
s.open()
s.dtr = False
s.rts = False
time.sleep(0.2)

def dump(tag, t=0.8):
    t0 = time.time(); buf = b""
    while time.time() - t0 < t:
        d = s.read(256)
        if d:
            buf += d
    print(f"[{tag}] {buf!r}")
    return buf

dump("idle")
s.write(b"\r\n"); dump("after CRLF")
s.write(b"\x03"); dump("after Ctrl-C")
s.write(b"\x03"); dump("after Ctrl-C #2")
s.write(b"\r\n"); dump("after CRLF #2")
s.write(b"print('PONG', 1+1)\r\n"); dump("after print")
s.close()
