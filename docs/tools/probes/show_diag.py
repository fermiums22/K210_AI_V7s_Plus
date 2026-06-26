import sys, gc, time
sys.path.insert(0, "/sd")
import show
for e in ["neutral", "happy", "laughing", "angry"]:
    t = time.ticks_ms()
    try:
        show.perform(e)
        print(e, "ok", time.ticks_diff(time.ticks_ms(), t), "ms free", gc.mem_free())
    except Exception as ex:
        print(e, "ERR", ex)
print("DIAG DONE")
