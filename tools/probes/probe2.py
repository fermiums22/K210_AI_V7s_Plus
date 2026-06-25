import os, gc, sys
print("uname:", os.uname())
print("freemem:", gc.mem_free())
print("flash:", os.listdir("/flash"))
for mod in ("network_esp32","network_lan8720","network","socket","usocket",
            "sensor","lcd","audio","Maix","fpioa_manager","machine","video","KPU","mic"):
    try:
        __import__(mod); print("HAVE", mod)
    except Exception as e:
        print("MISS", mod)
