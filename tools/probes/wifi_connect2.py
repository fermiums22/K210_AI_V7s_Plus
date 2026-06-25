# Robust wifi: power-cycle the ESP8285 (EN=IO8 low->high) before connecting,
# then wait for a REAL DHCP IP (not a stale 0.0.0.0).
import time
from network import ESP8285
from machine import UART
from fpioa_manager import fm
from Maix import GPIO

fm.register(8, fm.fpioa.GPIO0)
en = GPIO(GPIO.GPIO0, GPIO.OUT)
en.value(0)               # disable ESP
time.sleep_ms(300)
en.value(1)               # enable -> ESP cold boots
time.sleep_ms(1000)

fm.register(6, fm.fpioa.UART2_RX)
fm.register(7, fm.fpioa.UART2_TX)
uart = UART(UART.UART2, 115200, timeout=1000, read_buf_len=4096)
nic = ESP8285(uart)

ip = "0.0.0.0"
for attempt in range(3):
    try:
        nic.connect("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD")
    except Exception as e:
        print("connect err:", e)
    for _ in range(20):
        ip = nic.ifconfig()[0]
        if ip not in ("0.0.0.0", "", None):
            break
        time.sleep_ms(500)
    if ip not in ("0.0.0.0", "", None):
        break
    print("retry", attempt + 1)

print("RESULT ip:", ip, "connected:", nic.isconnected())
