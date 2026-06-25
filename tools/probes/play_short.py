# Play /sd/short.wav through the amp (enable IO9/IO10 during playback).
from fpioa_manager import fm
from Maix import I2S, GPIO
import audio, gc
gc.collect()

# amp control pins
fm.register(9, fm.fpioa.GPIO1)
fm.register(10, fm.fpioa.GPIO2)
mute = GPIO(GPIO.GPIO1, GPIO.OUT)
shdn = GPIO(GPIO.GPIO2, GPIO.OUT)

# audio I2S0
fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)
dev = I2S(I2S.DEVICE_0)

mute.value(1); shdn.value(1)          # amp ON
p = audio.Audio(path="/sd/short.wav")
p.volume(100)
info = p.play_process(dev)
print("wav_info:", info)
dev.channel_config(dev.CHANNEL_1, I2S.TRANSMITTER,
                   resolution=I2S.RESOLUTION_16_BIT,
                   cycles=I2S.SCLK_CYCLES_32,
                   align_mode=I2S.RIGHT_JUSTIFYING_MODE)
dev.set_sample_rate(info[1])
while p.play() is not None:
    pass
p.finish()
mute.value(0); shdn.value(0)          # amp OFF (silent)
print("played short.wav DONE")
