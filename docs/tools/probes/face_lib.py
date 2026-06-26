# Animated emotive face library (defined into the agent namespace).
# After exec: call face('happy'), face('sad', blink=True), etc.  10 emotions:
# neutral happy laughing excited love curious surprised sad angry sleepy.
import lcd, image, math
from fpioa_manager import fm
from Maix import GPIO

fm.register(17, fm.fpioa.GPIO6)
GPIO(GPIO.GPIO6, GPIO.OUT).value(0)
lcd.init()
W, H = lcd.width(), lcd.height()

BG    = (14, 16, 28)
EYE   = (236, 240, 255)
PUP   = (28, 40, 92)
GLINT = (255, 255, 255)
CORAL = (224, 122, 90)
CHEEK = (228, 116, 102)
RED   = (235, 70, 70)
BLUE  = (120, 180, 240)
DIM   = (120, 126, 152)
GRN   = (90, 220, 150)

_img = image.Image(size=(W, H))
LX, LY, RX, RY = 110, 102, 210, 102
D2R = 0.017453292


def _arc(cx, cy, r, a0, a1, color, th=4, step=4):
    a = a0
    while a <= a1:
        rad = a * D2R
        _img.draw_circle(int(cx + r * math.cos(rad)),
                         int(cy + r * math.sin(rad)), th, color, fill=True)
        a += step


def _round_eye(cx, cy, r, gx=0, gy=0):
    _img.draw_circle(cx, cy, r, EYE, fill=True)
    _img.draw_circle(cx + gx, cy + gy, int(r * 0.42), PUP, fill=True)
    _img.draw_circle(cx + gx + 6, cy + gy - 6, 5, GLINT, fill=True)


def _smile(cx, cy, r, a0, a1, color, th=5):
    _arc(cx, cy, r, a0, a1, color, th)


def _heart(cx, cy, s, color):
    for dy in range(0, s):
        half = int((s - dy) * 1.1)
        _img.draw_line(cx - half, cy - s // 3 + dy, cx + half, cy - s // 3 + dy, color, thickness=1)
    _img.draw_circle(cx - s // 2, cy - s // 3, s // 2, color, fill=True)
    _img.draw_circle(cx + s // 2, cy - s // 3, s // 2, color, fill=True)


def face(emo, blink=False):
    _img.draw_rectangle(0, 0, W, H, BG, fill=True)
    _img.draw_string(W - 26, 6, "*", CORAL, scale=3)

    if blink:
        _img.draw_rectangle(LX - 42, LY - 4, 84, 8, EYE, fill=True)
        _img.draw_rectangle(RX - 42, RY - 4, 84, 8, EYE, fill=True)
        _smile(160, 138, 56, 35, 145, CORAL); _img.draw_string(8, H - 22, emo, DIM, scale=2)
        lcd.display(_img); return

    if emo == "neutral":
        _round_eye(LX, LY, 42); _round_eye(RX, RY, 42)
        _smile(160, 140, 52, 40, 140, CORAL)
    elif emo == "happy":
        _arc(LX, LY + 8, 38, 202, 338, EYE, 6)        # ^_^ eyes
        _arc(RX, RY + 8, 38, 202, 338, EYE, 6)
        _img.draw_circle(LX - 8, 168, 12, CHEEK, fill=True)
        _img.draw_circle(RX + 8, 168, 12, CHEEK, fill=True)
        _smile(160, 130, 60, 25, 155, CORAL, 6)        # big warm smile
    elif emo == "laughing":
        _arc(LX, LY + 10, 40, 205, 335, EYE, 6)
        _arc(RX, RY + 10, 40, 205, 335, EYE, 6)
        _img.draw_circle(160, 168, 30, CORAL, fill=True)   # open laughing mouth
        _img.draw_circle(160, 160, 26, BG, fill=True)
        _img.draw_rectangle(132, 150, 56, 12, CORAL, fill=True)
    elif emo == "excited":
        _round_eye(LX, LY, 46, 0, -4); _round_eye(RX, RY, 46, 0, -4)
        _arc(LX, LY - 56, 26, 200, 340, GRN, 4)
        _arc(RX, RY - 56, 26, 200, 340, GRN, 4)
        _smile(160, 126, 62, 22, 158, GRN, 6)
    elif emo == "love":
        _heart(LX, LY, 26, RED); _heart(RX, RY, 26, RED)
        _img.draw_circle(LX - 8, 168, 12, CHEEK, fill=True)
        _img.draw_circle(RX + 8, 168, 12, CHEEK, fill=True)
        _smile(160, 132, 58, 26, 154, CORAL, 6)
    elif emo == "curious":
        _round_eye(LX, LY, 42, 10, -4); _round_eye(RX, RY, 44, 10, -4)
        _img.draw_line(LX - 26, LY - 50, LX + 22, LY - 44, DIM, 5)     # one raised brow
        _smile(160, 150, 40, 35, 120, CORAL, 5)
        _img.draw_string(244, 70, "?", GLINT, scale=4)
    elif emo == "surprised":
        _round_eye(LX, LY, 50); _round_eye(RX, RY, 50)
        _arc(LX, LY - 62, 28, 200, 340, DIM, 4)
        _arc(RX, RY - 62, 28, 200, 340, DIM, 4)
        _img.draw_circle(160, 170, 17, CORAL, fill=True)
        _img.draw_circle(160, 170, 11, BG, fill=True)
    elif emo == "sad":
        _round_eye(LX, LY + 6, 38, 0, 8); _round_eye(RX, RY + 6, 38, 0, 8)
        _img.draw_line(LX - 26, LY - 52, LX + 22, LY - 38, DIM, 5)     # inner-up brows
        _img.draw_line(RX + 26, RY - 52, RX - 22, RY - 38, DIM, 5)
        _arc(160, 196, 46, 200, 340, BLUE, 5)          # frown
        _img.draw_circle(LX - 28, LY + 34, 5, BLUE, fill=True)         # tear
    elif emo == "angry":
        _round_eye(LX, LY + 4, 40, 0, 6); _round_eye(RX, RY + 4, 40, 0, 6)
        _img.draw_line(LX - 26, LY - 42, LX + 24, LY - 26, RED, 7)     # inner-down
        _img.draw_line(RX + 26, RY - 42, RX - 24, RY - 26, RED, 7)
        _arc(160, 198, 44, 205, 335, RED, 6)           # frown
    elif emo == "sleepy":
        _arc(LX, LY, 38, 18, 162, DIM, 6)              # half-closed lids
        _arc(RX, RY, 38, 18, 162, DIM, 6)
        _img.draw_string(232, 58, "z", DIM, scale=2)
        _img.draw_string(248, 40, "z", DIM, scale=3)
        _img.draw_circle(160, 172, 7, DIM, fill=True)
    _img.draw_string(8, H - 22, emo, DIM, scale=2)
    lcd.display(_img)


NS = globals()
print("face lib ready: face('<emo>', blink=False)")
