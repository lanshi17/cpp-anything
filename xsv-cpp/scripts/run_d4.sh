#!/usr/bin/env bash
# 100M-row D4 subset. Base is excluded: its read-whole-file path needs ~38 GB.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DATA="datasets/data/xlarge_uniform.csv"
[ -f "$DATA" ] || { echo "missing $DATA; run scripts/generate_datasets.py --profile full" >&2; exit 2; }
[ -x build/all/xsv ] || { echo "missing build/all/xsv" >&2; exit 2; }
[ -x upstream/xsv/target/release/xsv ] || { echo "missing Rust reference" >&2; exit 2; }

# D4.count must not see a stale index: xsv count turns into an O(1) index read.
rm -f "$DATA.idx"
python3 scripts/run_benchmarks.py \
  --matrix evaluation/performance/d4.json \
  --profile standard \
  --rust-bin upstream/xsv/target/release/xsv \
  --cpp-variant all=build/all/xsv \
  --runs "${RUNS:-3}" --warmup "${WARMUP:-1}" --memory --memory-runs "${MEMORY_RUNS:-1}" \
  --out "${OUT:-results/d4}"
