#!/usr/bin/env python3
"""Replay the shared case corpus through the Rust reference and the C++ binary.

The oracle is intentionally narrow: stdout bytes, exit code, produced output
files and help text must match; stderr is compared by class unless a case names
a required substring. See docs/COMPATIBILITY.md §11.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

COMMANDS_P0 = ("count", "headers", "select", "slice", "search", "index", "sort", "stats", "join")
ROOT = Path(__file__).resolve().parent.parent


@dataclass
class Case:
    id: str
    command: str
    argv: list[str]
    tags: list[str] = field(default_factory=list)
    stdin_file: str | None = None
    stdin_text: str | None = None
    copy: dict[str, str] = field(default_factory=dict)
    prepare: str | None = None
    compare_stdout: bool = True
    stderr_contains: list[str] = field(default_factory=list)
    stderr_class: bool = True
    output_file: str | None = None
    output_files: list[str] = field(default_factory=list)
    env: dict[str, str] = field(default_factory=dict)
    timeout: float = 60.0


def load_cases(path: Path) -> list[Case]:
    cases: list[Case] = []
    with path.open(encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                raw = json.loads(line)
            except ValueError as error:
                raise SystemExit(f"{path}:{number}: invalid JSON: {error}") from error
            cases.append(
                Case(
                    id=raw["id"],
                    command=raw.get("command", raw["argv"][0]),
                    argv=list(raw["argv"]),
                    tags=list(raw.get("tags", [])),
                    stdin_file=raw.get("stdin_file"),
                    stdin_text=raw.get("stdin_text"),
                    copy=dict(raw.get("copy", {})),
                    prepare=raw.get("prepare"),
                    compare_stdout=raw.get("compare_stdout", True),
                    stderr_contains=list(raw.get("stderr_contains", [])),
                    stderr_class=raw.get("stderr_class", True),
                    output_file=raw.get("output_file"),
                    output_files=list(raw.get("output_files", [])),
                    env=dict(raw.get("env", {})),
                    timeout=float(raw.get("timeout", 60.0)),
                )
            )
    return cases


def expand(value: str, fixtures: Path, data: Path, work: Path) -> str:
    return (
        value.replace("{fixtures}", str(fixtures))
        .replace("{data}", str(data))
        .replace("{work}", str(work))
        .replace("{tmp}", str(work))
    )


def stderr_class(text: str) -> str:
    first = text.strip().splitlines()[0] if text.strip() else ""
    first = first.split(":")[0].lower()
    return re.sub(r"[0-9]+", "<n>", first)


def run_one(binary: Path, case: Case, work: Path, fixtures: Path, data: Path,
            index_binary: Path | None) -> dict[str, Any]:
    work.mkdir(parents=True, exist_ok=True)
    for destination, source in case.copy.items():
        target = work / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(expand(source, fixtures, data, work), target)

    if case.prepare in ("index", "index_stale", "index_corrupt", "index_self"):
        input_path = work / "input.csv"
        if case.prepare == "index_corrupt":
            input_path.write_text("a,b\n1,2\n", encoding="utf-8")
            (work / "input.csv.idx").write_bytes(b"\x00\x00\x00")
        else:
            producer = binary if case.prepare == "index_self" else index_binary
            if producer is None:
                raise RuntimeError("index preparation requires a binary")
            subprocess.run(
                [str(producer), "index", str(input_path)],
                cwd=work, check=True, capture_output=True, timeout=case.timeout,
            )
            if case.prepare == "index_stale":
                future = time.time() + 10
                os.utime(input_path, (future, future))

    argv = [str(binary)] + [expand(arg, fixtures, data, work) for arg in case.argv]
    stdin_bytes: bytes | None = None
    if case.stdin_file:
        stdin_bytes = Path(expand(case.stdin_file, fixtures, data, work)).read_bytes()
    elif case.stdin_text is not None:
        stdin_bytes = case.stdin_text.encode()

    env = os.environ.copy()
    env.update(case.env)
    started = time.perf_counter()
    completed = subprocess.run(
        argv,
        cwd=work,
        input=stdin_bytes,
        capture_output=True,
        timeout=case.timeout,
        env=env,
    )
    elapsed = time.perf_counter() - started
    output_bytes = None
    if case.output_file:
        output_path = work / case.output_file
        output_bytes = output_path.read_bytes() if output_path.exists() else b""
    outputs = {}
    for name in case.output_files:
        path = work / name
        outputs[name] = path.read_bytes() if path.exists() else b""
    return {
        "stdout": completed.stdout,
        "stderr": completed.stderr.decode("utf-8", "replace"),
        "exit": completed.returncode,
        "output": output_bytes,
        "outputs": outputs,
        "seconds": elapsed,
    }


def compare(case: Case, reference: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    problems: list[str] = []
    if reference["exit"] != candidate["exit"]:
        problems.append(f"exit {candidate['exit']} != {reference['exit']}")
    if case.compare_stdout and reference["stdout"] != candidate["stdout"]:
        a, b = reference["stdout"], candidate["stdout"]
        first = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
        problems.append(
            f"stdout differs at byte {first}: "
            f"ref={a[first:first + 40]!r} cpp={b[first:first + 40]!r}"
        )
    if case.output_file is not None and reference["output"] != candidate["output"]:
        problems.append("output file differs")
    for name in case.output_files:
        if reference["outputs"].get(name) != candidate["outputs"].get(name):
            problems.append(f"output file {name} differs")
    if case.stderr_contains:
        for needle in case.stderr_contains:
            if needle not in candidate["stderr"]:
                problems.append(f"stderr misses {needle!r}: {candidate['stderr'][:160]!r}")
    elif case.stderr_class and reference["exit"] != 0:
        if stderr_class(reference["stderr"]) != stderr_class(candidate["stderr"]):
            problems.append(
                f"stderr class {stderr_class(candidate['stderr'])!r} != "
                f"{stderr_class(reference['stderr'])!r}"
            )
    return problems


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-bin", type=Path, default=os.environ.get("XSV_CPP_BIN"))
    parser.add_argument("--rust-bin", type=Path, default=os.environ.get("XSV_RUST_BIN"))
    parser.add_argument("--cases", type=Path, default=ROOT / "evaluation/correctness/cases.jsonl")
    parser.add_argument("--fixtures", type=Path, default=ROOT / "evaluation/correctness/fixtures")
    parser.add_argument("--data", type=Path, default=ROOT / "datasets/data")
    parser.add_argument("--filter", default="", help="substring of the case id")
    parser.add_argument("--tag", action="append", default=[], help="required tag (repeatable)")
    parser.add_argument("--gate", choices=("p0",), help="require every P0 command family")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if not args.cpp_bin:
        parser.error("--cpp-bin or XSV_CPP_BIN is required")
    if not args.rust_bin:
        parser.error("--rust-bin or XSV_RUST_BIN is required")
    cpp_bin = Path(args.cpp_bin).resolve()
    rust_bin = Path(args.rust_bin).resolve()
    for label, binary in (("C++", cpp_bin), ("Rust", rust_bin)):
        if not binary.is_file():
            parser.error(f"{label} binary not found: {binary}")

    cases = load_cases(args.cases)
    if args.filter:
        cases = [case for case in cases if args.filter in case.id]
    if args.tag:
        wanted = set(args.tag)
        cases = [case for case in cases if wanted.issubset(set(case.tags))]

    if args.gate:
        present = {case.command for case in cases}
        missing = [command for command in COMMANDS_P0 if command not in present]
        if missing:
            print(f"gate {args.gate}: missing case families: {', '.join(missing)}", file=sys.stderr)
            print("The gate is red by design while a P0 command is unimplemented.", file=sys.stderr)
            return 2

    if not cases:
        print("no cases selected", file=sys.stderr)
        return 2

    results: list[dict[str, Any]] = []
    failures = 0
    started = time.perf_counter()

    with tempfile.TemporaryDirectory(prefix="xsv-diff-") as temporary:
        temp_root = Path(temporary)

        def execute(case: Case) -> dict[str, Any]:
            reference = run_one(rust_bin, case, temp_root / case.id / "rust",
                                args.fixtures, args.data, rust_bin)
            candidate = run_one(cpp_bin, case, temp_root / case.id / "cpp",
                                args.fixtures, args.data, rust_bin)
            problems = compare(case, reference, candidate)
            return {
                "id": case.id,
                "command": case.command,
                "tags": case.tags,
                "problems": problems,
                "seconds": reference["seconds"] + candidate["seconds"],
            }

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(execute, case): case for case in cases}
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                results.append(result)
                if result["problems"]:
                    failures += 1
                    print(f"FAIL {result['id']}")
                    for problem in result["problems"]:
                        print(f"     {problem}")
                elif args.verbose:
                    print(f"ok   {result['id']} ({result['seconds'] * 1000:.0f} ms)")

    elapsed = time.perf_counter() - started
    results.sort(key=lambda item: item["id"])
    print(f"\n{len(results) - failures}/{len(results)} cases matched in {elapsed:.2f}s")
    for command in COMMANDS_P0:
        family = [item for item in results if item["command"] == command]
        if not family:
            continue
        passed = len([item for item in family if not item["problems"]])
        print(f"  {command:8s} {passed}/{len(family)}")

    report = {
        "cpp_bin": str(cpp_bin),
        "rust_bin": str(rust_bin),
        "cases": len(results),
        "failures": failures,
        "seconds": elapsed,
        "results": results,
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
