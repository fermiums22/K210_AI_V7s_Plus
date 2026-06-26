import time
from network import ESP8285
from machine import UART
from fpioa_manager import fm
from Maix import GPIO

fm.register(8, fm.fpioa.GPIO0)
en = GPIO(GPIO.GPIO0, GPIO.OUT)
en.value(1)
time.sleep_ms(300)

fm.register(6, fm.fpioa.UART2_RX)
fm.register(7, fm.fpioa.UART2_TX)
uart = UART(UART.UART2, 115200, timeout=1000, read_buf_len=4096)

nic = ESP8285(uart)
print("connecting to wifi ...")
try:
    nic.connect("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD")
except Exception as e:
    print("connect raised:", e)

for _ in range(15):
    if nic.isconnected():
        break
    time.sleep_ms(500)

print("isconnected:", nic.isconnected())
try:
    print("ifconfig:", nic.ifconfig())
except Exception as e:
    print("ifconfig err:", e)
