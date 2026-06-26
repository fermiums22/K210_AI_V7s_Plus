# Diagnose PT8211 path: is I2S.play() blocking? build 0.5s tone, time the call.
import math, array, time
from Maix import I2S
from fpioa_manager import fm

fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)

SR = 44100
dev = I2S(I2S.DEVICE_0)
dev.channel_config(dev.CHANNEL_1, I2S.TRANSMITTER,
                   resolution=I2S.RESOLUTION_16_BIT,
                   cycles=I2S.SCLK_CYCLES_32,
                   align_mode=I2S.RIGHT_JUSTIFYING_MODE)
dev.set_sample_rate(SR)

n = SR // 2            # 0.5 s, stereo int16
amp = 8000
buf = array.array('h', bytearray(n * 2 * 2))   # zero-filled, no giant list
for i in range(n):
    s = int(amp * math.sin(2 * math.pi * 440 * i / SR))
    buf[2 * i] = s
    buf[2 * i + 1] = s

t0 = time.ticks_ms()
r = dev.play(buf)
dt = time.ticks_diff(time.ticks_ms(), t0)
print("play() returned:", r, "took_ms:", dt, "(buffer is 500 ms)")
time.sleep_ms(300)
print("DONE")
