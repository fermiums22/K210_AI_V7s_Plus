# Speaker test v2: play() is non-blocking (DMA), so sleep long enough for the
# buffer to clock out. Three clear 440 Hz beeps with gaps.
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

dur = 0.4
n = int(SR * dur)
amp = 9000
buf = array.array('h', bytearray(n * 2 * 2))
for i in range(n):
    s = int(amp * math.sin(2 * math.pi * 660 * i / SR))
    buf[2 * i] = s
    buf[2 * i + 1] = s
print("buffer ready")

for k in range(3):
    dev.play(buf)
    print("beep", k)
    time.sleep_ms(int(dur * 1000) + 120)   # let DMA finish this beep
    time.sleep_ms(250)                       # gap
print("DONE")
