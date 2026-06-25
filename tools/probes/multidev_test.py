# Find which I2S device drives the PT8211 DAC. Mic is "MIC0" (I2S0), so the
# speaker DAC is probably on a different device. Beep each device 2,1,0 in turn.
import math, array, time, gc
from Maix import I2S
from fpioa_manager import fm
gc.collect()

SR = 44100
nt = int(SR * 0.5)
tone = array.array('h', bytearray(nt * 2 * 2))
for i in range(nt):
    s = int(28000 * math.sin(2 * math.pi * 660 * i / SR))
    tone[2 * i] = s
    tone[2 * i + 1] = s
print("tone ready, free:", gc.mem_free())

def test(devnum):
    fm.register(35, getattr(fm.fpioa, 'I2S%d_SCLK' % devnum))
    fm.register(33, getattr(fm.fpioa, 'I2S%d_WS' % devnum))
    fm.register(34, getattr(fm.fpioa, 'I2S%d_OUT_D1' % devnum))
    dev = I2S(getattr(I2S, 'DEVICE_%d' % devnum))
    dev.channel_config(dev.CHANNEL_1, I2S.TRANSMITTER,
                       resolution=I2S.RESOLUTION_16_BIT,
                       cycles=I2S.SCLK_CYCLES_32,
                       align_mode=I2S.RIGHT_JUSTIFYING_MODE)
    dev.set_sample_rate(SR)
    print("=== DEVICE %d : beeping ~1.5s ===" % devnum)
    end = time.ticks_ms() + 1500
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        dev.play(tone)
        time.sleep_ms(120)
    del dev
    gc.collect()
    time.sleep_ms(900)   # silent gap between devices

for n in (2, 1, 0):
    try:
        test(n)
    except Exception as e:
        print("device", n, "ERROR:", e)
print("DONE")
