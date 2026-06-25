# Test if feeding the PT8211 DAC a continuous stream of silence (via the proper
# player) kills the hiss. Make a 4 s silence WAV, then: beep -> 4 s silence.
import ustruct, uos, gc
from fpioa_manager import fm
from Maix import I2S
import audio
gc.collect()

def make_silence(path, sr=16000, dur=4.0):
    datasize = int(sr * dur) * 4
    hdr = (b'RIFF' + ustruct.pack('<I', 36 + datasize) + b'WAVEfmt '
           + ustruct.pack('<IHHIIHH', 16, 1, 2, sr, sr * 4, 4, 16)
           + b'data' + ustruct.pack('<I', datasize))
    f = open(path, 'wb'); f.write(hdr)
    z = bytes(4096); left = datasize
    while left > 0:
        n = 4096 if left > 4096 else left
        f.write(z[:n]); left -= n
    f.close()

try:
    uos.stat('/flash/silence.wav')
except OSError:
    make_silence('/flash/silence.wav')
    print("silence.wav created")

fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)
dev = I2S(I2S.DEVICE_0)

def play(path, vol=100):
    p = audio.Audio(path=path)
    p.volume(vol)
    info = p.play_process(dev)
    dev.channel_config(dev.CHANNEL_1, I2S.TRANSMITTER,
                       resolution=I2S.RESOLUTION_16_BIT,
                       cycles=I2S.SCLK_CYCLES_32,
                       align_mode=I2S.RIGHT_JUSTIFYING_MODE)
    dev.set_sample_rate(info[1])
    while p.play() is not None:
        pass
    p.finish()

print(">>> BEEP")
play('/flash/beep.wav')
print(">>> 4s SILENCE fed to DAC now -- listen if hiss is gone")
play('/flash/silence.wav')
print(">>> DONE")
