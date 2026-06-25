import os
print("flash list:", os.listdir("/flash"))
st = os.statvfs("/flash")
print("bsize", st[0], "blocks", st[2], "bfree", st[3], "free_bytes", st[0]*st[3])
try:
    f = open("/flash/zz_test.txt", "w")
    f.write("hello")
    f.close()
    print("write OK, read:", open("/flash/zz_test.txt").read())
    os.remove("/flash/zz_test.txt")
    print("remove OK")
except Exception as e:
    print("WRITE TEST FAIL:", repr(e))
