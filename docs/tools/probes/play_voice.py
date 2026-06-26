# Play /sd/meatbags.wav. Correct amp sequencing: configure I2S FIRST, THEN
# enable the amp and wait 200 ms so the turn-on transient doesn't eat the start
# and no setup noise leaks; disable the amp right after.
import time
from fpioa_manager import fm
from Maix import I2S, GPIO
import audio, gc
gc.collect()

fm.register(9, fm.fpioa.GPIO1); fm.register(10, fm.fpioa.GPIO2)
mute = GPIO(GPIO.GPIO1, GPIO.OUT); shdn = GPIO(GPIO.GPIO2, GPIO.OUT)
mute.value(0); shdn.value(0)                 # keep amp OFF during setup

fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)
dev = I2S(I2S.DEVICE_0)

p = audio.Audio(path="/sd/meatbags.wav"); p.volume(85)
info = p.play_process(dev)
print("play_process info:", info)
dev.channel_config(dev.CHANNEL_1, I2S.TRANSMITTER, resolution=I2S.RESOLUTION_16_BIT,
                   cycles=I2S.SCLK_CYCLES_32, align_mode=I2S.RIGHT_JUSTIFYING_MODE)
dev.set_sample_rate(16000)                    # our WAV is 16 kHz; set explicitly

mute.value(1); shdn.value(1)                  # amp ON
time.sleep_ms(200)                            # settle before audio starts
while p.play() is not None:
    pass
p.finish()
time.sleep_ms(60)
mute.value(0); shdn.value(0)                  # amp OFF -> silent
print("played meatbags DONE")
