#!/usr/bin/env bash
# End-to-end reproduction on one machine: pin, build, test, smoke measure, report.
# Formal numbers require the full profile and a quiet machine:
#   scripts/reproduce.sh --profile standard
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="smoke"
JOBS="$(nproc)"
while [ $# -gt 0 ]; do
  case "$1" in
    --profile) PROFILE="$2"; shift ;;
    -j) JOBS="$2"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

cd "$ROOT"

echo "== pin and build the Rust reference"
scripts/fetch_upstream.sh

echo "== build C++ base and all"
scripts/build.sh base -j "$JOBS"
scripts/build.sh all -j "$JOBS"

echo "== unit and CLI tests"
ctest --test-dir build/base --output-on-failure
ctest --test-dir build/all --output-on-failure

echo "== datasets ($PROFILE)"
python3 scripts/generate_datasets.py --profile "$PROFILE" --force

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$PROFILE"
RUN_DIR="results/runs/$RUN_ID"
mkdir -p "$RUN_DIR"

echo "== differential (implemented P0 families)"
python3 scripts/run_correctness.py \
  --cpp-bin build/all/xsv \
  --rust-bin upstream/xsv/target/release/xsv \
  --gate p0 \
  --json-out "$RUN_DIR/differential.json"

echo "== environment"
python3 scripts/collect_env.py --out "$RUN_DIR/environment.json"

echo "== benchmark (may be long for standard)"
python3 scripts/run_benchmarks.py \
  --rust-bin upstream/xsv/target/release/xsv \
  --cpp-variant "base=build/base/xsv" \
  --cpp-variant "all=build/all/xsv" \
  --profile "$PROFILE" \
  --memory \
  --out "$RUN_DIR/raw"

echo "== report"
python3 scripts/generate_report.py --raw "$RUN_DIR/raw" --out "$RUN_DIR"

echo "done: $RUN_DIR/report.md"
# Optional, separate from the standard run:
#   scripts/run_d4.sh            # 100M-row count/index/select/search/stats/slice
#   scripts/run_cpu_metrics.sh   # TSC/getrusage substitute when perf is blocked
