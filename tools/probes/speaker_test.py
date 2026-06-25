# Speaker test: map I2S0 audio-out pins, init as transmitter, play a clean 440 Hz tone.
# Audio-out pins from /flash/config.json: I2S_WS=33, I2S_DA=34, I2S_BCK=35.
import math, array, time
from Maix import I2S
from fpioa_manager import fm

# Map K210 IO -> I2S0 audio-out signals
fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)
print("pins registered")

SR = 44100
dev = I2S(I2S.DEVICE_0)
dev.channel_config(dev.CHANNEL_1, I2S.TRANSMITTER,
                   resolution=I2S.RESOLUTION_16_BIT,
                   cycles=I2S.SCLK_CYCLES_32,
                   align_mode=I2S.RIGHT_JUSTIFYING_MODE)
dev.set_sample_rate(SR)
print("i2s configured")

# Build ~0.4 s of a 440 Hz sine, interleaved stereo L/R, 16-bit signed.
freq = 440
n = SR // 3          # ~0.33 s
amp = 6000
buf = array.array('h', [0] * (n * 2))
for i in range(n):
    s = int(amp * math.sin(2 * math.pi * freq * i / SR))
    buf[2 * i] = s
    buf[2 * i + 1] = s
print("buffer ready, samples:", n)

for k in range(3):
    dev.play(buf)
    print("played", k)
    time.sleep_ms(120)

# Drive silence then leave pins defined so amp input isn't floating (anti-hiss).
sil = array.array('h', [0] * (n * 2))
dev.play(sil)
print("DONE tone test")
