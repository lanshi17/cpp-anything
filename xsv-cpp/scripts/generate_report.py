#!/usr/bin/env python3
"""Turn raw benchmark JSONL into tables, SVGs and a Markdown report.

No number in the output is hand-entered: every cell comes from
``results/raw/*.jsonl`` plus the dataset manifests. Missing measurements are
reported as missing, never as zero.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parent.parent


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    records = []
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1 - weight) + ordered[upper] * weight


def bootstrap_ci(values: list[float], seed: int = 20261120, resamples: int = 10000) -> tuple[float, float]:
    if len(values) < 2:
        return (math.nan, math.nan)
    rng = random.Random(seed)
    medians = []
    for _ in range(resamples):
        sample = [values[rng.randrange(len(values))] for _ in range(len(values))]
        medians.append(statistics.median(sample))
    medians.sort()
    return medians[int(0.025 * len(medians))], medians[int(0.975 * len(medians)) - 1]


def summarize(timing: list[dict[str, Any]]) -> dict[tuple[str, str], dict[str, Any]]:
    grouped: dict[tuple[str, str], list[float]] = defaultdict(list)
    metadata: dict[tuple[str, str], dict[str, Any]] = {}
    for record in timing:
        key = (record["case"], record["variant"])
        grouped[key].append(float(record["wall_s"]))
        metadata[key] = record
    summary = {}
    for key, values in grouped.items():
        ci_low, ci_high = bootstrap_ci(values)
        summary[key] = {
            "n": len(values),
            "median_s": statistics.median(values),
            "mean_s": statistics.fmean(values),
            "p95_s": percentile(values, 0.95),
            "stddev_s": statistics.stdev(values) if len(values) > 1 else 0.0,
            "min_s": min(values),
            "max_s": max(values),
            "ci_low_s": ci_low,
            "ci_high_s": ci_high,
            "rows": metadata[key].get("rows"),
            "bytes": metadata[key].get("bytes"),
            "command": metadata[key].get("command"),
            "dataset": metadata[key].get("dataset"),
            "benchmark": metadata[key].get("benchmark"),
        }
    return summary


def variant_order(names: Iterable[str]) -> list[str]:
    preferred = ["rust", "base", "all", "native", "pst", "pmr", "simd", "tbb", "hash", "mmap", "ps", "pt", "st", "ht"]
    present = set(names)
    ordered = [name for name in preferred if name in present]
    ordered.extend(sorted(present - set(ordered)))
    return ordered


def fmt_ms(value: float) -> str:
    return f"{value * 1000:.3f}"


def ratio(value: float, reference: float) -> str:
    if not reference or math.isnan(reference) or math.isnan(value):
        return "—"
    return f"{value / reference:.3f}"


PALETTE = ["#4c78a8", "#59a14f", "#e15759", "#f28e2b", "#76b7b2", "#b07aa1", "#9c755f", "#bab0ac"]


def bar_svg(title: str, groups: list[str], series: dict[str, list[float]], ylabel: str) -> str:
    width = 900
    height = 380
    left = 70
    right = 20
    top = 50
    bottom = 80
    plot_w = width - left - right
    plot_h = height - top - bottom
    maximum = max((value for values in series.values() for value in values if not math.isnan(value)), default=0.0)
    maximum = maximum if maximum > 0 else 1.0
    series_names = list(series)
    group_w = plot_w / max(1, len(groups))
    bar_w = group_w * 0.8 / max(1, len(series_names))
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" role="img" aria-label="{title}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{left}" y="26" font-family="sans-serif" font-size="16" fill="#222">{title}</text>',
        f'<text x="14" y="{top + plot_h / 2}" font-family="sans-serif" font-size="12" fill="#555" transform="rotate(-90 14 {top + plot_h / 2})">{ylabel}</text>',
    ]
    for tick in range(5):
        value = maximum * tick / 4
        y = top + plot_h - plot_h * tick / 4
        parts.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}" stroke="#e6e6e6"/>')
        parts.append(f'<text x="{left - 8}" y="{y + 4:.1f}" text-anchor="end" font-family="sans-serif" font-size="11" fill="#666">{value:.3g}</text>')
    for gi, group in enumerate(groups):
        base_x = left + gi * group_w + group_w * 0.1
        for si, name in enumerate(series_names):
            value = series[name][gi] if gi < len(series[name]) else math.nan
            if math.isnan(value):
                continue
            bar_h = plot_h * value / maximum
            x = base_x + si * bar_w
            y = top + plot_h - bar_h
            parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_w * 0.9:.1f}" height="{bar_h:.1f}" fill="{PALETTE[si % len(PALETTE)]}"/>')
        parts.append(f'<text x="{left + gi * group_w + group_w / 2:.1f}" y="{top + plot_h + 20}" text-anchor="middle" font-family="sans-serif" font-size="11" fill="#333">{group}</text>')
    legend_x = left
    for si, name in enumerate(series_names):
        y = height - 30
        parts.append(f'<rect x="{legend_x}" y="{y - 10}" width="12" height="12" fill="{PALETTE[si % len(PALETTE)]}"/>')
        parts.append(f'<text x="{legend_x + 18}" y="{y}" font-family="sans-serif" font-size="12" fill="#333">{name}</text>')
        legend_x += 110
    parts.append("</svg>")
    return "\n".join(parts)


def line_svg(title: str, threads: list[int], series: dict[str, dict[int, float]], ylabel: str) -> str:
    width = 900
    height = 380
    left = 80
    right = 180
    top = 50
    bottom = 60
    plot_w = width - left - right
    plot_h = height - top - bottom
    values = [value for line in series.values() for value in line.values() if not math.isnan(value)]
    maximum = max(values, default=1.0)
    maximum = maximum if maximum > 0 else 1.0
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" role="img" aria-label="{title}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{left}" y="26" font-family="sans-serif" font-size="16" fill="#222">{title}</text>',
        f'<text x="16" y="{top + plot_h / 2}" font-family="sans-serif" font-size="12" fill="#555" transform="rotate(-90 16 {top + plot_h / 2})">{ylabel}</text>',
    ]
    for tick in range(5):
        value = maximum * tick / 4
        y = top + plot_h - plot_h * tick / 4
        parts.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}" stroke="#e6e6e6"/>')
        parts.append(f'<text x="{left - 8}" y="{y + 4:.1f}" text-anchor="end" font-family="sans-serif" font-size="11" fill="#666">{value:.3g}</text>')
    x_at = {}
    for index, thread in enumerate(threads):
        x = left + (plot_w * index / max(1, len(threads) - 1))
        x_at[thread] = x
        parts.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{top + plot_h}" stroke="#f1f1f1"/>')
        parts.append(f'<text x="{x:.1f}" y="{top + plot_h + 20}" text-anchor="middle" font-family="sans-serif" font-size="11" fill="#333">{thread}</text>')
    for si, (name, points) in enumerate(series.items()):
        color = PALETTE[si % len(PALETTE)]
        coords = []
        for thread in threads:
            if thread not in points:
                continue
            x = x_at[thread]
            y = top + plot_h - plot_h * points[thread] / maximum
            coords.append(f"{x:.1f},{y:.1f}")
            parts.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="3.5" fill="{color}"/>')
        if len(coords) > 1:
            parts.append(f'<polyline points="{" ".join(coords)}" fill="none" stroke="{color}" stroke-width="2"/>')
    legend_y = top + 8
    for si, name in enumerate(series):
        parts.append(f'<rect x="{left + plot_w + 20}" y="{legend_y - 10}" width="12" height="12" fill="{PALETTE[si % len(PALETTE)]}"/>')
        parts.append(f'<text x="{left + plot_w + 38}" y="{legend_y}" font-family="sans-serif" font-size="12" fill="#333">{name}</text>')
        legend_y += 22
    parts.append("</svg>")
    return "\n".join(parts)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, default=ROOT / "results/raw")
    parser.add_argument("--scaling-raw", type=Path, default=ROOT / "results/raw-scaling")
    parser.add_argument("--ablation-raw", type=Path, default=ROOT / "results/raw-ablation")
    parser.add_argument("--complexity", type=Path, default=ROOT / "results/processed/complexity.json")
    parser.add_argument("--out", type=Path, default=ROOT / "results")
    args = parser.parse_args(argv)

    timing = read_jsonl(args.raw / "timing.jsonl")
    memory = read_jsonl(args.raw / "memory.jsonl")
    scaling = read_jsonl(args.scaling_raw / "timing.jsonl")
    scaling = [record for record in scaling if record.get("threads") is not None]
    ablation = read_jsonl(args.ablation_raw / "timing.jsonl")
    ablation_summary = summarize(ablation) if ablation else {}
    if not timing:
        print(f"no timing records in {args.raw / 'timing.jsonl'}")
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "processed").mkdir(exist_ok=True)
    (args.out / "figures").mkdir(exist_ok=True)

    summary = summarize(timing)
    benchmark_of = {}
    for (case, _variant), item in summary.items():
        benchmark_of[case] = item["benchmark"]
    cases = sorted({case for case, _ in summary}, key=lambda value: (benchmark_of.get(value, ""), value))
    variants = variant_order({variant for _, variant in summary})

    with (args.out / "processed/timing.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["case", "benchmark", "command", "dataset", "variant", "n", "median_s", "mean_s", "p95_s",
                         "stddev_s", "min_s", "max_s", "ci_low_s", "ci_high_s", "rows", "bytes", "rows_per_s"])
        for (case, variant), item in sorted(summary.items()):
            writer.writerow([case, item["benchmark"], item["command"], item["dataset"], variant, item["n"],
                             f"{item['median_s']:.9f}", f"{item['mean_s']:.9f}", f"{item['p95_s']:.9f}",
                             f"{item['stddev_s']:.9f}", f"{item['min_s']:.9f}", f"{item['max_s']:.9f}",
                             f"{item['ci_low_s']:.9f}", f"{item['ci_high_s']:.9f}", item["rows"], item["bytes"],
                             f"{item['rows'] / item['median_s']:.3f}" if item["rows"] else ""])

    env_path = args.raw / "environment.json"
    env_note = "missing"
    if env_path.exists():
        env = json.loads(env_path.read_text())
        env_note = f"`{env.get('fingerprint')}` ({env.get('snapshot', {}).get('cpu', 'unknown CPU')})"

    lines = [
        "# xsv-cpp benchmark report",
        "",
        f"Generated from `{args.raw / 'timing.jsonl'}` and `{args.raw / 'memory.jsonl'}`. "
        "Every table cell is produced by `scripts/generate_report.py`; no value is typed by hand.",
        "",
        f"Variants present: {', '.join(variants)}.",
        f"Cases: {len(cases)}. Timing records: {len(timing)}. Memory records: {len(memory)}.",
        f"Environment fingerprint: {env_note}.",
        "",
        "## 1. Wall time",
        "",
        "Median wall-clock time per case, milliseconds. `ratio` is the candidate divided by the reference "
        "(lower is better); `<1` means faster.",
        "",
        "| case | " + " | ".join(variants) + " | base/rust | all/rust | all/base |",
        "| --- | " + " | ".join("---" for _ in variants) + " | --- | --- | --- |",
    ]
    for case in cases:
        cells = []
        for variant in variants:
            item = summary.get((case, variant))
            cells.append(fmt_ms(item["median_s"]) if item else "—")
        all_item = summary.get((case, "all"))
        rust_item = summary.get((case, "rust"))
        base_item = summary.get((case, "base"))
        base_vs_rust = ratio(base_item["median_s"], rust_item["median_s"]) if base_item and rust_item else "—"
        all_vs_rust = ratio(all_item["median_s"], rust_item["median_s"]) if all_item and rust_item else "—"
        all_vs_base = ratio(all_item["median_s"], base_item["median_s"]) if all_item and base_item else "—"
        lines.append(f"| `{case}` | " + " | ".join(cells) + f" | {base_vs_rust} | {all_vs_rust} | {all_vs_base} |")

    lines += [
        "",
        "## 2. Peak RSS",
        "",
        "`/usr/bin/time -v` median across the configured runs. Values are MiB. "
        "Streaming `all` builds drop consumed mapped pages every 8 MiB; the window, "
        "not the file size, dominates their RSS.",
        "",
    ]
    if memory:
        mem_summary: dict[tuple[str, str], list[int]] = defaultdict(list)
        for record in memory:
            mem_summary[(record["case"], record["variant"])].append(int(record["rss_kb_median"]))
        lines.append("| case | " + " | ".join(variants) + " |")
        lines.append("| --- | " + " | ".join("---" for _ in variants) + " |")
        for case in cases:
            cells = []
            for variant in variants:
                values = mem_summary.get((case, variant))
                cells.append(f"{statistics.median(values) / 1024:.1f}" if values else "—")
            lines.append(f"| `{case}` | " + " | ".join(cells) + " |")
    else:
        lines.append("No memory records yet: run `scripts/run_benchmarks.py --memory`.")
    lines.append("")

    lines += [
        "## 3. Throughput",
        "",
        "Rows/s from the median wall time. Only cases with a dataset manifest have throughput.",
        "",
        "| case | " + " | ".join(variants) + " |",
        "| --- | " + " | ".join("---" for _ in variants) + " |",
    ]
    for case in cases:
        cells = []
        for variant in variants:
            item = summary.get((case, variant))
            if item and item["rows"]:
                cells.append(f"{item['rows'] / item['median_s'] / 1e6:.2f} M")
            else:
                cells.append("—")
        lines.append(f"| `{case}` | " + " | ".join(cells) + " |")

    scaling_groups: dict[tuple[str, str, int], list[float]] = defaultdict(list)
    for record in scaling:
        scaling_groups[(record["case"], record["variant"], int(record["threads"]))].append(
            float(record["wall_s"]))
    if scaling_groups:
        scaling_threads = sorted({thread for _, _, thread in scaling_groups})
        lines += [
            "",
            "## 4. Thread scaling",
            "",
            "Median wall time for `stats -j N` on an indexed dataset. Speedup is relative to N=1.",
            "",
            "| case | variant | threads | median ms | M rows/s | speedup |",
            "| --- | --- | --- | --- | --- | --- |",
        ]
        scaling_series: dict[str, dict[int, float]] = defaultdict(dict)
        for (case, variant, thread), values in sorted(scaling_groups.items()):
            median = statistics.median(values)
            baseline = statistics.median(scaling_groups.get((case, variant, 1), [median]))
            rows = next((record.get("rows") for record in scaling
                         if record["case"] == case and record["variant"] == variant), None)
            lines.append(
                f"| `{case}` | {variant} | {thread} | {median * 1000:.3f} | "
                f"{(rows / median / 1e6) if rows else '—'} | {baseline / median:.3f} |"
            )
            if rows:
                scaling_series[variant][thread] = rows / median / 1e6
        (args.out / "figures/figure3-scaling.svg").write_text(
            line_svg("Throughput vs threads (indexed stats)", scaling_threads,
                     dict(scaling_series), "M rows/s"),
            encoding="utf-8",
        )
    else:
        lines += ["", "## 4. Thread scaling", "",
                  "No scaling records; run `scripts/run_benchmarks.py --threads 1,2,4,8,16,32`."]

    ablation_variants = variant_order({variant for _, variant in ablation_summary})
    axes = [variant for variant in ablation_variants
            if variant not in {"rust", "base", "all"}]
    lines += ["", "## 5. Ablation", ""]
    if ablation_summary and axes:
        benchmark_of_ablation = {}
        for (case, _variant), item in ablation_summary.items():
            benchmark_of_ablation[case] = item["benchmark"]
        ablation_cases = sorted({case for case, _ in ablation_summary},
                                key=lambda value: (benchmark_of_ablation.get(value, ""), value))
        lines += [
            "Speedup of each single-axis build over `base` (>1 means faster for the same case).",
            "",
            "| case | " + " | ".join(axes) + " |",
            "| --- | " + " | ".join("---" for _ in axes) + " |",
        ]
        ablation_series: dict[str, list[float]] = {axis: [] for axis in axes}
        for case in ablation_cases:
            base_item = ablation_summary.get((case, "base"))
            cells = []
            for axis in axes:
                item = ablation_summary.get((case, axis))
                value = (base_item["median_s"] / item["median_s"]) if base_item and item else math.nan
                cells.append(f"{value:.3f}" if not math.isnan(value) else "—")
                ablation_series[axis].append(value)
            lines.append(f"| `{case}` | " + " | ".join(cells) + " |")
        (args.out / "figures/figure4-ablation.svg").write_text(
            bar_svg("Speedup vs C++ base (>1 faster)", ablation_cases, ablation_series, "ratio"),
            encoding="utf-8",
        )
    else:
        lines.append("No ablation records; run the single-axis variants on the `B03/B05/B06/B07/B10` cases.")

    lines += ["", "## 6. Engineering complexity", ""]
    if args.complexity.exists():
        complexity = json.loads(args.complexity.read_text())
        lines += [
            "| metric | value |",
            "| --- | --- |",
            f"| C++ implementation LOC (headers included) | {complexity.get('source_lines'):,} |",
            f"| test LOC | {complexity.get('test_lines'):,} |",
            f"| documentation LOC | {complexity.get('doc_lines'):,} |",
            f"| scripts LOC | {complexity.get('script_lines'):,} |",
            f"| direct dependencies | {complexity.get('direct_dependencies')} |",
            f"| differential cases | {complexity.get('differential_cases')} |",
            f"| CTest tests | {complexity.get('ctest_tests')} |",
            f"| gate pass rate | {complexity.get('gate_pass_rate', 0) * 100:.1f}% |",
            f"| planned effort (person-days) | {complexity.get('planned_person_days')} |",
            f"| binary size (bytes) | {complexity.get('binary_bytes'):,} |",
        ]
        if complexity.get("build_seconds"):
            lines.append(f"| incremental clean build (s) | {complexity['build_seconds']:.1f} |")
        lines.append("")
        loc_series = {"lines": [
            complexity.get("implementation_lines", 0),
            complexity.get("header_lines", 0),
            complexity.get("test_lines", 0),
            complexity.get("doc_lines", 0),
            complexity.get("script_lines", 0),
        ]}
        (args.out / "figures/figure5-complexity.svg").write_text(
            bar_svg("Lines of code by area", ["src", "include", "tests", "docs", "scripts"],
                    loc_series, "lines"),
            encoding="utf-8",
        )
    else:
        lines.append("Run `scripts/collect_complexity.py` to populate this section.")

    lines += [
        "",
        "## 7. Interpretation guardrails",
        "",
        "- `rust` is the unmodified 0.13.0 release; `base` is C++ with every optimization axis off; `all` is the "
        "optimized build.",
        "- `all/rust` mixes language and algorithm. `all/base` isolates the optimization contribution; `base/rust` "
        "is the language-level number.",
        "- A single dataset size cannot establish scaling; see the thread-scaling section once collected.",
        f"- These numbers are only valid on the machine whose fingerprint is in `{args.raw / 'environment.json'}`.",
        "- `all` maps input files; peak RSS includes resident file-backed pages. Use the allocation counters "
        "(heaptrack or PMR statistics) to attribute allocator effects separately from page-cache residency.",
        "",
    ]

    (args.out / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    # Figures: one group per case, one bar per variant.
    series = {variant: [] for variant in variants}
    for case in cases:
        for variant in variants:
            item = summary.get((case, variant))
            series[variant].append(item["median_s"] * 1000 if item else math.nan)
    (args.out / "figures/figure1-wall-time.svg").write_text(
        bar_svg("Wall time by case (median, ms)", cases, series, "milliseconds"), encoding="utf-8"
    )

    if memory:
        mem_series = {variant: [] for variant in variants}
        for case in cases:
            for variant in variants:
                values = [int(record["rss_kb_median"]) for record in memory
                          if record["case"] == case and record["variant"] == variant]
                mem_series[variant].append(statistics.median(values) / 1024 if values else math.nan)
        (args.out / "figures/figure2-peak-rss.svg").write_text(
            bar_svg("Peak RSS by case (median, MiB)", cases, mem_series, "MiB"), encoding="utf-8"
        )

    print(f"wrote {args.out / 'report.md'}")
    print(f"wrote {args.out / 'processed/timing.csv'}")
    print(f"wrote {args.out / 'figures/figure1-wall-time.svg'}")
    return 0


if __name__ == "__main__":
    import sys

    sys.exit(main(sys.argv[1:]))
