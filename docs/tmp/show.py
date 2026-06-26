# V7s_Plus emotive show. The mouth and the amplifier are driven by the audio
# AMPLITUDE: loud -> mouth open, quiet -> mouth closed; the amp is un-muted only
# while there is sound and muted after ~100 ms of silence (no hiss). Right now
# the amplitude comes from a pre-computed envelope of each file; the SAME logic
# will run on the live wifi audio stream from the server later.
#
# Amp pins (active-low mod): MUTE=IO9, SHDN=IO10.
# Faces: /sd/f_<emo>.jpg (closed) + /sd/f_<emo>_t.jpg (open). Voices: /sd/e_<emo>.wav
import image, lcd, time, gc, math
from fpioa_manager import fm
from Maix import I2S, GPIO
import audio
try:
    from envelopes import ENV, WIN_MS
except Exception:
    ENV, WIN_MS = {}, 40

W, H = lcd.width(), lcd.height()
EMOS = ["neutral", "happy", "laughing", "excited", "love",
        "curious", "surprised", "sad", "angry", "sleepy"]
EYE_COL = {"neutral": (70, 200, 255), "happy": (90, 235, 150),
           "laughing": (90, 235, 150), "excited": (120, 230, 255),
           "love": (255, 110, 150), "curious": (120, 210, 255),
           "surprised": (120, 220, 255), "sad": (90, 150, 245),
           "angry": (255, 70, 60), "sleepy": (110, 130, 200)}
POST_BREATHE = 1.4
GATE_LVL = 2          # amplitude below this counts as silence
MOUTH_LVL = 4         # amplitude at/above this opens the mouth
GATE_MS = 100         # mute after this much continuous silence
# Mouth region (x, y, w, h) inside the 320x240 face. Only THIS rectangle is
# repainted while audio plays -> tiny SPI/DMA transfer that does not starve the
# I2S audio DMA, so play() never hangs. Tune to match the art if needed.
MOUTH = (100, 150, 120, 90)

fm.register(17, fm.fpioa.GPIO6)
GPIO(GPIO.GPIO6, GPIO.OUT).value(0)
lcd.init()

# Positioned partial-display call form, detected LAZILY on first real use (never
# at import -> module load does not touch the LCD). We need to blit a small
# sub-image at an offset: lcd.display(img, oft=(x,y)) on most MaixPy builds,
# else the positional lcd.display(img, x, y). If neither works _DISP stays None
# and the mouth simply does not animate (face was already drawn closed).
_DISP = None
_DISP_TRIED = False


def _blit(img, x, y):
    global _DISP, _DISP_TRIED
    if not _DISP_TRIED:
        _DISP_TRIED = True
        for nm, fn in (("oft", lambda: lcd.display(img, oft=(x, y))),
                       ("xy",  lambda: lcd.display(img, x, y))):
            try:
                fn()
                _DISP = nm
                return
            except Exception:
                pass
        return
    if _DISP == "oft":
        lcd.display(img, oft=(x, y))
    elif _DISP == "xy":
        lcd.display(img, x, y)

fm.register(9, fm.fpioa.GPIO1)
fm.register(10, fm.fpioa.GPIO2)
_mute = GPIO(GPIO.GPIO1, GPIO.OUT)   # active-low: 1=unmuted, 0=muted
_shdn = GPIO(GPIO.GPIO2, GPIO.OUT)   # active-low: 1=powered, 0=shutdown
_mute.value(0)
_shdn.value(0)

fm.register(33, fm.fpioa.I2S0_WS)
fm.register(35, fm.fpioa.I2S0_SCLK)
fm.register(34, fm.fpioa.I2S0_OUT_D1)
_dev = I2S(I2S.DEVICE_0)


def _levels(emo):
    return [int(c, 16) for c in ENV.get(emo, "")]


def _breathe(closed, col, dur):
    fb = image.Image(size=(W, H))
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < int(dur * 1000):
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
    lv = _levels(emo)
    try:
        p = audio.Audio(path="/sd/e_%s.wav" % emo)
        p.volume(95)
        p.play_process(_dev)
        _dev.channel_config(_dev.CHANNEL_1, I2S.TRANSMITTER,
                            resolution=I2S.RESOLUTION_16_BIT,
                            cycles=I2S.SCLK_CYCLES_32,
                            align_mode=I2S.RIGHT_JUSTIFYING_MODE)
        _dev.set_sample_rate(16000)
        _shdn.value(1)            # power the amp
        _mute.value(0)            # start muted; the gate un-mutes on sound
        time.sleep_ms(80)
        # Draw the full closed face ONCE (no audio in flight yet -> safe to do a
        # full-frame transfer). After this, the audio loop only repaints the
        # small MOUTH rectangle, which is a short SPI/DMA burst that does not
        # starve the I2S audio DMA -> play() keeps feeding without hanging.
        lcd.display(closed)
        mx, my = MOUTH[0], MOUTH[1]
        cm = closed.copy(roi=MOUTH)   # closed-mouth crop
        om = talk.copy(roi=MOUTH)     # open-mouth crop
        openm = False
        t0 = time.ticks_ms()
        quiet_since = t0
        while True:
            if p.play() is None:       # tight feed -> no DMA starvation
                break
            now = time.ticks_ms()
            idx = time.ticks_diff(now, t0) // WIN_MS
            lvl = lv[idx] if idx < len(lv) else 0
            # amp noise gate (GPIO only)
            if lvl >= GATE_LVL:
                _mute.value(1)
                quiet_since = now
            elif time.ticks_diff(now, quiet_since) >= GATE_MS:
                _mute.value(0)
            # amplitude-driven mouth: repaint only the mouth rect, only on change
            want = lvl >= MOUTH_LVL
            if want != openm:
                openm = want
                _blit(om if want else cm, mx, my)
        p.finish()
    except Exception as e:
        print("speak err:", e)
    finally:
        _mute.value(0)
        _shdn.value(0)            # full silence between phrases
    del talk
    gc.collect()
    lcd.display(closed)
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
