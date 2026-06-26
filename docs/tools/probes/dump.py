print("==== /flash/main.py ====")
try:
    print(open("/flash/main.py").read())
except Exception as e:
    print("err", e)
print("==== /flash/config.json ====")
try:
    print(open("/flash/config.json").read())
except Exception as e:
    print("err", e)
print("==== wifi module probe ====")
for mod in ("network","network_esp32","network_esp8285","esp","esp32","wifi","nm","ucurl","usocket","socket","modbus"):
    try:
        __import__(mod); print("HAVE", mod)
    except Exception as e:
        print("MISS", mod)
