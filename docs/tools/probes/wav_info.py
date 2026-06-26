import audio
from Maix import I2S
from fpioa_manager import fm
fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)
dev = I2S(I2S.DEVICE_0)
p = audio.Audio(path="/sd/meatbags.wav")
info = p.play_process(dev)
print("play_process info:", info)
p.finish()
# also read the WAV header bytes directly
f = open("/sd/meatbags.wav", "rb")
h = f.read(44); f.close()
import ustruct
ch = ustruct.unpack("<H", h[22:24])[0]
sr = ustruct.unpack("<I", h[24:28])[0]
bps = ustruct.unpack("<H", h[34:36])[0]
print("header: channels", ch, "rate", sr, "bits", bps)
