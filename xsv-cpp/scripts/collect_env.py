#!/usr/bin/env python3
"""Record the machine, toolchain, binaries and dependency revisions.

The result is JSON so a benchmark record can be tied to an exact environment.
Nothing here measures performance.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent


def sha256(path: Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command(args: list[str], cwd: Path | None = None) -> str | None:
    try:
        completed = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return (completed.stdout or completed.stderr).strip().splitlines()[0]


def cpu_info() -> dict[str, Any]:
    model = None
    flags = None
    mhz = []
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name") and model is None:
                model = line.split(":", 1)[1].strip()
            elif line.startswith("flags") and flags is None:
                flags = line.split(":", 1)[1].strip().split()
            elif line.startswith("cpu MHz"):
                try:
                    mhz.append(float(line.split(":", 1)[1]))
                except ValueError:
                    pass
    except OSError:
        pass
    governor = None
    try:
        governor = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").read_text().strip()
    except OSError:
        pass
    temperatures = []
    for zone in sorted(Path("/sys/class/thermal").glob("thermal_zone*/temp")):
        try:
            temperatures.append(int(zone.read_text().strip()) / 1000.0)
        except (OSError, ValueError):
            pass
    return {
        "model": model,
        "flags": flags,
        "physical_cores": os.cpu_count(),
        "mhz_min": min(mhz) if mhz else None,
        "mhz_max": max(mhz) if mhz else None,
        "governor": governor,
        "temp_max_c": max(temperatures) if temperatures else None,
    }


def memory_kb() -> int | None:
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal:"):
                return int(line.split()[1])
    except OSError:
        pass
    return None


def git_revision(path: Path) -> str | None:
    if not (path / ".git").exists():
        return None
    return command(["git", "rev-parse", "HEAD"], cwd=path)


def dependency_revisions(build_root: Path) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name in ("onetbb", "re2", "abseil", "fast_float"):
        source = build_root / "_deps" / f"{name}-src"
        result[name] = {
            "path": str(source) if source.exists() else None,
            "commit": git_revision(source) if source.exists() else None,
        }
    return result


def dataset_fingerprints(data: Path, manifests: Path) -> dict[str, Any]:
    index = manifests / "index.json"
    files = {}
    if index.exists():
        raw = json.loads(index.read_text())
        for item in raw.get("datasets", []):
            files[item["dataset"]] = {
                "rows": item["rows"],
                "bytes": item["bytes"],
                "sha256": item["sha256"],
                "manifest": str(manifests / f"{item['dataset']}.json"),
            }
    return {"index_sha256": sha256(index), "datasets": files}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-bin", type=Path, default=ROOT / "build/all/xsv")
    parser.add_argument("--rust-bin", type=Path, default=ROOT / "upstream/xsv/target/release/xsv")
    parser.add_argument("--data", type=Path, default=ROOT / "datasets/data")
    parser.add_argument("--manifests", type=Path, default=ROOT / "datasets/manifests")
    parser.add_argument("--build-root", type=Path, default=ROOT / "build/all")
    parser.add_argument("--out", type=Path, default=ROOT / "results/environment.json")
    args = parser.parse_args(argv)

    payload: dict[str, Any] = {
        "os": {
            "system": platform.system(),
            "release": platform.release(),
            "version": platform.version(),
            "machine": platform.machine(),
            "distribution": command(["lsb_release", "-ds"]),
        },
        "kernel": platform.release(),
        "cpu": cpu_info(),
        "memory_total_kb": memory_kb(),
        "tools": {
            "gcc": command(["gcc", "--version"]),
            "gxx": command(["g++", "--version"]),
            "cmake": command(["cmake", "--version"]),
            "ninja": command(["ninja", "--version"]),
            "python": platform.python_version(),
            "rustc": command(["rustc", "--version"]),
            "cargo": command(["cargo", "--version"]),
            "hyperfine": command(["hyperfine", "--version"]),
            "perf": command(["perf", "--version"]),
            "heaptrack": command(["heaptrack", "--version"]),
            "valgrind": command(["valgrind", "--version"]),
            "git": command(["git", "--version"]),
        },
        "binaries": {
            "cpp": {
                "path": str(args.cpp_bin),
                "sha256": sha256(args.cpp_bin),
                "version": command([str(args.cpp_bin), "--version"]),
            },
            "rust": {
                "path": str(args.rust_bin),
                "sha256": sha256(args.rust_bin),
                "version": command([str(args.rust_bin), "--version"]),
            },
        },
        "dependencies": dependency_revisions(args.build_root),
        "datasets": dataset_fingerprints(args.data, args.manifests),
        "upstream_pin": (ROOT / "upstream/PIN.txt").read_text() if (ROOT / "upstream/PIN.txt").exists() else None,
        "loadavg": list(os.getloadavg()),
        "nproc": os.cpu_count(),
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
