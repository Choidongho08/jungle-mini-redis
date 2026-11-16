"""Compare HTTP latency with and without a warmed Mini Redis cache (run cache_api first)."""

import argparse
import http.client
import math
import statistics
import time
import uuid


def request(connection, path):
    start = time.perf_counter()
    connection.request("GET", path)
    response = connection.getresponse()
    body = response.read()
    if response.status != 200:
        raise RuntimeError(f"HTTP {response.status}: {body!r}")
    return (time.perf_counter() - start) * 1000, response.getheader("X-Cache"), body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--requests", type=int, default=100)
    args = parser.parse_args()
    if args.requests < 1:
        parser.error("--requests must be positive")

    item = uuid.uuid4().hex
    uncached = f"/uncached/{item}"
    cached = f"/cached/{item}"
    connection = http.client.HTTPConnection("127.0.0.1", args.port, timeout=5)
    try:
        _, _, expected = request(connection, uncached)
        cold_ms, state, body = request(connection, cached)
        if state != "MISS" or body != expected:
            raise RuntimeError("Unexpected cache warmup response")

        timings = {"BYPASS": [], "HIT": []}
        for _ in range(args.requests):
            for path, expected_state in ((uncached, "BYPASS"), (cached, "HIT")):
                elapsed, state, body = request(connection, path)
                if state != expected_state or body != expected:
                    raise RuntimeError(f"Unexpected {path} response: {state}")
                timings[state].append(elapsed)

        print(f"Cold MISS: {cold_ms:.2f} ms")
        for state, samples in timings.items():
            p95 = sorted(samples)[math.ceil(len(samples) * 0.95) - 1]
            print(f"{state:6} n={len(samples):3} median={statistics.median(samples):.2f} ms "
                  f"p95={p95:.2f} ms")
    finally:
        connection.close()


if __name__ == "__main__":
    main()
