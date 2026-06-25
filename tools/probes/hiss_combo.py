# PAM8403 has no GPIO mute (MUTE/SHND hardwired high). Hypothesis: the loud hiss
# comes from PT8211's I2S inputs floating when I2S is uninitialized. Keep I2S
# alive and feed silence -> DAC holds clean 0 -> amp should go quiet.
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

# 0.2 s silence buffer
ns = SR // 5
sil = array.array('h', bytearray(ns * 2 * 2))

# loud 0.5 s tone, near full scale
nt = SR // 2
tone = array.array('h', bytearray(nt * 2 * 2))
for i in range(nt):
    s = int(28000 * math.sin(2 * math.pi * 660 * i / SR))
    tone[2 * i] = s
    tone[2 * i + 1] = s

def hold_silence(seconds):
    end = time.ticks_ms() + int(seconds * 1000)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        dev.play(sil)
        time.sleep_ms(190)

print(">>> QUIET PHASE A (5s): should be SILENT now")
hold_silence(5)
print(">>> LOUD BEEP now")
dev.play(tone)
time.sleep_ms(700)
print(">>> QUIET PHASE B (4s): should be SILENT again")
hold_silence(4)
print(">>> DONE (I2S kept alive whole time)")
