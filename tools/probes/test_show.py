import sys, gc
sys.path.insert(0, "/sd")
import show
print("show loaded, free", gc.mem_free())
show.perform("happy")
show.perform("angry")
print("perform x2 done, free", gc.mem_free())
