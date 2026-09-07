import socket
import time
import statistics

HOST = "127.0.0.1"
PORT = 6379
REQUESTS = 10000


def encode_command(*args):
    parts = [f"*{len(args)}\r\n".encode()]

    for arg in args:
        value = str(arg).encode()
        parts.append(f"${len(value)}\r\n".encode())
        parts.append(value)
        parts.append(b"\r\n")

    return b"".join(parts)


def read_response(sock):
    data = b""

    while b"\r\n" not in data:
        chunk = sock.recv(4096)

        if not chunk:
            raise ConnectionError("Server closed the connection")

        data += chunk

    return data


def benchmark_operation(sock, request):
    latencies = []

    for _ in range(REQUESTS):
        start = time.perf_counter_ns()

        sock.sendall(request)
        response = read_response(sock)

        end = time.perf_counter_ns()

        if not response:
            raise RuntimeError("Empty response")

        latencies.append((end - start) / 1_000_000)

    latencies.sort()

    average = statistics.mean(latencies)
    median = statistics.median(latencies)
    p95 = latencies[int(REQUESTS * 0.95) - 1]
    p99 = latencies[int(REQUESTS * 0.99) - 1]

    return {
        "average": average,
        "median": median,
        "minimum": min(latencies),
        "maximum": max(latencies),
        "p95": p95,
        "p99": p99,
        "throughput": 1000 / average,
    }


with socket.create_connection((HOST, PORT)) as sock:
    operations = {
        "SET": encode_command("SET", "benchmark_key", "benchmark_value"),
        "GET": encode_command("GET", "benchmark_key"),
        "EXISTS": encode_command("EXISTS", "benchmark_key"),
        "DEL": encode_command("DEL", "benchmark_key"),
    }

    # Ensure the key exists before GET and EXISTS benchmarks.
    sock.sendall(operations["SET"])
    read_response(sock)

    for name, request in operations.items():
        if name == "DEL":
            # Recreate the key before each DEL benchmark.
            sock.sendall(encode_command("SET", "benchmark_key", "benchmark_value"))
            read_response(sock)

        result = benchmark_operation(sock, request)

        print(f"\n{name} Benchmark")
        print("-" * 20)
        print(f"Requests  : {REQUESTS}")
        print(f"Average   : {result['average']:.3f} ms")
        print(f"Median    : {result['median']:.3f} ms")
        print(f"Minimum   : {result['minimum']:.3f} ms")
        print(f"Maximum   : {result['maximum']:.3f} ms")
        print(f"P95       : {result['p95']:.3f} ms")
        print(f"P99       : {result['p99']:.3f} ms")
        print(f"Throughput: {result['throughput']:.2f} requests/sec")