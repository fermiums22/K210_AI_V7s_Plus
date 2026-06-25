# Canonical MaixPy WAV playback via audio.Audio + I2S play_process/play loop.
from fpioa_manager import fm
from Maix import I2S
import audio, gc
gc.collect()

fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)

wav_dev = I2S(I2S.DEVICE_0)
player = audio.Audio(path="/flash/beep.wav")
player.volume(100)
wav_info = player.play_process(wav_dev)
print("wav_info:", wav_info)
wav_dev.channel_config(wav_dev.CHANNEL_1, I2S.TRANSMITTER,
                       resolution=I2S.RESOLUTION_16_BIT,
                       cycles=I2S.SCLK_CYCLES_32,
                       align_mode=I2S.RIGHT_JUSTIFYING_MODE)
wav_dev.set_sample_rate(wav_info[1])

n = 0
while True:
    ret = player.play()
    if ret == None:
        break
    n += 1
player.finish()
print("played chunks:", n, "-- DONE")
