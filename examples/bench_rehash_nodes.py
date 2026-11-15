"""Reproduce bucket-vs-node-budget experiments without changing production files.

Use --phase prepare, test, bench in that order inside the Docker dev service.
Generated snapshots and binaries live under build/rehash-nodes.
"""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import time

from examples.bench_mixed import batches, measure, server, workload
from examples.resp_client import RedisClient, encode


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/rehash_nodes"
FLAGS = ["-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra",
         "-Wpedantic", "-Werror", "-O2"]


def run(command, **kwargs):
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def replace_step(source, replacement):
    function = source.index("static void rehash_step")
    start = source.rfind("\n/*", 0, function) + 1
    end = source.index("StoreResult store_set(", function)
    return source[:start] + replacement + "\n\n" + source[end:]


def compile_fixture(source, snapshot, output):
    run(["cc", *FLAGS, f"-I{snapshot / 'src'}",
         f'-DSTORE_SOURCE="{snapshot / "src/storage/store.c"}"',
         source, "-o", output])


def prepare(output):
    original = (ROOT / "src/storage/store.c").read_text()
    for label, replacement in (("bucket", "bucket.inc"), ("nodes", "candidate.inc")):
        snapshot = output / label / "source"
        snapshot.mkdir(parents=True, exist_ok=True)
        for directory in ("src", "tests", "examples"):
            shutil.copytree(ROOT / directory, snapshot / directory, dirs_exist_ok=True)
        shutil.copy2(ROOT / "Makefile", snapshot / "Makefile")
        (snapshot / "src/storage/store.c").write_text(
            replace_step(original, (FIXTURES / replacement).read_text()))
        binary = output / label / "bin"
        run(["make", f"BUILD_DIR={binary}", "all"], cwd=snapshot)
        compile_fixture(FIXTURES / "bench_store.c", snapshot, binary / "bench_store")
        if label == "nodes":
            compile_fixture(FIXTURES / "test_nodes.c", snapshot, binary / "test_nodes")

        # 긴 체인 TCP 실험용 스냅샷에만 준비 상태 재설정 명령을 추가한다.
        stress = output / label / "stress-source"
        shutil.copytree(snapshot / "src", stress / "src", dirs_exist_ok=True)
        shutil.copy2(ROOT / "Makefile", stress / "Makefile")
        path = stress / "src/storage/store.c"
        path.write_text(path.read_text() + f'''
#include "{FIXTURES / 'fixture.h'}"

void bench_reset_store(Store *store)
{{
    static uint64_t keys[12288];
    static bool ready;
    if (!ready) {{
        collision_keys(keys, 12288, 16384);
        ready = true;
    }}
    Store *replacement = rehash_fixture(keys, 12288, 16384);
    Store old = *store;
    *store = *replacement;
    *replacement = old;
    store_destroy(replacement);
}}
''')
        path = stress / "src/commands/command.c"
        text = path.read_text().replace('#include "commands/command.h"',
                                        '#include "commands/command.h"\n\nvoid bench_reset_store(Store *store);')
        signature = "Reply command_execute(Store *store, const Request *request, int64_t now,\n                         Persistence *persistence)\n{"
        assert signature in text
        text = text.replace(signature, signature + '''
    if (matches(request->args[0], "BENCH_RESET")) {
        bench_reset_store(store);
        return resp_text("+OK\\r\\n");
    }
''')
        path.write_text(text)
        path = stress / "src/server/server.c"
        text = path.read_text()
        assert "#define SWEEP_INTERVAL_MS 100" in text
        path.write_text(text.replace("#define SWEEP_INTERVAL_MS 100", "#define SWEEP_INTERVAL_MS 3600000"))
        run(["make", f"BUILD_DIR={output / label / 'stress-bin'}", "all"], cwd=stress)


def test(output):
    for label in ("bucket", "nodes"):
        snapshot = output / label / "source"
        run(["make", f"BUILD_DIR={output / label / 'bin'}", "test"], cwd=snapshot)
    run([output / "nodes/bin/test_nodes"])


def pct(values, fraction):
    ordered = sorted(values)
    import math
    return ordered[math.ceil(len(ordered) * fraction) - 1]


