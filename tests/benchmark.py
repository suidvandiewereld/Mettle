import threading
import time
import socket

HOST = "127.0.0.1"
PORT = 5000
PATH = "/"
THREADS = 1
DURATION = 30

request = f"GET {PATH} HTTP/1.1\r\nHost: {HOST}\r\nConnection: close\r\n\r\n".encode()

total_requests = 0
lock = threading.Lock()
stop = False

SOCKET_TIMEOUT = 10

def worker():
    global total_requests
    while not stop:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(SOCKET_TIMEOUT)
            s.connect((HOST, PORT))
            s.sendall(request)
            while True:
                data = s.recv(4096)
                if not data:
                    break
            s.close()
            with lock:
                total_requests += 1
        except (socket.timeout, OSError):
            pass
        except Exception:
            pass

threads = []

for _ in range(THREADS):
    t = threading.Thread(target=worker)
    t.start()
    threads.append(t)

start = time.time()
time.sleep(DURATION)
stop = True

for t in threads:
    t.join()

elapsed = time.time() - start

print("Total Requests:", total_requests)
print("Requests/sec:", total_requests / elapsed)