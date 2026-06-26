# V7s_Plus upper level (K210 Maix Dock M1W) -- boot script.
#
# DEPLOY TO /sd/main.py  (NOT /flash/main.py!). With an SD card inserted MaixPy
# boots with cwd=/sd and auto-runs /sd/main.py; /flash/main.py is ignored.
#
# On power-up:
#   1. show status on the 2.4" LCD
#   2. auto-connect Wi-Fi (ESP8285) using credentials from /flash/wifi.json
#   3. print the IP so it can be reached over the network
#
# Wi-Fi credentials live ONLY on the board in /flash/wifi.json (gitignored),
# never in this file, so the repo stays free of secrets. Format:
#   {"ssid": "...", "passwd": "..."}
#
# Pins (from /flash/config.json, board type "dock"):
#   WIFI_EN=8, WIFI_TX(ESP->K210)=6, WIFI_RX(K210->ESP)=7   -> UART2
import time
import ujson

# --- audio amplifier (PAM8403) control --------------------------------------
# Hardware mod: PAM8403 control pins are cut from their +5V pull-ups and wired
# to spare K210 GPIOs, both active-LOW:
#   MUTE (pin 5)  -> IO9   (LOW = muted)
#   SHND (pin 12) -> IO10  (LOW = shutdown, no hiss / no current)
# Both have a 10k pull-down so the amp powers up SILENT. Firmware raises them
# only while actually playing audio.
AMP_MUTE_PIN = 9
AMP_SHDN_PIN = 10
_amp_mute = None
_amp_shdn = None


def amp_init():
    global _amp_mute, _amp_shdn
    try:
        from fpioa_manager import fm
        from Maix import GPIO
        fm.register(AMP_MUTE_PIN, fm.fpioa.GPIO1)
        fm.register(AMP_SHDN_PIN, fm.fpioa.GPIO2)
        _amp_mute = GPIO(GPIO.GPIO1, GPIO.OUT)
        _amp_shdn = GPIO(GPIO.GPIO2, GPIO.OUT)
        _amp_mute.value(0)            # muted
        _amp_shdn.value(0)            # shutdown -> fully silent at boot
    except Exception as e:
        print("[amp] init:", e)


def amp(on):
    """Enable (True) / silence (False) the speaker amplifier.

    on  -> SHND high (powered) + MUTE high (un-muted)
    off -> both low (shutdown + muted, zero hiss)
    """
    if _amp_shdn is not None:
        _amp_shdn.value(1 if on else 0)
    if _amp_mute is not None:
        _amp_mute.value(1 if on else 0)

SSID = None
PASSWD = None
try:
    with open("/flash/wifi.json") as _f:
        _c = ujson.load(_f)
        SSID, PASSWD = _c.get("ssid"), _c.get("passwd")
except Exception as _e:
    print("[wifi] no /flash/wifi.json:", _e)


_lcd_ready = False


def _lcd_init():
    global _lcd_ready
    if _lcd_ready:
        return True
    try:
        import lcd
        from fpioa_manager import fm
        from Maix import GPIO
        # display enable pin (IO17), as the stock demo does. Use GPIO6 (GPIO7 is
        # already bound to the BOOT key on IO16 and would warn).
        fm.register(17, fm.fpioa.GPIO6)
        GPIO(GPIO.GPIO6, GPIO.OUT).value(0)
        lcd.init()
        _lcd_ready = True
        return True
    except Exception as e:
        print("[lcd] init:", e)
        return False


def _lcd_show(lines, fg, bg=0x0000):
    if not _lcd_init():
        return
    try:
        import lcd
        lcd.clear(bg)
        y = 8
        for ln in lines:
            lcd.draw_string(8, y, ln, fg, bg)
            y += 20
    except Exception as e:
        print("[lcd]", e)


def _real_ip(nic):
    ip = nic.ifconfig()[0]
    return ip if ip not in ("0.0.0.0", "", None) else None


def wifi_connect(ssid, passwd, attempts=3):
    from network import ESP8285
    from machine import UART
    from fpioa_manager import fm
    from Maix import GPIO
    # power-cycle the ESP8285 (EN=IO8) so it cold-boots from any stuck AT state
    fm.register(8, fm.fpioa.GPIO0)
    en = GPIO(GPIO.GPIO0, GPIO.OUT)
    en.value(0)
    time.sleep_ms(300)
    en.value(1)
    time.sleep_ms(1000)
    # UART2 to the ESP8285
    fm.register(6, fm.fpioa.UART2_RX)
    fm.register(7, fm.fpioa.UART2_TX)
    uart = UART(UART.UART2, 115200, timeout=1000, read_buf_len=4096)
    nic = ESP8285(uart)
    for i in range(attempts):
        try:
            nic.connect(ssid, passwd)
        except Exception as e:
            print("[wifi] connect err:", e)
        for _ in range(20):
            if _real_ip(nic):                 # wait for a real DHCP lease
                return nic
            time.sleep_ms(500)
        print("[wifi] retry", i + 1)
    return nic


def main():
    amp_init()                        # keep the amplifier muted at boot
    _lcd_show(["V7s_Plus  K210", "WiFi: connecting..."], 0xFFFF)
    if not SSID:
        print("[wifi] no credentials, skipping")
        _lcd_show(["V7s_Plus  K210", "WiFi: NO CONFIG"], 0xF800)
        return
    nic = None
    try:
        nic = wifi_connect(SSID, PASSWD)
    except Exception as e:
        print("[wifi] fatal:", e)
    if nic and _real_ip(nic):
        ip = nic.ifconfig()[0]
        print("[wifi] connected ip=%s ssid=%s" % (ip, SSID))
        _lcd_show(["V7s_Plus  K210", "WiFi OK", "IP: " + ip, "SSID: " + SSID], 0x07E0)
        # keep a handle around for the REPL / later services
        try:
            import builtins
            builtins.nic = nic
            builtins.amp = amp        # so you can toggle the amp from the REPL
        except Exception:
            pass
    else:
        print("[wifi] FAILED to connect to", SSID)
        _lcd_show(["V7s_Plus  K210", "WiFi FAILED", "SSID: " + str(SSID)], 0xF800)


main()
time.sleep(2.5)          # keep the IP / status screen visible before the show

# Launch the emotive show — cycles 10 emotions (face + voice) forever. This is
# the robot's default idle behaviour on power-up.
try:
    import show
    show.run()
except Exception as _e:
    print("[show] fatal:", _e)
