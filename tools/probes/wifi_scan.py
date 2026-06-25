# Bring up the M1W ESP8285 and scan for APs (no password needed). Pins from
# /flash/config.json: WIFI_EN=8, WIFI_TX=6 (ESP->K210), WIFI_RX=7 (K210->ESP).
import time
from network import ESP8285
from machine import UART
from fpioa_manager import fm
from Maix import GPIO

print("ESP8285 methods:", [a for a in dir(ESP8285) if not a.startswith('__')])

# enable ESP8285
fm.register(8, fm.fpioa.GPIO0)
en = GPIO(GPIO.GPIO0, GPIO.OUT)
en.value(1)
time.sleep_ms(300)

# UART2 <-> ESP8285
fm.register(6, fm.fpioa.UART2_RX)   # ESP TX (IO6) -> K210 RX
fm.register(7, fm.fpioa.UART2_TX)   # K210 TX -> ESP RX (IO7)
uart = UART(UART.UART2, 115200, timeout=1000, read_buf_len=4096)

try:
    nic = ESP8285(uart)
    print("ESP8285 init OK")
except Exception as e:
    print("ESP8285 init FAIL:", e)
    raise

try:
    print("isconnected:", nic.isconnected())
except Exception as e:
    print("isconnected err:", e)

try:
    aps = nic.scan()
    print("scan count:", len(aps))
    for a in aps:
        print("AP:", a)
except Exception as e:
    print("scan err:", e)
