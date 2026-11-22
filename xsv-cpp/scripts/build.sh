#!/usr/bin/env bash
# Configure+build one preset, then run its tests unless --no-test is given.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${1:-}"
if [ -z "$PRESET" ]; then
  echo "usage: scripts/build.sh <preset> [--no-test] [--configure-only] [-j N]" >&2
  echo "presets: base pmr simd tbb hash mmap ps pt st pst ht all native asan tsan" >&2
  exit 2
fi
shift || true

TEST=1
CONFIGURE_ONLY=0
JOBS="$(nproc)"
while [ $# -gt 0 ]; do
  case "$1" in
    --no-test) TEST=0 ;;
    --configure-only) CONFIGURE_ONLY=1 ;;
    -j) JOBS="$2"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

cd "$ROOT"
cmake --preset "$PRESET"
if [ "$CONFIGURE_ONLY" -eq 1 ]; then
  exit 0
fi
cmake --build "build/$PRESET" -j "$JOBS"

if [ "$TEST" -eq 1 ]; then
  ctest --test-dir "build/$PRESET" --output-on-failure
fi
