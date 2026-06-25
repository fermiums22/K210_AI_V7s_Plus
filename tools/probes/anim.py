# On-board face animator: breathing (vertical bob) + blink, from a single
# pre-rendered JPG per emotion (memory: framebuffer + one image only).
import image, lcd, time, gc, math
for _n in ("_img", "img", "src", "cur", "fb"):
    if _n in globals():
        try:
            del globals()[_n]
        except Exception:
            pass
gc.collect()

W, H = lcd.width(), lcd.height()
fb = image.Image(size=(W, H))
EYE_COL = {"neutral": (70, 200, 255), "happy": (90, 235, 150),
           "laughing": (90, 235, 150), "excited": (120, 230, 255),
           "love": (255, 110, 150), "curious": (120, 210, 255),
           "surprised": (120, 220, 255), "sad": (90, 150, 245),
           "angry": (255, 70, 60), "sleepy": (110, 130, 200)}
cur = None
cur_emo = None


def load_face(emo):
    global cur, cur_emo
    if cur is not None:
        del cur
        gc.collect()
    cur = image.Image("/sd/f_%s.jpg" % emo)
    cur_emo = emo
    gc.collect()


def _blink(s, dx, dy):
    # eye centres in the source image, mapped through the breathing scale
    col = EYE_COL.get(cur_emo, (70, 200, 255))
    for ox in (126, 194):
        ex = int(dx + ox * s)
        ey = int(dy + 126 * s)
        hw = int(34 * s)
        fb.draw_rectangle(ex - hw, ey - int(26 * s), hw * 2, int(52 * s), (42, 48, 60), fill=True)
        fb.draw_line(ex - int(26 * s), ey, ex + int(26 * s), ey, col, thickness=5)


def animate(emo, dur=5.0, amp=0.05, period=3.2):
    # cartoon breathing = gentle squash/stretch (scale), NOT a positional bob
    if emo != cur_emo:
        load_face(emo)
    t0 = time.ticks_ms()
    end = int(dur * 1000)
    while time.ticks_diff(time.ticks_ms(), t0) < end:
        tt = time.ticks_diff(time.ticks_ms(), t0)
        s = 1.0 + amp * (0.5 + 0.5 * math.sin(6.2832 * tt / (period * 1000.0)))
        dx = (W - W * s) / 2.0
        dy = (H - H * s) / 2.0
        fb.draw_rectangle(0, 0, W, H, (8, 10, 16), fill=True)
        fb.draw_image(cur, int(round(dx)), int(round(dy)), s, s)
        if (tt % 3400) < 150:          # blink ~every 3.4s for 150ms
            _blink(s, dx, dy)
        lcd.display(fb)
        time.sleep_ms(45)


NS = globals()
print("anim ready: animate('emo', dur)")
