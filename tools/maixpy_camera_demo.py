# MaixPy / MicroPython camera smoke test for K210 + GC0328.
# Run on the board after flashing MaixPy firmware.

import lcd
import sensor
import time

lcd.init()
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.run(1)

while True:
    img = sensor.snapshot()
    lcd.display(img)
    time.sleep_ms(10)
