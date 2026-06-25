import os
for f in ("/flash/silence.wav", "/flash/beep.wav"):
    try:
        os.remove(f); print("removed", f)
    except Exception as e:
        print("skip", f, e)
st = os.statvfs("/flash")
print("flash list:", os.listdir("/flash"), "free_bytes:", st[0] * st[3])
