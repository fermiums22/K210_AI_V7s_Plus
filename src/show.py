# V7s_Plus emotive show: cycle 10 emotions forever. While a line plays, the
# audio feed and the mouth-frame swap run in ONE loop (no threads -- threads
# starve the draw loop on this MaixPy), so the mouth alternates closed/open in
# step with the speech. Then a short breathing beat, then the next emotion.
#
# Amp: kept POWERED (SHDN=IO10 high); silenced via MUTE=IO9 only.
# Faces: /sd/f_<emo>.jpg (closed) + /sd/f_<emo>_t.jpg (open).  Voices: /sd/e_<emo>.wav
import image, lcd, time, gc, math
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
POST_BREATHE = 1.4
TALK_MS = 120          # mouth swap period while speaking

fm.register(17, fm.fpioa.GPIO6)
GPIO(GPIO.GPIO6, GPIO.OUT).value(0)
lcd.init()

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


def amp(on):
    _mute.value(1 if on else 0)


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
    try:
        p = audio.Audio(path="/sd/e_%s.wav" % emo)
        p.volume(95)
        p.play_process(_dev)
        _dev.channel_config(_dev.CHANNEL_1, I2S.TRANSMITTER,
                            resolution=I2S.RESOLUTION_16_BIT,
                            cycles=I2S.SCLK_CYCLES_32,
                            align_mode=I2S.RIGHT_JUSTIFYING_MODE)
        _dev.set_sample_rate(16000)
        amp(True)
        time.sleep_ms(120)
        lcd.display(closed)
        last = time.ticks_ms()
        openm = False
        while True:                       # interleave: feed audio + swap mouth
            if p.play() is None:
                break
            now = time.ticks_ms()
            if time.ticks_diff(now, last) >= TALK_MS:
                openm = not openm
                lcd.display(talk if openm else closed)
                last = now
        p.finish()
    except Exception as e:
        print("speak err:", e)
    finally:
        amp(False)
    del talk
    gc.collect()
    lcd.display(closed)                    # mouth shut
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
