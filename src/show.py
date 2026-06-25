# V7s_Plus emotive show: cycle 10 emotions forever. Per emotion: while the
# neural-TTS line plays (audio in a background thread) the face alternates
# between two pre-rendered frames -- mouth closed (f_<emo>.jpg) and mouth open
# (f_<emo>_t.jpg) -- so the robot looks like it's talking; then a short
# breathing (squash/stretch) + blink beat, then the next emotion. No long dead
# air after a phrase.
#
# Amp: kept POWERED (SHDN=IO10 high); silenced via MUTE=IO9 only -> instant,
# no power-up/down hiss.
import image, lcd, time, gc, math, _thread
from fpioa_manager import fm
from Maix import I2S, GPIO
import audio

W, H = lcd.width(), lcd.height()
EMOS = ["neutral", "happy", "laughing", "excited", "love",
        "curious", "surprised", "sad", "angry", "sleepy"]
EYE_COL = {"neutral": (70, 200, 255), "happy": (90, 235, 150),
           "laughing": (90, 235, 150), "excited": (120, 230, 255),
           "love": (255, 110, 150), "curious": (120, 210, 255),
           "surprised": (120, 220, 255), "sad": (90, 150, 245),
           "angry": (255, 70, 60), "sleepy": (110, 130, 200)}
POST_BREATHE = 1.6     # seconds of idle breathing after each phrase
TALK_MS = 140          # mouth open/close swap period

fm.register(17, fm.fpioa.GPIO6)
GPIO(GPIO.GPIO6, GPIO.OUT).value(0)
lcd.init()

# amplifier: powered, silence via MUTE only
fm.register(9, fm.fpioa.GPIO1)
fm.register(10, fm.fpioa.GPIO2)
_mute = GPIO(GPIO.GPIO1, GPIO.OUT)
_shdn = GPIO(GPIO.GPIO2, GPIO.OUT)
_shdn.value(1)
_mute.value(0)

fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)
_dev = I2S(I2S.DEVICE_0)

_state = {"talking": False}


def amp(on):
    _mute.value(1 if on else 0)


def _speak(path, max_s=12):
    try:
        p = audio.Audio(path=path)
        p.volume(95)
        p.play_process(_dev)
        _dev.channel_config(_dev.CHANNEL_1, I2S.TRANSMITTER,
                            resolution=I2S.RESOLUTION_16_BIT,
                            cycles=I2S.SCLK_CYCLES_32,
                            align_mode=I2S.RIGHT_JUSTIFYING_MODE)
        _dev.set_sample_rate(16000)
        amp(True)
        time.sleep_ms(120)
        end = time.ticks_ms() + int(max_s * 1000)
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            if p.play() is None:
                break
        p.finish()
    except Exception as e:
        print("speak err:", e)
    finally:
        amp(False)
        _state["talking"] = False


def _breathe(closed, col, dur):
    fb = image.Image(size=(W, H))
    t0 = time.ticks_ms()
    endt = int(dur * 1000)
    while time.ticks_diff(time.ticks_ms(), t0) < endt:
        tt = time.ticks_diff(time.ticks_ms(), t0)
        s = 1.0 + 0.05 * (0.5 + 0.5 * math.sin(6.2832 * tt / 3200.0))
        dx = (W - W * s) / 2.0
        dy = (H - H * s) / 2.0
        fb.draw_rectangle(0, 0, W, H, (8, 10, 16), fill=True)
        fb.draw_image(closed, int(round(dx)), int(round(dy)), s, s)
        if (tt % 3000) < 150:
            for ox in (126, 194):
                ex = int(dx + ox * s)
                ey = int(dy + 126 * s)
                fb.draw_rectangle(ex - int(34 * s), ey - int(26 * s),
                                  int(68 * s), int(52 * s), (42, 48, 60), fill=True)
                fb.draw_line(ex - int(26 * s), ey, ex + int(26 * s), ey, col, thickness=5)
        lcd.display(fb)
        time.sleep_ms(45)
    del fb
    gc.collect()


def perform(emo):
    col = EYE_COL.get(emo, (70, 200, 255))
    closed = image.Image("/sd/f_%s.jpg" % emo)
    talk = image.Image("/sd/f_%s_t.jpg" % emo)
    _state["talking"] = True
    try:
        _thread.start_new_thread(_speak, ("/sd/e_%s.wav" % emo,))
    except Exception as e:
        print("thread err:", e)
        _state["talking"] = False
    # talking: swap mouth-open / mouth-closed by wall clock (robust to slow loop)
    while _state["talking"]:
        lcd.display(talk if (time.ticks_ms() // TALK_MS) % 2 else closed)
        time.sleep_ms(40)
    del talk
    gc.collect()
    # short breathing beat, then return
    _breathe(closed, col, POST_BREATHE)
    del closed
    gc.collect()


def run(loops=None):
    n = 0
    while loops is None or n < loops:
        for emo in EMOS:
            try:
                perform(emo)
            except Exception as e:
                print("perform err:", emo, e)
            gc.collect()
        n += 1


if __name__ == "__main__":
    run()
