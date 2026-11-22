#!/usr/bin/env python3
"""Run the process-level benchmark matrix.

Timing is measured in fresh processes, with binaries shuffled inside each round
so page-cache and turbo drift do not systematically favour the first one.
Peak RSS is measured in a separate `/usr/bin/time -v` pass.

Results are append-only JSONL with an environment fingerprint. A different CPU,
kernel, binary hash or dataset set refuses to append to an existing file.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
TIME_BIN = shutil.which("/usr/bin/time") or "/usr/bin/time"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def cpu_flags() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("flags"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return ""


def read_first(path: str) -> str | None:
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def environment_snapshot(binaries: dict[str, Path], dataset_index: Path) -> dict[str, Any]:
    return {
        "cpu": cpu_model(),
        "cpu_flags_sha256": hashlib.sha256(cpu_flags().encode()).hexdigest(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "nproc": os.cpu_count(),
        "governor": read_first("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "dataset_index_sha256": sha256_file(dataset_index) if dataset_index.exists() else None,
        "binaries": {name: {"path": str(path), "sha256": sha256_file(path)} for name, path in binaries.items()},
    }


def fingerprint(snapshot: dict[str, Any]) -> str:
    canonical = json.dumps(snapshot, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(canonical).hexdigest()


def frequencies() -> dict[str, Any]:
    values: list[int] = []
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("cpu MHz"):
                try:
                    values.append(int(float(line.split(":", 1)[1].strip())))
                except ValueError:
                    pass
    except OSError:
        pass
    temps: list[float] = []
    for zone in sorted(Path("/sys/class/thermal").glob("thermal_zone*/temp")):
        try:
            temps.append(int(zone.read_text().strip()) / 1000.0)
        except (OSError, ValueError):
            pass
    return {
        "mhz_min": min(values) if values else None,
        "mhz_median": statistics.median(values) if values else None,
        "temp_max_c": max(temps) if temps else None,
        "loadavg": list(os.getloadavg()),
    }


def drop_cache(path: Path) -> bool:
    try:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)
        return True
    except (OSError, AttributeError):
        return False


def manifest_rows(manifests: Path, dataset: str) -> tuple[int, int] | None:
    path = manifests / f"{dataset}.json"
    if not path.exists():
        return None
    data = json.loads(path.read_text())
    return int(data["rows"]), int(data["bytes"])


def run_process(binary: Path, argv: list[str], cwd: Path, timeout: float,
                env: dict[str, str], stdout_path: Path | None = None) -> dict[str, Any]:
    started = time.perf_counter()
    stdout = stdout_path.open("wb") if stdout_path else subprocess.DEVNULL
    try:
        completed = subprocess.run(
            [str(binary)] + argv,
            cwd=cwd,
            stdout=stdout,
            stderr=subprocess.PIPE,
            timeout=timeout,
            env=env,
        )
    finally:
        if stdout_path:
            stdout.close()
    return {
        "wall_s": time.perf_counter() - started,
        "exit": completed.returncode,
        "stderr": completed.stderr.decode("utf-8", "replace")[:400],
    }


def measure_rss(binary: Path, argv: list[str], cwd: Path, timeout: float,
                env: dict[str, str], runs: int) -> dict[str, Any]:
    samples: list[int] = []
    walls: list[float] = []
    for _ in range(runs):
        with tempfile.NamedTemporaryFile("r", suffix=".time", delete=False) as handle:
            stats_path = Path(handle.name)
        try:
            result = subprocess.run(
                [TIME_BIN, "-v", "-o", str(stats_path), str(binary)] + argv,
                cwd=cwd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=timeout,
                env=env,
            )
            text = stats_path.read_text(errors="replace")
        finally:
            stats_path.unlink(missing_ok=True)
        wall = None
        rss = None
        for line in text.splitlines():
            if "Maximum resident set size (kbytes):" in line:
                rss = int(line.rsplit(":", 1)[1].strip())
            elif line.startswith("Elapsed (wall clock) time"):
                wall = line
        if result.returncode != 0 or rss is None:
            raise RuntimeError(f"memory run failed for {binary} {argv}: exit={result.returncode}")
        samples.append(rss)
        if wall:
            walls.append(wall)
    return {"rss_kb_median": statistics.median(samples), "rss_kb_min": min(samples),
            "rss_kb_max": max(samples), "samples": samples}


@dataclass
class Case:
    id: str
    benchmark: str
    command: str
    dataset: str
    argv: list[str]
    status: str
    copy: list[str]
    profiles: list[str]
    prepare: str | None
    scaling: bool


def load_matrix(path: Path) -> list[Case]:
    raw = json.loads(path.read_text())
    cases = []
    for item in raw.get("cases", []):
        cases.append(
            Case(
                id=item["id"],
                benchmark=item["benchmark"],
                command=item["command"],
                dataset=item["dataset"],
                argv=list(item["argv"]),
                status=item.get("status", "implemented"),
                copy=list(item.get("copy", [])),
                profiles=list(item.get("profiles", ["smoke", "standard", "full"])),
                prepare=item.get("prepare"),
                scaling=bool(item.get("scaling", False)),
            )
        )
    return cases


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, default=ROOT / "evaluation/performance/matrix.json")
    parser.add_argument("--data", type=Path, default=ROOT / "datasets/data")
    parser.add_argument("--manifests", type=Path, default=ROOT / "datasets/manifests")
    parser.add_argument("--rust-bin", type=Path, required=True)
    parser.add_argument("--cpp-variant", action="append", default=[],
                        help="variant=path (repeatable); default: base=build/base/xsv")
    parser.add_argument("--runs", type=int, default=None)
    parser.add_argument("--warmup", type=int, default=None)
    parser.add_argument("--profile", choices=("smoke", "standard"), default="smoke")
    parser.add_argument("--seed", type=int, default=20261120)
    parser.add_argument("--cold", action="store_true", help="fadvise DONTNEED before each run")
    parser.add_argument("--memory", action="store_true", help="measure peak RSS")
    parser.add_argument("--memory-runs", type=int, default=3)
    parser.add_argument("--threads", default="", help="comma list; sets XSV_THREADS for C++ binaries")
    parser.add_argument("--out", type=Path, default=ROOT / "results/raw")
    parser.add_argument("--case", action="append", default=[], help="only these case ids")
    parser.add_argument("--include-planned", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args(argv)

    runs = args.runs if args.runs is not None else (5 if args.profile == "smoke" else 20)
    warmup = args.warmup if args.warmup is not None else (1 if args.profile == "smoke" else 3)

    binaries: dict[str, Path] = {"rust": args.rust_bin.resolve()}
    if args.cpp_variant:
        for item in args.cpp_variant:
            if "=" not in item:
                parser.error(f"--cpp-variant must be variant=path: {item}")
            name, path = item.split("=", 1)
            binaries[name] = Path(path).resolve()
    else:
        binaries["all"] = (ROOT / "build/all/xsv").resolve()

    for name, path in binaries.items():
        if not path.is_file():
            parser.error(f"binary for {name} not found: {path}")

    if not args.data.is_dir():
        parser.error(f"dataset directory not found: {args.data} (run scripts/generate_datasets.py)")

    dataset_index = args.manifests / "index.json"
    snapshot = environment_snapshot(binaries, dataset_index)
    env_id = fingerprint(snapshot)
    args.out.mkdir(parents=True, exist_ok=True)
    timing_path = args.out / "timing.jsonl"
    memory_path = args.out / "memory.jsonl"
    env_path = args.out / "environment.json"

    if env_path.exists() and not args.dry_run:
        previous = json.loads(env_path.read_text())
        if previous.get("fingerprint") != env_id:
            print("error: environment fingerprint differs from existing results; refusing to append", file=sys.stderr)
            print(f"  file: {previous.get('fingerprint')}\n  now:  {env_id}", file=sys.stderr)
            return 3

    if not args.dry_run:
        env_path.write_text(json.dumps({"fingerprint": env_id, "snapshot": snapshot}, indent=2) + "\n")

    selected_cases = set(args.case)
    cases = [case for case in load_matrix(args.matrix)
             if (case.status == "implemented" or args.include_planned)
             and args.profile in case.profiles
             and (case.scaling == bool(args.threads))
             and (not selected_cases or case.id in selected_cases)]
    for case in cases:
        for argument in case.argv:
            if "{data}/" in argument:
                path = Path(argument.split("{data}/", 1)[1]).name
                if not (args.data / path).exists():
                    parser.error(f"{case.id}: missing dataset file {args.data / path}")

    threads = [int(value) for value in args.threads.split(",") if value.strip()] if args.threads else [None]

    if args.dry_run:
        for case in cases:
            for name, binary in binaries.items():
                for thread in threads:
                    print(f"[{case.id}] {name} threads={thread}: {binary} {' '.join(case.argv)}")
        return 0

    workdirs: list[Path] = []
    timing_records: list[dict[str, Any]] = []
    memory_records: list[dict[str, Any]] = []
    total = len(cases) * len(binaries) * (warmup + runs) * len(threads)
    done = 0

    for case in cases:
        rows_bytes = manifest_rows(args.manifests, case.dataset)
        workdir = Path(tempfile.mkdtemp(prefix="xsv-bench-"))
        workdirs.append(workdir)
        for source in case.copy:
            shutil.copyfile(args.data / Path(source).name, workdir / Path(source).name)
        base_argv = [argument.replace("{data}", str(args.data)).replace("{work}", str(workdir))
                     for argument in case.argv]
        if case.prepare == "index":
            producer = next(iter(binaries.values()))
            target = next((arg for arg in base_argv if arg.endswith(".csv")), None)
            if target is None:
                parser.error(f"{case.id}: prepare=index needs a .csv argument")
            index_env = os.environ.copy()
            indexed = subprocess.run([str(producer), "index", target], cwd=workdir,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                     env=index_env, timeout=args.timeout)
            if indexed.returncode != 0:
                print(f"error: preparing index for {case.id}: {indexed.stderr.decode()}", file=sys.stderr)
                return 4
        for thread in threads:
            env = os.environ.copy()
            if thread is not None:
                env["XSV_THREADS"] = str(thread)
            case_argv = [arg.replace("{threads}", str(thread if thread is not None else 1))
                         for arg in base_argv]
            order_seed = args.seed ^ int.from_bytes(
                hashlib.sha256((case.id + str(thread)).encode()).digest()[:4], "big")
            rng = random.Random(order_seed)

            for _ in range(warmup):
                order = list(binaries.items())
                rng.shuffle(order)
                for name, binary in order:
                    result = run_process(binary, case_argv, workdir, args.timeout, env)
                    done += 1
                    if result["exit"] != 0:
                        print(f"error: warmup {case.id} {name} exited {result['exit']}: {result['stderr']}", file=sys.stderr)
                        return 4

            for run in range(runs):
                order = list(binaries.items())
                rng.shuffle(order)
                for name, binary in order:
                    if args.cold:
                        for argument in case_argv:
                            if argument.endswith(".csv"):
                                drop_cache(Path(argument))
                    before = frequencies()
                    result = run_process(binary, case_argv, workdir, args.timeout, env)
                    after = frequencies()
                    done += 1
                    if result["exit"] != 0:
                        print(f"error: {case.id} {name} exited {result['exit']}: {result['stderr']}", file=sys.stderr)
                        return 4
                    record = {
                        "kind": "timing",
                        "case": case.id,
                        "benchmark": case.benchmark,
                        "command": case.command,
                        "dataset": case.dataset,
                        "variant": name,
                        "threads": thread,
                        "run": run,
                        "wall_s": result["wall_s"],
                        "exit": result["exit"],
                        "rows": rows_bytes[0] if rows_bytes else None,
                        "bytes": rows_bytes[1] if rows_bytes else None,
                        "rows_per_s": (rows_bytes[0] / result["wall_s"]) if rows_bytes else None,
                        "mb_per_s": (rows_bytes[1] / result["wall_s"] / 1e6) if rows_bytes else None,
                        "cold": args.cold,
                        "loadavg": after["loadavg"],
                        "mhz_median": after["mhz_median"],
                        "temp_max_c": after["temp_max_c"],
                        "seed": order_seed,
                    }
                    timing_records.append(record)
                    with timing_path.open("a") as handle:
                        handle.write(json.dumps(record, sort_keys=True) + "\n")
                    if done % 10 == 0 or done == total:
                        print(f"\r{done}/{total} runs", end="", file=sys.stderr)

            if args.memory:
                for name, binary in binaries.items():
                    rss = measure_rss(binary, case_argv, workdir, args.timeout, env, args.memory_runs)
                    record = {
                        "kind": "memory",
                        "case": case.id,
                        "benchmark": case.benchmark,
                        "command": case.command,
                        "dataset": case.dataset,
                        "variant": name,
                        "threads": thread,
                        "runs": args.memory_runs,
                        **rss,
                    }
                    memory_records.append(record)
                    with memory_path.open("a") as handle:
                        handle.write(json.dumps(record, sort_keys=True) + "\n")

    for workdir in workdirs:
        shutil.rmtree(workdir, ignore_errors=True)
    print(file=sys.stderr)
    print(f"wrote {len(timing_records)} timing records to {timing_path}")
    if args.memory:
        print(f"wrote {len(memory_records)} memory records to {memory_path}")

    for case in cases:
        for name in binaries:
            values = [r["wall_s"] for r in timing_records if r["case"] == case.id and r["variant"] == name]
            if values:
                print(f"  {case.id:28s} {name:8s} median={statistics.median(values) * 1000:9.3f} ms n={len(values)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
