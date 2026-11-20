#!/usr/bin/env python3
"""Replays tests/golden/cli.jsonl through a build of cpp-anything.

Each line records a command's argv and the stdout the Rust reference produced.
The C++ port is done with a case when the bytes agree; `--verbose` prints the
first differing line for every failure.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile


def normalize(text: str) -> str:
    """The same normalization `cpp-anything rsi record` applies: no trailing
    newline, no trailing spaces, and stable text for the temp dir and sync date."""
    text = text.replace(tempfile.gettempdir(), "<tmp>")
    lines = []
    for line in text.splitlines():
        if line.startswith("#define CPPAN_MATRIX_GENERATED_AT"):
            lines.append('#define CPPAN_MATRIX_GENERATED_AT "<date>"')
        else:
            lines.append(line.rstrip())
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    binary, golden = argv[1], argv[2]
    verbose = "--verbose" in argv
    failures = 0
    total = 0
    with open(golden, encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            case = json.loads(line)
            total += 1
            run = subprocess.run(
                [binary, *case["argv"]], capture_output=True, text=False, check=False
            )
            got = normalize(run.stdout.decode("utf-8", "replace"))
            expected = case["stdout"]
            if got == expected:
                continue
            failures += 1
            print(f"FAIL {case['name']}: {' '.join(case['argv'])}")
            if verbose:
                want = expected.splitlines()
                have = got.splitlines()
                for index in range(max(len(want), len(have))):
                    left = want[index] if index < len(want) else "<missing>"
                    right = have[index] if index < len(have) else "<missing>"
                    if left != right:
                        print(f"  line {index + 1}\n  - {left!r}\n  + {right!r}")
                        break
            else:
                print(f"  expected {len(expected)} bytes, got {len(got)}")
    print(f"{total - failures}/{total} golden cases match")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
