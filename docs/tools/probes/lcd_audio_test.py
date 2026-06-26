import image, lcd, time, gc, audio
from Maix import I2S, GPIO
from fpioa_manager import fm

lcd.init()
print("lcd disp-ish:", [a for a in dir(lcd) if not a.startswith('_')])

fm.register(9, fm.fpioa.GPIO1); fm.register(10, fm.fpioa.GPIO2)
mute = GPIO(GPIO.GPIO1, GPIO.OUT); shdn = GPIO(GPIO.GPIO2, GPIO.OUT)
fm.register(33, fm.fpioa.I2S0_WS); fm.register(35, fm.fpioa.I2S0_SCLK); fm.register(34, fm.fpioa.I2S0_OUT_D1)
dev = I2S(I2S.DEVICE_0)

sp = image.Image(size=(120, 90))
sp.draw_rectangle(0, 0, 120, 90, (40, 46, 58), fill=True)
sp.draw_circle(60, 45, 22, (90, 235, 150), fill=True)

# find a working positioned/partial display call
variant = None
for name, fn in (("oft", lambda: lcd.display(sp, oft=(100, 150))),
                 ("roi", lambda: lcd.display(sp, roi=(0, 0, 120, 90))),
                 ("xy", lambda: lcd.display(sp, 100, 150)),
                 ("plain", lambda: lcd.display(sp))):
    try:
        fn(); variant = name; print("display variant OK:", name); break
    except Exception as e:
        print("variant", name, "ERR", e)

p = audio.Audio(path="/sd/e_neutral.wav"); p.volume(95); p.play_process(dev)
dev.channel_config(dev.CHANNEL_1, I2S.TRANSMITTER, resolution=I2S.RESOLUTION_16_BIT,
                   cycles=I2S.SCLK_CYCLES_32, align_mode=I2S.RIGHT_JUSTIFYING_MODE)
dev.set_sample_rate(16000)
shdn.value(1); mute.value(1)
t0 = time.ticks_ms(); last = t0; cnt = 0; op = False
while True:
    if p.play() is None:
        break
    now = time.ticks_ms()
    if time.ticks_diff(now, last) >= 110:
        op = not op
        sp.draw_rectangle(0, 0, 120, 90, (40, 46, 58), fill=True)
        if op:
            sp.draw_circle(60, 45, 22, (90, 235, 150), fill=True)
        try:
            lcd.display(sp, oft=(100, 150))
        except Exception:
            lcd.display(sp)
        last = now; cnt += 1
p.finish(); mute.value(0); shdn.value(0)
print("AUDIO DONE, partial displays:", cnt)
