# K210: ensure wifi, connect to PC server, exchange a message.
import time, socket
from network import ESP8285
from machine import UART
from fpioa_manager import fm
from Maix import GPIO

PC_IP = "192.168.0.10"
PORT = 9999

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
s.connect((PC_IP, PORT))
print("connected to PC")
data = s.recv(64)
print("recv from PC:", data)
s.send(b"OK_FROM_K210\n")
s.close()
print("TCP TEST DONE")
