import os
print("sd list:", os.listdir("/sd"))
for p in ("/sd/boot.py", "/sd/main.py"):
    try:
        d = open(p).read()
        print("====", p, "len", len(d), "====")
        print(d[:200])
    except Exception as e:
        print(p, "ERR", e)
