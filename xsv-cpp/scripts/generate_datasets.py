#!/usr/bin/env python3
"""Generate deterministic CSV datasets for the xsv-cpp experiment.

The generator is deliberately dependency-free (Python standard library only) and
version-independent: it implements its own SplitMix64 PRNG, so CPython's
``random`` implementation cannot change a corpus between releases.

Every file is written once, hashed while it is written, and described by a JSON
manifest. Re-running the same profile/seed must produce byte-identical files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Iterator

MASK64 = (1 << 64) - 1
GENERATOR_VERSION = 1
DEFAULT_SEED = 20261120


class Rng:
    """SplitMix64: tiny, deterministic and stable across Python versions."""

    def __init__(self, seed: int) -> None:
        self.state = seed & MASK64

    def next_u64(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)

    def randbelow(self, n: int) -> int:
        if n <= 0:
            raise ValueError("n must be positive")
        limit = (1 << 64) - ((1 << 64) % n)
        while True:
            value = self.next_u64()
            if value < limit:
                return value % n

    def choice(self, items: Iterable[str]) -> str:
        items = tuple(items)
        return items[self.randbelow(len(items))]

    def random(self) -> float:
        return self.next_u64() / float(1 << 64)


def csv_field(value: str) -> str:
    if any(ch in value for ch in ',"\r\n'):
        return '"' + value.replace('"', '""') + '"'
    return value


def write_csv(
    path: Path,
    header: list[str],
    rows: Iterable[Iterable[str]],
) -> tuple[int, int, str]:
    path.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    size = 0
    count = 0
    with path.open("w", encoding="utf-8", newline="") as handle:
        def emit(line: str) -> None:
            nonlocal size
            data = line.encode("utf-8")
            handle.write(line)
            digest.update(data)
            size += len(data)

        emit(",".join(csv_field(name) for name in header) + "\n")
        for row in rows:
            emit(",".join(csv_field(field) for field in row) + "\n")
            count += 1
    return count, size, digest.hexdigest()


def write_bytes(path: Path, data: bytes, rows: int | None = None) -> tuple[int, int, str]:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return (data.count(b"\n") if rows is None else rows), len(data), hashlib.sha256(data).hexdigest()


def text(rng: Rng, length: int) -> str:
    alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_"
    return "".join(alphabet[rng.randbelow(len(alphabet))] for _ in range(length))


CATEGORIES = ("alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta")
CITIES = ("Paris", "London", "Berlin", "Tokyo", "São Paulo", "Nairobi", "Sydney")
NOTES = ("", "ok", "needs review", 'say "hi"', "line\nbreak", "café", "日本語")


def uniform_rows(rng: Rng, count: int, columns: int, seed_offset: int) -> Iterator[list[str]]:
    column_names = [f"c{i}" for i in range(columns)]
    for row in range(count):
        values = [
            str(seed_offset + row),
            str(rng.randbelow(1_000_000)),
            rng.choice(CATEGORIES),
            f"{rng.random() * 1000:.6f}",
            rng.choice(("true", "false")),
            rng.choice(CITIES),
            str(rng.randbelow(100)),
            rng.choice(NOTES),
            f"{rng.next_u64():016x}",
            str(rng.randbelow(10_000)),
        ]
        # For wider schemas, repeat the pattern with a distinct suffix.
        extra = [
            f"v{row}-{i}-{rng.randbelow(1000)}" for i in range(max(0, columns - 10))
        ]
        yield values + extra


def high_card_rows(rng: Rng, count: int) -> Iterator[list[str]]:
    for row in range(count):
        yield [f"key-{row:09d}", f"{rng.next_u64():016x}", rng.choice(CATEGORIES), str(row)]


def low_card_rows(rng: Rng, count: int) -> Iterator[list[str]]:
    for row in range(count):
        yield [rng.choice(CATEGORIES), str(rng.randbelow(8)), rng.choice(CITIES), str(row)]


def skewed_rows(rng: Rng, count: int) -> Iterator[list[str]]:
    for row in range(count):
        # Cube of a uniform value concentrates mass near zero.
        key = int((rng.random() ** 3) * 10000)
        yield [str(key), str(row), rng.choice(CATEGORIES), f"{rng.random():.6f}"]


def null_heavy_rows(rng: Rng, count: int) -> Iterator[list[str]]:
    for row in range(count):
        def maybe(value: str) -> str:
            return "" if rng.randbelow(10) < 8 else value

        yield [
            str(row),
            maybe(str(rng.randbelow(1_000_000))),
            maybe(rng.choice(CATEGORIES)),
            maybe(f"{rng.random():.6f}"),
            maybe(rng.choice(CITIES)),
            maybe(text(rng, 24)),
            maybe(str(rng.randbelow(100))),
            maybe(f"{rng.next_u64():016x}"),
        ]


def wide_rows(rng: Rng, count: int) -> Iterator[list[str]]:
    for row in range(count):
        long_a = text(rng, 1000 + rng.randbelow(3000))
        long_b = text(rng, 1000 + rng.randbelow(3000))
        if rng.randbelow(10) == 0:
            long_a += ',quote " and\nnewline'
        yield [str(row), long_a, long_b, rng.choice(CATEGORIES), str(rng.randbelow(1000)), text(rng, 40)]


def sort_rows(rng: Rng, count: int, order: str) -> Iterator[list[str]]:
    keys = [rng.randbelow(100_000) for _ in range(count)]
    if order == "sorted":
        keys.sort()
    elif order == "reverse":
        keys.sort(reverse=True)
    elif order == "dupes":
        keys = [rng.randbelow(16) for _ in range(count)]
    for row, key in enumerate(keys):
        if order == "long":
            value = text(rng, 200 + rng.randbelow(600))
        else:
            value = f"value-{rng.randbelow(1_000_000)}"
        yield [str(key), str(row), value]


def join_rows(rng: Rng, count: int, key_space: int, skew: bool) -> Iterator[list[str]]:
    for row in range(count):
        if skew:
            key = int((rng.random() ** 3) * key_space)
        else:
            key = rng.randbelow(key_space)
        yield [f"k{key:07d}", str(row), rng.choice(CATEGORIES), text(rng, 16), str(rng.randbelow(1000))]


@dataclass
class Dataset:
    name: str
    profile: str
    distribution: str
    rows: int
    columns: int
    schema: list[str]
    writer: Callable[[Path], tuple[int, int, str]]
    note: str = ""


def dataset_specs(profile: str, seed: int) -> list[Dataset]:
    if profile == "smoke":
        small, medium, large = 5_000, 50_000, 200_000
        join_left, join_right, sort_count = 20_000, 2_000, 20_000
        wide_count = 2_000
    elif profile == "standard":
        small, medium, large = 100_000, 1_000_000, 10_000_000
        join_left, join_right, sort_count = 1_000_000, 100_000, 1_000_000
        wide_count = 100_000
    elif profile == "full":
        small, medium, large = 100_000, 1_000_000, 10_000_000
        join_left, join_right, sort_count = 10_000_000, 1_000_000, 1_000_000
        wide_count = 100_000
    else:
        raise SystemExit(f"unknown profile: {profile}")

    specs: list[Dataset] = []

    def add_uniform(name: str, rows: int, columns: int, dist: str) -> None:
        specs.append(
            Dataset(
                name=name,
                profile=profile,
                distribution=dist,
                rows=rows,
                columns=columns,
                schema=[f"c{i}" for i in range(columns)],
                writer=lambda path, r=rows, c=columns, d=dist: write_csv(
                    path,
                    [f"c{i}" for i in range(c)],
                    uniform_rows(Rng(seed + c), r, c, 0),
                ),
            )
        )

    add_uniform("small_uniform", small, 10, "uniform")
    add_uniform("medium_uniform", medium, 20, "uniform")
    add_uniform("large_uniform", large, 30, "uniform")
    if profile == "full":
        add_uniform("xlarge_uniform", 100_000_000, 30, "uniform")

    specs.append(
        Dataset(
            "high_card", profile, "high-cardinality", medium, 4,
            ["key", "code", "category", "row"],
            lambda path: write_csv(path, ["key", "code", "category", "row"], high_card_rows(Rng(seed + 1), medium)),
        )
    )
    specs.append(
        Dataset(
            "low_card", profile, "low-cardinality", medium, 4,
            ["category", "small_int", "city", "row"],
            lambda path: write_csv(path, ["category", "small_int", "city", "row"], low_card_rows(Rng(seed + 2), medium)),
        )
    )
    specs.append(
        Dataset(
            "skewed", profile, "skewed", medium, 4,
            ["key", "row", "category", "metric"],
            lambda path: write_csv(path, ["key", "row", "category", "metric"], skewed_rows(Rng(seed + 3), medium)),
        )
    )
    specs.append(
        Dataset(
            "null_heavy", profile, "null-heavy", medium, 8,
            ["row", "number", "category", "metric", "city", "note", "small", "code"],
            lambda path: write_csv(
                path,
                ["row", "number", "category", "metric", "city", "note", "small", "code"],
                null_heavy_rows(Rng(seed + 4), medium),
            ),
        )
    )
    specs.append(
        Dataset(
            "wide_rows", profile, "wide-rows", wide_count, 6,
            ["row", "long_a", "long_b", "category", "small", "note"],
            lambda path: write_csv(
                path,
                ["row", "long_a", "long_b", "category", "small", "note"],
                wide_rows(Rng(seed + 5), wide_count),
            ),
        )
    )

    for order in ("random", "sorted", "reverse", "dupes", "long"):
        specs.append(
            Dataset(
                f"sort_{order}", profile, f"sort-{order}", sort_count, 3,
                ["key", "row", "value"],
                lambda path, o=order: write_csv(
                    path, ["key", "row", "value"], sort_rows(Rng(seed + 6), sort_count, o)
                ),
            )
        )

    specs.append(
        Dataset(
            "join_left", profile, "join-left", join_left, 5,
            ["key", "row", "category", "payload", "amount"],
            lambda path: write_csv(
                path, ["key", "row", "category", "payload", "amount"],
                join_rows(Rng(seed + 7), join_left, max(1, join_right * 4), False),
            ),
        )
    )
    specs.append(
        Dataset(
            "join_right", profile, "join-right", join_right, 5,
            ["key", "row", "category", "payload", "amount"],
            lambda path: write_csv(
                path, ["key", "row", "category", "payload", "amount"],
                join_rows(Rng(seed + 8), join_right, max(1, join_right * 4), False),
            ),
        )
    )

    specs.append(
        Dataset(
            "complex_edge", profile, "parser-edge", 5, 6,
            ["id", "name, with comma", "note", "dup", "dup", "empty"],
            lambda path: write_complex_edge(path),
            note="BOM, quoted newline, doubled quotes, unicode, duplicate header names",
        )
    )
    specs.append(
        Dataset(
            "complex_crlf", profile, "parser-edge", 3, 3,
            ["a", "b", "c"],
            lambda path: write_bytes(path, b"a,b,c\r\n1,2,3\r\n4,5,6\r\n", rows=2),
        )
    )
    specs.append(
        Dataset(
            "complex_no_newline", profile, "parser-edge", 1, 2,
            ["a", "b"],
            lambda path: write_bytes(path, b"a,b\n1,2", rows=1),
        )
    )
    specs.append(
        Dataset(
            "complex_empty", profile, "parser-edge", 0, 0,
            [],
            lambda path: write_bytes(path, b"", rows=0),
            note="empty file",
        )
    )
    specs.append(
        Dataset(
            "complex_ragged", profile, "parser-edge", 2, 2,
            ["a", "b"],
            lambda path: write_bytes(path, b"a,b\n1,2,3\n", rows=2),
            note="invalid on purpose; error-path cases only",
        )
    )
    return specs


def write_complex_edge(path: Path) -> tuple[int, int, str]:
    data = (
        "\ufeffid,\"name, with comma\",note,dup,dup,empty\n"
        "1,\"line\nbreak\",\"quote \"\"inside\"\"\",x,y,\n"
        "2,日本語,café,é,é,😀\n"
        "3,,\" a b \",\"\",\"\",\"\"\n"
        "4,\"a,b\",\"multi\nline\nrecord\",z,z,long\n"
        "5,\"\",plain,,,\n"
    ).encode("utf-8")
    return write_bytes(path, data, rows=5)


def manifest_for(dataset: Dataset, path: Path, rows: int, columns: int, size: int, sha: str, seed: int) -> dict:
    return {
        "dataset": dataset.name,
        "profile": dataset.profile,
        "rows": rows,
        "columns": columns,
        "seed": seed,
        "encoding": "UTF-8",
        "header": bool(dataset.schema),
        "bytes": size,
        "sha256": sha,
        "distribution": dataset.distribution,
        "schema": dataset.schema,
        "file": str(path.relative_to(path.parents[1])),
        "generator": "scripts/generate_datasets.py",
        "generator_version": GENERATOR_VERSION,
        "note": dataset.note,
    }


def check_existing(manifest_path: Path, data_path: Path) -> bool:
    if not manifest_path.exists() or not data_path.exists():
        return False
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    digest = hashlib.sha256()
    with data_path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return manifest.get("sha256") == digest.hexdigest()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("datasets"), help="dataset root")
    parser.add_argument("--profile", choices=("smoke", "standard", "full"), default="smoke")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="manifest seed label")
    parser.add_argument("--only", action="append", default=[], help="generate only this dataset (repeatable)")
    parser.add_argument("--force", action="store_true", help="rewrite files even when hashes match")
    parser.add_argument("--list", action="store_true", help="list datasets and exit")
    args = parser.parse_args(argv)

    specs = dataset_specs(args.profile, args.seed)
    if args.only:
        wanted = set(args.only)
        specs = [spec for spec in specs if spec.name in wanted]
        missing = wanted - {spec.name for spec in specs}
        if missing:
            parser.error("unknown dataset(s): " + ", ".join(sorted(missing)))

    if args.list:
        for spec in specs:
            print(f"{spec.name:20s} rows={spec.rows:<12d} cols={spec.columns:<3d} {spec.distribution}")
        return 0

    data_dir = args.out / "data"
    manifest_dir = args.out / "manifests"
    data_dir.mkdir(parents=True, exist_ok=True)
    manifest_dir.mkdir(parents=True, exist_ok=True)

    index: list[dict] = []
    for spec in specs:
        data_path = data_dir / f"{spec.name}.csv"
        manifest_path = manifest_dir / f"{spec.name}.json"
        if not args.force and check_existing(manifest_path, data_path):
            print(f"skip {spec.name} (manifest hash matches)")
            index.append(json.loads(manifest_path.read_text(encoding="utf-8")))
            continue
        rows, size, sha = spec.writer(data_path)
        manifest = manifest_for(spec, data_path, rows, spec.columns, size, sha, args.seed)
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"wrote {spec.name:20s} rows={rows:<12d} bytes={size:<12d} sha256={sha[:12]}…")
        index.append(manifest)

    (manifest_dir / "index.json").write_text(
        json.dumps(
            {
                "profile": args.profile,
                "seed": args.seed,
                "generator_version": GENERATOR_VERSION,
                "datasets": sorted(index, key=lambda item: item["dataset"]),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
