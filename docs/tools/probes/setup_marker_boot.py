import os
try:
    os.remove("/flash/MARK")
    print("old MARK removed")
except Exception:
    print("no old MARK")
trivial = b"f=open('/flash/MARK','wb')\nf.write(b'boot')\nf.close()\n"
f = open("/flash/boot.py", "wb")
f.write(trivial)
f.close()
print("trivial boot.py installed:", os.listdir("/flash"))
