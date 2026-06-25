# K210 WiFi dev-agent: connect to the PC dev_server and serve EXEC/FILE commands
# over WiFi so we can iterate without the serial/COM round-trip.
#
# Start once over COM (background); afterwards drive it from the PC with
# tools/dev_run.py.  Protocol per command (both directions are length-framed):
#   PC -> board : [4-byte BE len][ "EXEC\n"<code>  |  "FILE <name>\n"<bytes>  | "BYE\n" ]
#   board -> PC : [4-byte BE len][ captured stdout / result text ]
import time, socket, gc, sys, uio, ujson
from network import ESP8285
from machine import UART
from fpioa_manager import fm
from Maix import GPIO

PC = "192.168.0.10"
PORT = 8890

# Wi-Fi credentials live on the board in /flash/wifi.json (never in git).
try:
    _c = ujson.load(open("/flash/wifi.json"))
    SSID, PASS = _c["ssid"], _c["passwd"]
except Exception:
    SSID, PASS = "YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD"

fm.register(8, fm.fpioa.GPIO0)
en = GPIO(GPIO.GPIO0, GPIO.OUT)
en.value(0); time.sleep_ms(300); en.value(1); time.sleep_ms(1000)   # cold-boot ESP
fm.register(6, fm.fpioa.UART2_RX)
fm.register(7, fm.fpioa.UART2_TX)
uart = UART(UART.UART2, 115200, timeout=1000, read_buf_len=4096)
nic = ESP8285(uart)
ip = "0.0.0.0"
for _ in range(3):
    try:
        nic.connect(SSID, PASS)
    except Exception as e:
        print("connect err:", e)
    for _ in range(20):
        ip = nic.ifconfig()[0]
        if ip not in ("0.0.0.0", "", None):
            break
        time.sleep_ms(500)
    if ip not in ("0.0.0.0", "", None):
        break
print("dev-agent ip:", ip)

NS = {}   # persistent namespace shared across EXEC commands


def recvn(s, n):
    # The ESP8285 socket returns b'' on recv timeout (idle), which is NOT a
    # close -- keep waiting so the agent can sit idle between commands without
    # reconnect-storming.
    b = b""
    while len(b) < n:
        try:
            d = s.recv(n - len(b) if n - len(b) < 2048 else 2048)
        except OSError:
            time.sleep_ms(40)
            continue
        if d:
            b += d
        else:
            time.sleep_ms(20)
    return b


def send_msg(s, data):
    s.send(len(data).to_bytes(4, "big"))
    mv = memoryview(data)
    off = 0
    while off < len(data):
        off += s.send(mv[off:off + 2048])


def do_exec(code, s):
    out = []
    def rprint(*a, **k):
        out.append(k.get("sep", " ").join(str(x) for x in a) + k.get("end", "\n"))
    NS["print"] = rprint
    NS["send"] = lambda m: out.append(str(m) + "\n")
    NS["sock"] = s
    try:
        exec(code.decode(), NS)
    except Exception as e:
        es = uio.StringIO()
        sys.print_exception(e, es)
        out.append(es.getvalue())
    return ("".join(out)).encode()


while True:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((PC, PORT))
        send_msg(s, ("READY " + ip).encode())
        while True:
            n = int.from_bytes(recvn(s, 4), "big")
            head = recvn(s, n)                       # framed header
            nl = head.index(b"\n") if b"\n" in head else len(head)
            line = head[:nl].decode()
            if line == "BYE":
                s.close()
                raise SystemExit
            if line.startswith("FILE "):
                _, name, size = line.split(" ")
                size = int(size)
                f = open("/sd/" + name, "wb")
                got = 0
                while got < size:                    # STREAM to disk, no big buffer
                    need = size - got
                    chunk = recvn(s, 4096 if need > 4096 else need)
                    f.write(chunk)
                    got += len(chunk)
                f.close()
                send_msg(s, ("ok wrote %d to /sd/%s" % (got, name)).encode())
            else:                                    # EXEC: code follows the newline
                resp = do_exec(head[nl + 1:], s)
                send_msg(s, resp)
            gc.collect()
    except SystemExit:
        break
    except Exception as e:
        try:
            s.close()
        except Exception:
            pass
        time.sleep(2)   # lost connection -> retry
print("dev-agent stopped")
