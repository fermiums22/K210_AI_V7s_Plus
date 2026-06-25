import os
try:
    os.mount(__import__('machine'), '/sd')  # may already be auto-mounted
except Exception:
    pass
for path in ('/sd', '/flash'):
    try:
        st = os.statvfs(path)
        free = st[0] * st[3]
        print(path, "OK  free_bytes:", free, "files:", os.listdir(path))
    except Exception as e:
        print(path, "FAIL:", e)
