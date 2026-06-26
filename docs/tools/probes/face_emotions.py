# Animated emotive AI face for the 2.4" LCD. Emotions: neutral, happy, sad,
# surprised, angry, sleepy, love. Cycles through them live with labels.
import lcd, image, time, math, gc
from fpioa_manager import fm
from Maix import GPIO

fm.register(17, fm.fpioa.GPIO6)
GPIO(GPIO.GPIO6, GPIO.OUT).value(0)
lcd.init()
W, H = lcd.width(), lcd.height()

BG    = (16, 18, 30)
EYE   = (238, 240, 255)
PUPIL = (32, 44, 96)
GLINT = (255, 255, 255)
CORAL = (217, 119, 87)
CHEEK = (228, 120, 104)
RED   = (235, 70, 70)
BLUE  = (120, 180, 240)
DIM   = (120, 125, 150)

img = image.Image(size=(W, H))
LX, LY, RX, RY = 112, 100, 208, 100


def arc(cx, cy, r, a0, a1, color, th=5):
    prev = None
    a = a0
    while a <= a1:
        rad = a * math.pi / 180
        x = int(cx + r * math.cos(rad)); y = int(cy + r * math.sin(rad))
        if prev:
            img.draw_line(prev[0], prev[1], x, y, color, thickness=th)
        prev = (x, y); a += 8


def round_eye(cx, cy, r, gx=0, gy=0):
    img.draw_circle(cx, cy, r, EYE, fill=True)
    img.draw_circle(cx + gx, cy + gy, r * 4 // 10, PUPIL, fill=True)
    img.draw_circle(cx + gx + 6, cy + gy - 5, 5, GLINT, fill=True)


def brow(cx, y_in, y_out, color=DIM, th=6):
    # one eyebrow: inner end near center, outer end away
    img.draw_line(cx - 26, y_out, cx + 22, y_in, color, th)


def heart(cx, cy, s, color):
    img.draw_circle(cx - s // 2, cy - s // 4, s // 2, color, fill=True)
    img.draw_circle(cx + s // 2, cy - s // 4, s // 2, color, fill=True)
    img.draw_line(cx - s, cy - s // 4, cx, cy + s, color, thickness=2)
    img.draw_line(cx + s, cy - s // 4, cx, cy + s, color, thickness=2)
    for dy in range(0, s):
        half = s - dy
        img.draw_line(cx - half, cy - s // 4 + dy, cx + half, cy - s // 4 + dy, color, thickness=1)


def face(emo, blink=False):
    img.draw_rectangle(0, 0, W, H, BG, fill=True)
    img.draw_string(W - 28, 6, "*", CORAL, scale=3)

    if blink:
        img.draw_rectangle(LX - 44, LY - 4, 88, 8, EYE, fill=True)
        img.draw_rectangle(RX - 44, RY - 4, 88, 8, EYE, fill=True)
        arc(160, 150, 46, 22, 158, CORAL); _label(emo); lcd.display(img); return

    if emo == "neutral":
        round_eye(LX, LY, 44); round_eye(RX, RY, 44)
        arc(160, 150, 44, 30, 150, CORAL)
    elif emo == "happy":
        # happy closed up-arcs ^_^ + big smile + cheeks
        arc(LX, LY + 6, 40, 200, 340, EYE, 7)
        arc(RX, RY + 6, 40, 200, 340, EYE, 7)
        img.draw_circle(LX - 8, 170, 13, CHEEK, fill=True)
        img.draw_circle(RX + 8, 170, 13, CHEEK, fill=True)
        arc(160, 140, 52, 18, 162, CORAL, 6)
    elif emo == "sad":
        round_eye(LX, LY + 6, 40, 0, 8); round_eye(RX, RY + 6, 40, 0, 8)
        brow(LX, LY - 54, LY - 40, DIM)            # inner-up
        brow(RX, RY - 54, RY - 40, DIM); _mirror_last()
        arc(160, 192, 46, 200, 340, BLUE, 6)        # frown
        img.draw_circle(LX - 30, LY + 30, 5, BLUE, fill=True)  # tear
    elif emo == "surprised":
        round_eye(LX, LY, 50); round_eye(RX, RY, 50)
        arc(LX, LY - 60, 30, 200, 340, DIM, 5)       # raised brows
        arc(RX, RY - 60, 30, 200, 340, DIM, 5)
        img.draw_circle(160, 168, 16, CORAL, fill=True)
        img.draw_circle(160, 168, 11, BG, fill=True) # open "O" mouth
    elif emo == "angry":
        img.draw_circle(LX, LY + 4, 40, EYE, fill=True)
        img.draw_circle(RX, RY + 4, 40, EYE, fill=True)
        img.draw_circle(LX, LY + 8, 17, PUPIL, fill=True)
        img.draw_circle(RX, RY + 8, 17, PUPIL, fill=True)
        img.draw_line(LX - 26, LY - 44, LX + 24, LY - 28, RED, 7)   # inner-down
        img.draw_line(RX + 26, RY - 44, RX - 24, RY - 28, RED, 7)
        arc(160, 196, 44, 205, 335, RED, 6)          # frown
    elif emo == "sleepy":
        arc(LX, LY, 40, 20, 160, DIM, 6)             # half-closed (lower lids)
        arc(RX, RY, 40, 20, 160, DIM, 6)
        img.draw_string(228, 60, "z", DIM, scale=2)
        img.draw_string(244, 44, "z", DIM, scale=3)
        img.draw_circle(160, 170, 8, DIM, fill=True)
    elif emo == "love":
        heart(LX, LY, 26, RED); heart(RX, RY, 26, RED)
        img.draw_circle(LX - 8, 170, 13, CHEEK, fill=True)
        img.draw_circle(RX + 8, 170, 13, CHEEK, fill=True)
        arc(160, 140, 52, 18, 162, CORAL, 6)
    _label(emo)
    lcd.display(img)


def _mirror_last():
    pass


def _label(emo):
    img.draw_string(8, H - 22, emo.upper(), (200, 205, 230), scale=2)


order = ["neutral", "happy", "surprised", "sad", "angry", "sleepy", "love"]
for emo in order:
    print("emo:", emo)
    face(emo); time.sleep_ms(1700)
    face(emo, blink=True); time.sleep_ms(140)
    face(emo); time.sleep_ms(700)
    gc.collect()
face("neutral")
print("emotions demo done")
