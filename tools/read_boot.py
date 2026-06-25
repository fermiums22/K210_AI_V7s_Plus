"""Open COM port (resets the board) and capture the boot log for N seconds,
so we can watch /flash/main.py auto-run (Wi-Fi connect, IP, etc.)."""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM20"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
s.dtr = False
s.rts = False
time.sleep(0.2)

# trigger a HARD reset so /flash/main.py runs exactly like a power-on
s.reset_input_buffer()
s.write(b"\r\x03")     # interrupt anything running
time.sleep(0.3)
s.write(b"import machine\r\n")
time.sleep(0.2)
s.write(b"machine.reset()\r\n")
time.sleep(0.1)

t0 = time.time()
buf = b""
while time.time() - t0 < secs:
    d = s.read(512)
    if d:
        buf += d
        try:
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
        except Exception:
            pass
s.close()
print("\n----- captured %d bytes in %.0fs -----" % (len(buf), secs))
