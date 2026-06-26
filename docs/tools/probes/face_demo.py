# A friendly AI-agent face on the 2.4" LCD: big eyes with glints, blinking,
# rosy cheeks, a warm coral smile. Live demo (~16 s) then returns.
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

img = image.Image(size=(W, H))
LX, LY, RX, RY, ER = 112, 102, 208, 102, 44


def frame(gaze=0.0, blink=False):
    img.draw_rectangle(0, 0, W, H, BG, fill=True)
    # Claude sparkle, top area
    img.draw_string(W - 28, 8, "*", CORAL, scale=3)
    if blink:
        img.draw_rectangle(LX - ER, LY - 4, 2 * ER, 8, EYE, fill=True)
        img.draw_rectangle(RX - ER, RY - 4, 2 * ER, 8, EYE, fill=True)
    else:
        px = int(gaze * 15)
        img.draw_circle(LX, LY, ER, EYE, fill=True)
        img.draw_circle(RX, RY, ER, EYE, fill=True)
        img.draw_circle(LX + px, LY + 4, 19, PUPIL, fill=True)
        img.draw_circle(RX + px, RY + 4, 19, PUPIL, fill=True)
        img.draw_circle(LX + px + 7, LY - 5, 6, GLINT, fill=True)
        img.draw_circle(RX + px + 7, RY - 5, 6, GLINT, fill=True)
    # cheeks
    img.draw_circle(LX - 6, 168, 13, CHEEK, fill=True)
    img.draw_circle(RX + 6, 168, 13, CHEEK, fill=True)
    # smile (lower arc)
    cx, cy, r = 160, 150, 48
    prev = None
    for a in range(22, 159, 8):
        rad = a * math.pi / 180
        x = int(cx + r * math.cos(rad))
        y = int(cy + r * math.sin(rad))
        if prev:
            img.draw_line(prev[0], prev[1], x, y, CORAL, thickness=5)
        prev = (x, y)
    lcd.display(img)


print("face demo: look")
gazes = [0.0, 1.0, 0.0, -1.0, 0.0]
t_end = time.ticks_ms() + 16000
i = 0
while time.ticks_diff(t_end, time.ticks_ms()) > 0:
    g = gazes[i % len(gazes)]
    frame(gaze=g, blink=False)
    time.sleep_ms(1300)
    frame(gaze=g, blink=True)
    time.sleep_ms(130)
    frame(gaze=g, blink=False)
    time.sleep_ms(90)
    i += 1
    gc.collect()
print("face demo done")
