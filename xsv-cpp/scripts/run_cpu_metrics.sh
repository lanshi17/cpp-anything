#!/usr/bin/env bash
# perf_event_paranoid=4 blocks perf; use the TSC/wait4 helper instead.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
scripts/build_cpu_metrics.sh >/dev/null
for variant in base all; do
  [ -x "build/$variant/xsv" ] || { echo "missing build/$variant/xsv" >&2; exit 2; }
done
python3 scripts/run_cpu_metrics.py \
  --rust-bin upstream/xsv/target/release/xsv \
  --cpp-variant base=build/base/xsv \
  --cpp-variant all=build/all/xsv \
  --runs "${RUNS:-7}" --warmup "${WARMUP:-1}" \
  --out "${OUT:-results/cpu-metrics}"
