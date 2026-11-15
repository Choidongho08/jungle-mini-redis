"""Compare preserved before/after binaries; validate every timed RESP reply.

Run both binaries and the driver inside one Docker dev container. Timings include
the Python client and loopback TCP, not Docker's host port forwarding.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess
import threading
import time

from examples.bench_mixed import server
from examples.resp_client import RedisClient, encode


def percentile(values, fraction=0.95):
    return sorted(values)[math.ceil(len(values) * fraction) - 1]


def workload(kind, commands):
    if kind == "bulk-get":
        return [(encode("GET", "bulk"), b"x" * 4096)] * commands
    requests = []
    value = b"v" * 128
    for i in range(commands // 3):
        key = f"bench:{i % 64}"
        requests.extend(((encode("SET", key, value), b"OK"),
                         (encode("GET", key), value),
                         (encode("DEL", key), 1)))
    return requests


def run_batches(redis, batches, record):
    latencies = []
    started = time.perf_counter_ns()
    for frame, expected in batches:
        batch_start = time.perf_counter_ns()
        redis.socket.sendall(frame)
        for value in expected:
            actual = redis.read_reply()
            if actual != value:
                raise AssertionError((actual, value))
        if record:
            latencies.append((time.perf_counter_ns() - batch_start) / 1000)
    return (time.perf_counter_ns() - started) / 1e9, latencies


def measure(binary, kind, pipeline, commands):
    requests = workload(kind, commands)
    batches = [(b"".join(frame for frame, _ in requests[i:i + pipeline]),
                [expected for _, expected in requests[i:i + pipeline]])
               for i in range(0, len(requests), pipeline)]
    with server(binary) as port, RedisClient("127.0.0.1", port) as redis:
        if redis.command("SET", "bulk", b"x" * 4096) != b"OK":
            raise AssertionError("seed failed")
        run_batches(redis, batches[:min(len(batches), 256)], False)
        seconds, latencies = run_batches(redis, batches, True)
    return {"seconds": seconds, "commands": len(requests),
            "commands_per_second": len(requests) / seconds,
            "batch_p95_us": percentile(latencies)}


def measure_probe(binary, probes=1000):
    """PING latency on a second connection while the first pipelines 4 KiB GETs."""
    with server(binary) as port:
        with RedisClient("127.0.0.1", port) as setup:
            if setup.command("SET", "bulk", b"x" * 4096) != b"OK":
                raise AssertionError("seed failed")
        started = threading.Event()
        stop = threading.Event()

        def load():
            count = 0
            with RedisClient("127.0.0.1", port) as redis:
                frame = encode("GET", "bulk") * 128
                while not stop.is_set():
                    redis.socket.sendall(frame)
                    started.set()
                    for _ in range(128):
                        if redis.read_reply() != b"x" * 4096:
                            raise AssertionError("background GET mismatch")
                    count += 128
            return count

        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(load)
            latencies = []
            try:
                if not started.wait(5):
                    raise TimeoutError("load did not start")
                with RedisClient("127.0.0.1", port) as probe:
                    for _ in range(probes):
                        begin = time.perf_counter_ns()
                        if probe.command("PING") != b"PONG":
                            raise AssertionError("probe mismatch")
                        latencies.append((time.perf_counter_ns() - begin) / 1000)
            finally:
                stop.set()
            completed = future.result()
        return {"probe_p50_us": statistics.median(latencies),
                "probe_p95_us": percentile(latencies), "background_commands": completed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--commands", type=int, default=24576)
    parser.add_argument("--output", type=Path, default=Path("build/pipeline-results.json"))
    args = parser.parse_args()
    if args.trials < 1 or args.commands < 384 or args.commands % 384:
        parser.error("trials must be positive; commands must be a positive multiple of 384")
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    cases = [("mixed", 1), ("mixed", 32), ("mixed", 128), ("bulk-get", 32)]
    raw = {f"{kind}/p{pipeline}": {label: [] for label in binaries}
           for kind, pipeline in cases}
    raw["probe-under-load"] = {label: [] for label in binaries}
    for trial in range(args.trials):
        # 시행마다 측정 순서를 뒤집어 먼저 실행한 쪽으로의 편향을 줄인다.
        labels = list(binaries) if trial % 2 == 0 else list(reversed(binaries))
        for kind, pipeline in cases:
            case = f"{kind}/p{pipeline}"
            for label in labels:
                result = measure(binaries[label], kind, pipeline, args.commands)
                raw[case][label].append(result)
                print(f"trial={trial + 1} {case} {label}: "
                      f"{result['commands_per_second']:.0f} commands/s", flush=True)
        for label in labels:
            result = measure_probe(binaries[label])
            raw["probe-under-load"][label].append(result)
            print(f"trial={trial + 1} probe {label}: "
                  f"p95={result['probe_p95_us']:.1f} us", flush=True)

    summary = {}
    for case, by_label in raw.items():
        summary[case] = {}
        for label, trials in by_label.items():
            summary[case][label] = {
                metric: statistics.median(trial[metric] for trial in trials)
                for metric in trials[0]
            }
        if case != "probe-under-load":
            summary[case]["speedup"] = (summary[case]["after"]["commands_per_second"] /
                                        summary[case]["before"]["commands_per_second"])
    report = {
        "environment": {"platform": platform.platform(), "machine": platform.machine(),
                        "python": platform.python_version(),
                        "compiler": subprocess.check_output(["cc", "--version"], text=True).splitlines()[0]},
        "binaries": {label: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                     for label, path in binaries.items()},
        "trials": args.trials, "commands_per_trial": args.commands,
        "summary": summary, "raw": raw,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print(f"Full report: {args.output}")


if __name__ == "__main__":
    main()
