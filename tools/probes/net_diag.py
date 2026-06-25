import time, socket
from network import ESP8285
from machine import UART
from fpioa_manager import fm
from Maix import GPIO

fm.register(8, fm.fpioa.GPIO0)
GPIO(GPIO.GPIO0, GPIO.OUT).value(1)
time.sleep_ms(200)
fm.register(6, fm.fpioa.UART2_RX)
fm.register(7, fm.fpioa.UART2_TX)
uart = UART(UART.UART2, 115200, timeout=1000, read_buf_len=4096)
nic = ESP8285(uart)
# Always (re)connect: isconnected() can be True with a stale 0.0.0.0 lease.
nic.connect("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD")
ip = "0.0.0.0"
for _ in range(30):
    ip = nic.ifconfig()[0]
    if ip not in ("0.0.0.0", "", None):
        break
    time.sleep_ms(500)
print("wifi:", nic.isconnected(), "ip:", ip)

targets = [("192.168.0.1", 80), ("192.168.0.10", 9999), ("1.1.1.1", 80)]
for host, port in targets:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((host, port))
        print("OK  ", host, port)
        s.close()
    except Exception as e:
        print("FAIL", host, port, "->", e)
print("NET DIAG DONE")
