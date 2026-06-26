import os
print("flash:", os.listdir("/flash"))
for p in ("/flash/boot.py", "/flash/main.py"):
    try:
        d = open(p).read()
        print("====", p, "len", len(d), "====")
        print(d[:120])
    except Exception as e:
        print(p, "ERR", e)
