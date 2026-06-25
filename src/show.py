# V7s_Plus emotive show: cycle 10 emotions forever. Each emotion: robot face
# with breathing (squash/stretch) + blinking, a moving mouth while the voice
# line plays (audio runs in a background thread so the mouth animates in sync),
# then idle-breathe for the rest of the ~7 s slot.
#
# Amp: per the hardware mod the amplifier is left POWERED (SHDN=IO10 high) and
# we silence only via MUTE=IO9 (active-low) -> instant, no power-up/down hiss.
#
# Faces: /sd/f_<emo>.jpg   Voices: /sd/e_<emo>.wav
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
SLOT_S = 7.0

fm.register(17, fm.fpioa.GPIO6)
GPIO(GPIO.GPIO6, GPIO.OUT).value(0)
lcd.init()

# amplifier: keep powered, silence with MUTE only
fm.register(9, fm.fpioa.GPIO1)     # MUTE  (active low)
fm.register(10, fm.fpioa.GPIO2)    # SHDN  (kept high = powered)
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
    _mute.value(1 if on else 0)    # SHDN stays high; toggle MUTE only


def _speak(path, max_s=10):
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
        amp(False)                 # instant mute right after the phrase
        _state["talking"] = False


def _frame(fb, img, col, tt, talking):
    s = 1.0 + 0.05 * (0.5 + 0.5 * math.sin(6.2832 * tt / 3200.0))   # breathing
    dx = (W - W * s) / 2.0
    dy = (H - H * s) / 2.0
    fb.draw_rectangle(0, 0, W, H, (8, 10, 16), fill=True)
    fb.draw_image(img, int(round(dx)), int(round(dy)), s, s)
    # blink ~every 3.4 s
    if (not talking) and (tt % 3400) < 150:
        for ox in (126, 194):
            ex = int(dx + ox * s)
            ey = int(dy + 126 * s)
            fb.draw_rectangle(ex - int(34 * s), ey - int(26 * s),
                              int(68 * s), int(52 * s), (42, 48, 60), fill=True)
            fb.draw_line(ex - int(26 * s), ey, ex + int(26 * s), ey, col, thickness=5)
    # talking mouth: cover the static mouth, draw an opening/closing bar
    if talking:
        mx = int(dx + 160 * s)
        my = int(dy + 184 * s)
        fb.draw_rectangle(mx - int(48 * s), my - int(24 * s),
                          int(96 * s), int(48 * s), (34, 39, 52), fill=True)
        mh = int((3 + 9 * abs(math.sin(tt / 70.0))) * s)
        fb.draw_rectangle(mx - int(24 * s), my - mh, int(48 * s), 2 * mh, col, fill=True)
    lcd.display(fb)


def perform(emo):
    col = EYE_COL.get(emo, (70, 200, 255))
    img = image.Image("/sd/f_%s.jpg" % emo)
    fb = image.Image(size=(W, H))
    _state["talking"] = True
    try:
        _thread.start_new_thread(_speak, ("/sd/e_%s.wav" % emo,))
    except Exception as e:
        print("thread err:", e)
        _state["talking"] = False
    t0 = time.ticks_ms()
    while _state["talking"] or time.ticks_diff(time.ticks_ms(), t0) < int(SLOT_S * 1000):
        tt = time.ticks_diff(time.ticks_ms(), t0)
        _frame(fb, img, col, tt, _state["talking"])
        time.sleep_ms(40)
    del img
    del fb
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
