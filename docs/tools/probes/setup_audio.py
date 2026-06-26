# Define persistent audio helpers in the agent namespace: amp(on) and
# play(path, rate, vol). Run once via dev_run exec; reuse across commands.
import time
from fpioa_manager import fm
from Maix import I2S, GPIO
import audio

fm.register(9, fm.fpioa.GPIO1)
fm.register(10, fm.fpioa.GPIO2)
_mute = GPIO(GPIO.GPIO1, GPIO.OUT)
_shdn = GPIO(GPIO.GPIO2, GPIO.OUT)
_mute.value(0)
_shdn.value(0)

fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)
_dev = I2S(I2S.DEVICE_0)


def amp(on):
    _mute.value(1 if on else 0)
    _shdn.value(1 if on else 0)


def play(path, rate=16000, vol=85, max_s=12):
    # Bounded + guaranteed amp-off: the while loop can never hang the board, and
    # the amplifier is ALWAYS silenced afterwards (finally), so no leftover hiss.
    p = audio.Audio(path=path)
    p.volume(vol)
    info = p.play_process(_dev)
    _dev.channel_config(_dev.CHANNEL_1, I2S.TRANSMITTER,
                        resolution=I2S.RESOLUTION_16_BIT,
                        cycles=I2S.SCLK_CYCLES_32,
                        align_mode=I2S.RIGHT_JUSTIFYING_MODE)
    _dev.set_sample_rate(rate)
    amp(True)
    time.sleep_ms(200)
    t_end = time.ticks_ms() + int(max_s * 1000)
    try:
        while time.ticks_diff(t_end, time.ticks_ms()) > 0:
            if p.play() is None:
                break
    finally:
        try:
            p.finish()
        except Exception:
            pass
        time.sleep_ms(60)
        amp(False)                     # <- always off after playback
    return info


# expose into the shared namespace
NS = globals()
print("audio helpers ready: amp(on), play(path,rate,vol)")
