#!/usr/bin/env python3
"""Measure CPU-side indicators without perf_event access.

perf_event_paranoid=4 blocks hardware counters on the reference host. This
runner uses a TSC + wait4 helper to record wall time, TSC cycles, user/system
CPU seconds, rusage page faults, context switches and max RSS. It does not
claim IPC or retired instructions.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CASES = [
    ("B01.count.large", ["count", "{data}/large_uniform.csv"]),
    ("B03.select.medium", ["select", "-n", "1,10,20", "{data}/medium_uniform.csv"]),
    ("B04.search.medium", ["search", "-n", "alpha", "{data}/medium_uniform.csv"]),
    ("B05.sort.medium", ["sort", "-n", "-s", "1", "{data}/medium_uniform.csv"]),
    ("B06.join.medium", ["join", "-n", "1", "{data}/join_left.csv", "1", "{data}/join_right.csv"]),
    ("B07.stats.medium", ["stats", "-n", "{data}/medium_uniform.csv"]),
    ("B10.slice.medium", ["slice", "-s", "1000", "-l", "1000", "{data}/medium_uniform.csv"]),
]


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, default=ROOT / "build/cpu-metrics")
    parser.add_argument("--rust-bin", type=Path, default=ROOT / "upstream/xsv/target/release/xsv")
    parser.add_argument("--cpp-variant", action="append", default=[])
    parser.add_argument("--data", type=Path, default=ROOT / "datasets/data")
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--out", type=Path, default=ROOT / "results/cpu-metrics")
    parser.add_argument("--cases", default="", help="comma list of case ids")
    args = parser.parse_args(argv)

    if not args.helper.is_file():
        parser.error(f"helper not found: {args.helper}")
    variants = {"rust": args.rust_bin}
    for item in args.cpp_variant:
        if "=" not in item:
            parser.error(f"--cpp-variant must be name=path: {item}")
        name, path = item.split("=", 1)
        variants[name] = Path(path)
    for name, path in variants.items():
        if not path.is_file():
            parser.error(f"binary for {name} not found: {path}")

    wanted = {item for item in args.cases.split(",") if item}
    cases = [case for case in CASES if not wanted or case[0] in wanted]
    args.out.mkdir(parents=True, exist_ok=True)
    output = args.out / "timing.jsonl"
    output.write_text("", encoding="utf-8")

    records = []
    for case_id, template in cases:
        argv_cmd = [arg.replace("{data}", str(args.data)) for arg in template]
        for name, binary in variants.items():
            for _ in range(args.warmup):
                subprocess.run([str(args.helper), "--", str(binary)] + argv_cmd,
                               cwd=ROOT, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, check=False)
            for run in range(args.runs):
                completed = subprocess.run(
                    [str(args.helper), "--", str(binary)] + argv_cmd,
                    cwd=ROOT, capture_output=True, text=True, timeout=600,
                )
                if completed.returncode != 0 or not completed.stdout.strip():
                    print(f"error: {case_id} {name}: {completed.stderr[:200]}", file=sys.stderr)
                    return 1
                metrics = json.loads(completed.stdout)
                record = {"case": case_id, "variant": name, "run": run, **metrics}
                records.append(record)
                with output.open("a") as handle:
                    handle.write(json.dumps(record, sort_keys=True) + "\n")

    grouped = {}
    for record in records:
        grouped.setdefault((record["case"], record["variant"]), []).append(record)
    lines = [
        "# CPU hardware indicators (TSC / getrusage)",
        "",
        "`perf_event_paranoid=4` blocks perf counters. These are the portable",
        "substitutes: wall time, TSC cycles, CPU seconds, faults and context",
        "switches. **IPC and retired instructions are not measured.**",
        "",
        "| case | variant | wall ms | CPU s | eff. GHz | TSC Gcycles | minflt | ctx switches | maxRSS MiB |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    summary = {}
    for (case, variant), values in sorted(grouped.items()):
        med = {key: statistics.median([v[key] for v in values])
               for key in ("wall_s", "cpu_s", "effective_ghz", "tsc_cycles",
                           "minflt", "nvcsw", "nivcsw", "maxrss_kb")}
        summary[f"{case}|{variant}"] = med
        lines.append(
            f"| `{case}` | {variant} | {med['wall_s']*1000:.1f} | "
            f"{med['cpu_s']:.3f} | {med['effective_ghz']:.2f} | "
            f"{med['tsc_cycles']/1e9:.1f} | {med['minflt']:.0f} | "
            f"{med['nvcsw']+med['nivcsw']:.0f} | {med['maxrss_kb']/1024:.1f} |"
        )
    (args.out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                           encoding="utf-8")
    print(f"wrote {output} and {args.out / 'summary.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
