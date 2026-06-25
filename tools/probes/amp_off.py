# Immediately silence the PAM8403: drive MUTE (IO9) and SHND (IO10) LOW.
from fpioa_manager import fm
from Maix import GPIO
fm.register(9, fm.fpioa.GPIO1)
fm.register(10, fm.fpioa.GPIO2)
mute = GPIO(GPIO.GPIO1, GPIO.OUT)
shdn = GPIO(GPIO.GPIO2, GPIO.OUT)
mute.value(0)   # MUTE -> GND
shdn.value(0)   # SHUTDOWN -> GND
print("amp MUTE(IO9)=0 SHDN(IO10)=0  -> silenced")
