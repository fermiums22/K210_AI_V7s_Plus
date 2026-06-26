import os, gc, sys
print("uname:", os.uname())
print("freemem:", gc.mem_free())
try: print("flash:", os.listdir("/flash"))
except Exception as e: print("flash err", e)
try: print("sd:", os.listdir("/sd"))
except Exception as e: print("no sd:", e)
for mod in ("sensor","lcd","audio","Maix","fpioa_manager","network_esp32","machine"):
    try:
        __import__(mod); print("HAVE", mod)
    except Exception as e:
        print("MISS", mod, "-", e)
