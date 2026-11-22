#!/usr/bin/env bash
# Run the P0 gate and CTest for every built variant. Used before a formal run.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
VARIANTS="${*:-base all pmr simd tbb hash mmap pst}"
for variant in $VARIANTS; do
  if [ ! -x "build/$variant/xsv" ]; then
    echo "missing build/$variant/xsv" >&2
    exit 2
  fi
  printf '%-6s ctest: ' "$variant"
  ctest --test-dir "build/$variant" 2>&1 | grep -Eo '[0-9]+% tests passed[^,]*' | head -1
  printf '%-6s gate:  ' "$variant"
  python3 scripts/run_correctness.py \
    --cpp-bin "build/$variant/xsv" \
    --rust-bin upstream/xsv/target/release/xsv \
    --gate p0 2>&1 | grep 'cases matched'
done