def stress(binary, samples):
    # absent는 fixture의 충돌 버킷 0과 다르므로 긴 체인 조회 비용을 배제한다.
    hash_value = 14695981039346656037
    for byte in b"absent":
        hash_value = ((hash_value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    assert hash_value & (16384 - 1)
    latency = []
    trigger_latency = []
    with server(binary) as port, RedisClient("127.0.0.1", port) as loader, \
            RedisClient("127.0.0.1", port) as probe:
        # 첫 연결을 먼저 등록해 같은 루프에 준비되면 loader 슬롯부터 실행한다.
        assert loader.command("PING") == b"PONG"
        assert probe.command("PING") == b"PONG"
        trigger = encode("GET", "absent") * 32
        for i in range(samples + 20):
            assert loader.command("BENCH_RESET") == b"OK"
            begin = time.perf_counter_ns()
            loader.socket.sendall(trigger)
            probe_begin = time.perf_counter_ns()
            probe.socket.sendall(encode("PING"))
            assert probe.read_reply() == b"PONG"
            elapsed = (time.perf_counter_ns() - probe_begin) / 1000
            for _ in range(32):
                assert loader.read_reply() is None
            trigger_elapsed = (time.perf_counter_ns() - begin) / 1000
            if i >= 20:
                latency.append(elapsed)
                trigger_latency.append(trigger_elapsed)
    return {"ping_p50_us": statistics.median(latency), "ping_p95_us": pct(latency, .95),
            "ping_p99_us": pct(latency, .99), "ping_max_us": max(latency),
            "trigger_p95_us": pct(trigger_latency, .95), "samples": samples,
            "ping_samples_us": latency}


def bench(output, trials, samples):
    requests = workload()
    prepared = [(batch, b"".join(item.frame for item in batch)) for batch in batches(requests)]
    raw = {"normal": {"bucket": [], "nodes": []}, "stress": {"bucket": [], "nodes": []},
           "micro": {}}
    # 일반 부하의 첫 실행은 예열로 제외한다.
    for label in ("bucket", "nodes"):
        measure(output / label / "bin/mini-redis", prepared)
    for trial in range(trials):
        order = ("bucket", "nodes") if trial % 2 == 0 else ("nodes", "bucket")
        for label in order:
            rate, timings = measure(output / label / "bin/mini-redis", prepared)
            result = {"commands_per_second": rate,
                      "get_p95_us": pct(timings["GET"], .95),
                      "get_p99_us": pct(timings["GET"], .99)}
            raw["normal"][label].append(result)
            print(f"normal trial={trial + 1} {label}: {rate:.0f} commands/s", flush=True)
        for label in order:
            result = stress(output / label / "stress-bin/mini-redis", samples)
            raw["stress"][label].append(result)
            print(f"stress trial={trial + 1} {label}: PING p95={result['ping_p95_us']:.2f} us "
                  f"p99={result['ping_p99_us']:.2f} us", flush=True)
    for label in ("bucket", "nodes"):
        print(f"micro: {label}", flush=True)
        raw["micro"][label] = json.loads(subprocess.check_output(
            [str(output / label / "bin/bench_store")], text=True))
    summary = {"normal": {}, "stress": {}, "micro": {}}
    for kind in ("normal", "stress"):
        for label in ("bucket", "nodes"):
            summary[kind][label] = {
                metric: statistics.median(trial[metric] for trial in raw[kind][label])
                for metric in raw[kind][label][0] if not metric.endswith("samples_us")
            }
    for label in ("bucket", "nodes"):
        summary["micro"][label] = {
            kind: {metric: statistics.median(trial[metric] for trial in trials)
                   for metric in trials[0]}
            for kind, trials in raw["micro"][label].items()
        }
    binaries = {label: {"sha256": hashlib.sha256((output / label / "bin/mini-redis").read_bytes()).hexdigest(),
                        "store_source_sha256": hashlib.sha256((output / label / "source/src/storage/store.c").read_bytes()).hexdigest()}
                for label in ("bucket", "nodes")}
    report = {"environment": {"platform": platform.platform(), "python": platform.python_version(),
                              "compiler": subprocess.check_output(["cc", "--version"], text=True).splitlines()[0]},
              "flags": FLAGS, "normal_commands_per_trial": len(requests), "trials": trials,
              "stress_samples_per_trial": samples, "node_budget": 16,
              "micro_trials": 51, "binaries": binaries, "summary": summary, "raw": raw}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=("prepare", "test", "bench"), required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build/rehash-nodes")
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--stress-samples", type=int, default=300)
    args = parser.parse_args()
    if args.trials < 1 or args.stress_samples < 1:
        parser.error("trials and stress-samples must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.phase == "prepare":
        prepare(output)
    elif args.phase == "test":
        test(output)
    else:
        bench(output, args.trials, args.stress_samples)


if __name__ == "__main__":
    main()
