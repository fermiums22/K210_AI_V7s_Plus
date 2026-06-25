# K210 as TCP server: wifi up, listen on 8888, greet a client, echo back.
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
if not nic.isconnected():
    nic.connect("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD")
    for _ in range(20):
        if nic.isconnected():
            break
        time.sleep_ms(500)
print("wifi:", nic.isconnected(), nic.ifconfig()[0])

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
print("socket methods:", [a for a in dir(s) if not a.startswith('__')])
s.bind(("0.0.0.0", 8888))
s.listen(1)
print("K210 listening on 8888 ...")
conn, addr = s.accept()
print("client:", addr)
conn.send(b"HELLO_FROM_K210\n")
data = conn.recv(64)
print("recv:", data)
conn.close(); s.close()
print("K210 SERVER TEST DONE")
