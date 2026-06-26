import socket, sys, time
ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.132"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8888
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(15)
print("connecting to", ip, port, flush=True)
s.connect((ip, port))
print("connected", flush=True)
data = s.recv(64)
print("recv from board:", data, flush=True)
s.sendall(b"OK_FROM_PC\n")
s.close()
print("client done", flush=True)
