"""Compare real RESP2 servers under the same growing, mixed workload."""

from contextlib import contextmanager
from dataclasses import dataclass
import math
from pathlib import Path
import random
import statistics
import subprocess
import sys
import time

from examples.resp_client import RedisClient, encode


TRIALS = 5
BATCH_SIZE = 32
FINAL_KEYS = 15000


@dataclass
class Request:
    command: str
    frame: bytes
    expected: bytes | int | None
    grows: bool = False
    after_growth: bool = False


def workload():
    rng = random.Random(1729)
    entries = {}
    capacity = 16
    requests = []

    def add(command, key, value=None):
        nonlocal capacity
        grows = False
        if command == "SET":
            grows = key not in entries and len(entries) >= capacity * 3 // 4
            if grows:
                capacity *= 2
            entries[key] = value
            expected = b"OK"
            frame = encode(command, key, value)
        else:
            if command == "GET":
                expected = entries.get(key)
            else:
                expected = int(entries.pop(key, None) is not None)
            frame = encode(command, key)
        requests.append(Request(command, frame, expected, grows))
        if grows:
            requests.append(Request("GET", encode("GET", key), value, after_growth=True))

    key = lambda i: f"item:{i:05d}"
    for i in range(FINAL_KEYS):
        value = rng.randbytes(128)
        add("SET", key(i), value)
        add("GET", key(rng.randrange(i + 1)))
        add("GET", key(rng.randrange(i + 2)))
        if i % 4 == 0:
            # Exercise both deletion and re-insertion while the live set grows.
            victim = key(rng.randrange(i + 1))
            previous = entries[victim]
            add("DEL", victim)
            add("GET", victim)
            add("SET", victim, previous)
        if i % 8 == 0:
            add("SET", key(rng.randrange(i + 1)), value)
    return requests


def batches(requests):
    batch = []
    for request in requests:
        # Put growth at the start of its pipeline so its follower shares a send.
        if batch and (len(batch) == BATCH_SIZE or request.grows):
            yield batch
            batch = []
        batch.append(request)
    if batch:
        yield batch


@contextmanager
def server(binary):
    process = subprocess.Popen([Path(binary).resolve(), "127.0.0.1", "0"],
                               stdout=subprocess.PIPE, text=True)
    try:
        line = process.stdout.readline()
        if not line.startswith("Listening on 127.0.0.1:"):
            raise RuntimeError(f"{binary} failed to start: {line.strip()}")
        yield int(line.rsplit(":", 1)[1])
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        process.stdout.close()


def measure(binary, prepared_batches):
    times = {name: [] for name in ("GET", "SET", "DEL", "growth SET", "following GET")}
    with server(binary) as port, RedisClient("127.0.0.1", port) as client:
        if client.command("PING") != b"PONG":
            raise RuntimeError(f"{binary} did not answer PING")
        start = time.perf_counter_ns()
        for batch, payload in prepared_batches:
            sent = time.perf_counter_ns()
            client.socket.sendall(payload)
            for request in batch:
                reply = client.read_reply()
                elapsed = (time.perf_counter_ns() - sent) / 1000
                if reply != request.expected:
                    raise RuntimeError(f"{binary}: unexpected {request.command} reply")
                times[request.command].append(elapsed)
                if request.grows:
                    times["growth SET"].append(elapsed)
                if request.after_growth:
                    times["following GET"].append(elapsed)
        seconds = (time.perf_counter_ns() - start) / 1e9
    return sum(len(times[cmd]) for cmd in ("GET", "SET", "DEL")) / seconds, times


def percentile(ordered, fraction):
    return ordered[math.ceil(len(ordered) * fraction) - 1]


def main(binaries):
    requests = workload()
    prepared = [(batch, b"".join(r.frame for r in batch)) for batch in batches(requests)]
    print(f"{len(requests)} requests/trial, {sum(r.grows for r in requests)} growths/trial, "
          f"up to {BATCH_SIZE} requests/pipeline")
    names = ("list", "linear-array", "sorted-array")
    totals = {name: [] for name in names}
    combined = {name: {} for name in names}
    for trial in range(TRIALS):
        for position in range(len(names)):
            index = (trial + position) % len(names)
            name = names[index]
            throughput, timings = measure(binaries[index], prepared)
            totals[name].append(throughput)
            for command, samples in timings.items():
                combined[name].setdefault(command, []).extend(samples)
    for name in names:
        rates = totals[name]
        print(f"\n{name}: {statistics.median(rates):,.0f} ops/s "
              f"(range {min(rates):,.0f}–{max(rates):,.0f})")
        for command, samples in combined[name].items():
            ordered = sorted(samples)
            print(f"{command:14} n={len(samples):6d}  "
                  f"p95={percentile(ordered, .95):7.1f}  "
                  f"p99={percentile(ordered, .99):7.1f}  max={ordered[-1]:7.1f} µs")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: python -m examples.bench_mixed LIST LINEAR SORTED")
    main(sys.argv[1:])
