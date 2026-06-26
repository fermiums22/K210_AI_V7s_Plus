import os
# remove any old marker
try:
    os.remove("/flash/MARK")
    print("old MARK removed")
except Exception:
    print("no old MARK")
# write a trivial main.py that just drops a marker on boot
trivial = b"f=open('/flash/MARK','wb')\nf.write(b'booted')\nf.close()\n"
f = open("/flash/main.py", "wb")
f.write(trivial)
f.close()
print("trivial main.py installed:", os.listdir("/flash"))
