# MaixPy idiom: feed play() in a TIGHT loop (no sleep). play() blocks when the
# DMA buffer is full, so a tight loop = continuous output. Test on device 0.
import math, array, time, gc
from Maix import I2S
from fpioa_manager import fm
gc.collect()

fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)

SR = 16000
dev = I2S(I2S.DEVICE_0)
dev.channel_config(dev.CHANNEL_1, I2S.TRANSMITTER,
                   resolution=I2S.RESOLUTION_16_BIT,
                   cycles=I2S.SCLK_CYCLES_32,
                   align_mode=I2S.RIGHT_JUSTIFYING_MODE)
dev.set_sample_rate(SR)

# one 50 ms chunk, looped many times back-to-back
n = SR // 20
chunk = array.array('h', bytearray(n * 2 * 2))
for i in range(n):
    s = int(22000 * math.sin(2 * math.pi * 523 * i / SR))
    chunk[2 * i] = s
    chunk[2 * i + 1] = s

print("TIGHT-LOOP tone ~3s starting")
t_end = time.ticks_ms() + 3000
cnt = 0
while time.ticks_diff(t_end, time.ticks_ms()) > 0:
    dev.play(chunk)     # no sleep: blocks when DMA busy
    cnt += 1
print("DONE, play() calls:", cnt)
