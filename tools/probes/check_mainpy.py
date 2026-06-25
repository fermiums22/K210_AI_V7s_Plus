import os
print("exists:", "main.py" in os.listdir("/flash"))
data = open("/flash/main.py").read()
print("size:", len(data))
print("--- first 200 chars ---")
print(data[:200])
print("--- running it now ---")
exec(data)
print("--- main.py finished ---")
