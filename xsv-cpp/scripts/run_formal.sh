#!/usr/bin/env bash
# Formal measurement: headline, ablation, thread scaling, complexity, report.
# Assumes base/all and the single-axis variants are built. Set --build to build
# the selected variants first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="standard"
RUNS=20
ABLATION_RUNS=5
SCALING_RUNS=10
BUILD=0
JOBS="$(nproc)"
VARIANT_NAMES=(base all pmr simd tbb hash mmap pst)
while [ $# -gt 0 ]; do
  case "$1" in
    --profile) PROFILE="$2"; shift ;;
    --runs) RUNS="$2"; shift ;;
    --ablation-runs) ABLATION_RUNS="$2"; shift ;;
    --scaling-runs) SCALING_RUNS="$2"; shift ;;
    --build) BUILD=1 ;;
    -j) JOBS="$2"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

cd "$ROOT"
if [ ! -f "datasets/manifests/large_uniform.json" ] || [ ! -f "datasets/data/large_uniform.csv" ]; then
  echo "== generating $PROFILE datasets"
  python3 scripts/generate_datasets.py --profile "$PROFILE"
fi

if [ "$BUILD" -eq 1 ]; then
  for variant in "${VARIANT_NAMES[@]}"; do
    echo "== build $variant"
    cmake --preset "$variant" >/dev/null
    cmake --build "build/$variant" -j "$JOBS" >/dev/null
  done
fi

for variant in "${VARIANT_NAMES[@]}"; do
  if [ ! -x "build/$variant/xsv" ]; then
    echo "missing build/$variant/xsv (use --build)" >&2
    exit 2
  fi
done

RUN_ROOT="results/runs/$(date -u +%Y%m%dT%H%M%SZ)-formal"
mkdir -p "$RUN_ROOT"
echo "results: $RUN_ROOT"

echo "== differential gate"
python3 scripts/run_correctness.py \
  --cpp-bin build/all/xsv \
  --rust-bin upstream/xsv/target/release/xsv \
  --gate p0 \
  --json-out "$RUN_ROOT/differential.json"

echo "== headline (rust / base / all)"
python3 scripts/run_benchmarks.py \
  --profile "$PROFILE" --runs "$RUNS" --warmup 3 --memory \
  --rust-bin upstream/xsv/target/release/xsv \
  --cpp-variant base=build/base/xsv \
  --cpp-variant all=build/all/xsv \
  --out "$RUN_ROOT/formal"

echo "== ablation (medium cases, single axes)"
python3 scripts/run_benchmarks.py \
  --profile "$PROFILE" --runs "$ABLATION_RUNS" --warmup 1 \
  --rust-bin upstream/xsv/target/release/xsv \
  --cpp-variant base=build/base/xsv \
  --cpp-variant pmr=build/pmr/xsv \
  --cpp-variant simd=build/simd/xsv \
  --cpp-variant tbb=build/tbb/xsv \
  --cpp-variant hash=build/hash/xsv \
  --cpp-variant mmap=build/mmap/xsv \
  --cpp-variant pst=build/pst/xsv \
  --cpp-variant all=build/all/xsv \
  --case B03.select.medium --case B05.sort.medium --case B06.join.medium \
  --case B07.stats.medium --case B10.slice.medium \
  --out "$RUN_ROOT/ablation"

echo "== thread scaling (indexed stats)"
python3 scripts/run_benchmarks.py \
  --profile "$PROFILE" --runs "$SCALING_RUNS" --warmup 2 \
  --threads 1,2,4,8,16,32 \
  --rust-bin upstream/xsv/target/release/xsv \
  --cpp-variant base=build/base/xsv \
  --cpp-variant all=build/all/xsv \
  --out "$RUN_ROOT/scaling"

echo "== environment"
python3 scripts/collect_env.py --out "$RUN_ROOT/environment.json"

echo "== complexity"
python3 scripts/collect_complexity.py --out "$RUN_ROOT/complexity.json" --measure-build

# The clean-build timing target removes the test executable; restore it so the
# next `ctest` is meaningful.
cmake --build build/all -j "$JOBS" >/dev/null

echo "== report"
python3 scripts/generate_report.py \
  --raw "$RUN_ROOT/formal" \
  --ablation-raw "$RUN_ROOT/ablation" \
  --scaling-raw "$RUN_ROOT/scaling" \
  --complexity "$RUN_ROOT/complexity.json" \
  --out "$RUN_ROOT"

echo "done: $RUN_ROOT/report.md"
