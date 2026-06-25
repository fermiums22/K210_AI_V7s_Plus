# K210 WiFi receiver: (re)connect wifi, pull files from the PC push-server into
# /sd. Pair with tools/wifi_push.py.
import time, socket, gc
from network import ESP8285
from machine import UART
from fpioa_manager import fm
from Maix import GPIO

PC = "192.168.0.10"
PORT = 8888

fm.register(8, fm.fpioa.GPIO0)
GPIO(GPIO.GPIO0, GPIO.OUT).value(1)
time.sleep_ms(200)
fm.register(6, fm.fpioa.UART2_RX)
fm.register(7, fm.fpioa.UART2_TX)
uart = UART(UART.UART2, 115200, timeout=1000, read_buf_len=4096)
nic = ESP8285(uart)
nic.connect("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD")        # always force a real DHCP lease
ip = "0.0.0.0"
for _ in range(30):
    ip = nic.ifconfig()[0]
    if ip not in ("0.0.0.0", "", None):
        break
    time.sleep_ms(500)
print("ip:", ip)

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((PC, PORT))
buf = b""

def _line():
    global buf
    while b"\n" not in buf:
        d = s.recv(1460)
        if not d:
            raise OSError("closed")
        buf += d
    i = buf.index(b"\n")
    r = buf[:i]; buf = buf[i + 1:]
    return r.decode()

def _take(n):
    global buf
    while len(buf) < n:
        d = s.recv(1460)
        if not d:
            raise OSError("closed")
        buf += d
    r = buf[:n]; buf = buf[n:]
    return r

cnt = 0
while True:
    name = _line()
    if name == "EOF":
        break
    size = int(_line())
    f = open("/sd/" + name, "wb")
    got = 0
    while got < size:
        chunk = _take(2048 if size - got > 2048 else size - got)
        f.write(chunk)
        got += len(chunk)
    f.close()
    cnt += 1
    print("recv", name, got)
    gc.collect()
s.send(b"OK\n")
s.close()
print("RECV DONE", cnt, "files")
