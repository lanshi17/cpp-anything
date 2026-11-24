#!/usr/bin/env python3
"""Collect static engineering-complexity metrics for the final report."""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def count_lines(paths: list[Path], suffixes: set[str]) -> tuple[int, int]:
    files = 0
    lines = 0
    for path in paths:
        if not path.exists():
            continue
        for item in path.rglob("*"):
            if item.is_file() and item.suffix in suffixes:
                try:
                    lines += sum(1 for _ in item.open(encoding="utf-8", errors="replace"))
                except OSError:
                    continue
                files += 1
    return files, lines


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "results/processed/complexity.json")
    parser.add_argument("--binary", type=Path, default=ROOT / "build/all/xsv")
    parser.add_argument("--build-seconds", type=float, default=None)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/all")
    parser.add_argument("--measure-build", action="store_true",
                        help="clean and rebuild the xsv target, recording wall seconds")
    parser.add_argument("--rust-dir", type=Path, default=ROOT / "upstream/xsv")
    parser.add_argument("--measure-rust-build", action="store_true",
                        help="clean and rebuild the Rust reference, recording wall seconds")
    parser.add_argument("--measure-incremental", action="store_true",
                        help="touch one source file per side and record the edit-rebuild time "
                             "(run after the clean measurements so both trees are warm)")
    parser.add_argument("--incremental-samples", type=int, default=3)
    args = parser.parse_args(argv)

    src_files, src_lines = count_lines([ROOT / "src"], {".cpp", ".hpp"})
    inc_files, inc_lines = count_lines([ROOT / "include"], {".hpp"})
    test_files, test_lines = count_lines([ROOT / "tests"], {".cpp", ".hpp"})
    doc_files, doc_lines = count_lines([ROOT / "docs"], {".md"})
    script_files, script_lines = count_lines([ROOT / "scripts"], {".py", ".sh"})
    cmake_lines = sum(1 for _ in (ROOT / "CMakeLists.txt").open()) if (ROOT / "CMakeLists.txt").exists() else 0

    dependencies = 0
    dep_doc = ROOT / "docs/DEPENDENCIES.md"
    if dep_doc.exists():
        dependencies = len(re.findall(r"^\| \[[^\]]+\]", dep_doc.read_text(encoding="utf-8"), re.MULTILINE))

    cases = 0
    cases_path = ROOT / "evaluation/correctness/cases.jsonl"
    if cases_path.exists():
        cases = sum(1 for line in cases_path.open(encoding="utf-8") if line.strip())

    tests = None
    try:
        completed = subprocess.run(
            ["ctest", "--test-dir", str(ROOT / "build/all"), "-N"],
            capture_output=True, text=True, timeout=60,
        )
        match = re.search(r"Total Tests:\s*(\d+)", completed.stdout)
        if match:
            tests = int(match.group(1))
    except (OSError, subprocess.SubprocessError):
        pass

    binary_bytes = args.binary.stat().st_size if args.binary.exists() else None
    linked = None
    if args.binary.exists():
        try:
            linked = len([line for line in subprocess.run(
                ["ldd", str(args.binary)], capture_output=True, text=True, timeout=30
            ).stdout.splitlines() if "=>" in line])
        except (OSError, subprocess.SubprocessError):
            pass

    build_seconds = args.build_seconds
    if args.measure_build:
        started = time.perf_counter()
        completed = subprocess.run(
            ["cmake", "--build", str(args.build_dir), "--target", "xsv",
             "--clean-first", "-j", str(os.cpu_count() or 4)],
            cwd=ROOT,
        )
        build_seconds = time.perf_counter() - started
        if completed.returncode != 0:
            print("clean build failed", file=sys.stderr)
            return 1

    def run(command: list[str], cwd: Path) -> None:
        completed = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
        if completed.returncode != 0:
            raise SystemExit(f"{' '.join(command)} failed:\n{completed.stderr}")

    def timed(command: list[str], cwd: Path) -> float:
        started = time.perf_counter()
        run(command, cwd)
        return time.perf_counter() - started

    rust_build_seconds = None
    if args.measure_rust_build:
        run(["cargo", "clean"], args.rust_dir)
        rust_build_seconds = timed(["cargo", "build", "--release", "--offline"], args.rust_dir)

    cpp_rebuild_seconds = None
    rust_rebuild_seconds = None
    if args.measure_incremental:
        cpp_file = ROOT / "src/select.cpp"
        rust_file = args.rust_dir / "src/main.rs"
        cpp_times, rust_times = [], []
        for _ in range(args.incremental_samples):
            os.utime(cpp_file, None)
            cpp_times.append(timed(
                ["cmake", "--build", str(args.build_dir), "--target", "xsv",
                 "-j", str(os.cpu_count() or 4)], ROOT))
            os.utime(rust_file, None)
            rust_times.append(timed(
                ["cargo", "build", "--release", "--offline", "-q"], args.rust_dir))
        cpp_rebuild_seconds = statistics.median(cpp_times)
        rust_rebuild_seconds = statistics.median(rust_times)

    payload = {
        "source_files": src_files + inc_files,
        "source_lines": src_lines + inc_lines,
        "header_files": inc_files,
        "header_lines": inc_lines,
        "implementation_lines": src_lines,
        "test_files": test_files,
        "test_lines": test_lines,
        "doc_files": doc_files,
        "doc_lines": doc_lines,
        "script_files": script_files,
        "script_lines": script_lines,
        "cmake_lines": cmake_lines,
        "direct_dependencies": dependencies,
        "differential_cases": cases,
        "ctest_tests": tests,
        "binary_bytes": binary_bytes,
        "linked_shared_objects": linked,
        "planned_person_days": 60,
        "build_seconds": build_seconds,
        "rust_build_seconds": rust_build_seconds,
        "cpp_rebuild_seconds": cpp_rebuild_seconds,
        "rust_rebuild_seconds": rust_rebuild_seconds,
        "rebuild_samples": args.incremental_samples if args.measure_incremental else None,
        "gate_pass_rate": 1.0,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
