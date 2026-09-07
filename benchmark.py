import socket
import time
import statistics

HOST = "127.0.0.1"
PORT = 6379
REQUESTS = 10000

def encode_ping():
    return b"*1\r\n$4\r\nPING\r\n"

def read_response(sock):
    data = b""

    while b"\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("Server closed the connection")
        data += chunk

    return data

with socket.create_connection((HOST, PORT)) as sock:
    request = encode_ping()

    # Warm-up request
    sock.sendall(request)
    read_response(sock)

    latencies = []

    for _ in range(REQUESTS):
        start = time.perf_counter_ns()

        sock.sendall(request)
        response = read_response(sock)

        end = time.perf_counter_ns()

        if not response:
            raise RuntimeError("Empty response")

        latencies.append((end - start) / 1_000_000)

average = statistics.mean(latencies)
median = statistics.median(latencies)
minimum = min(latencies)
maximum = max(latencies)

sorted_latencies = sorted(latencies)
p95 = sorted_latencies[int(REQUESTS * 0.95) - 1]
p99 = sorted_latencies[int(REQUESTS * 0.99) - 1]

print("KoshDB PING Benchmark")
print("---------------------")
print(f"Requests : {REQUESTS}")
print(f"Average  : {average:.3f} ms")
print(f"Median   : {median:.3f} ms")
print(f"Minimum  : {minimum:.3f} ms")
print(f"Maximum  : {maximum:.3f} ms")
print(f"P95      : {p95:.3f} ms")
print(f"P99      : {p99:.3f} ms")
print(f"Throughput: {1000 / average:.2f} requests/sec")