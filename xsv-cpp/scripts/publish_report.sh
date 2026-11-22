#!/usr/bin/env bash
# Copy the latest formal run's narrative, auto tables and figures into report/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN="${1:-}"
if [ -z "$RUN" ]; then
  RUN="$(find results/runs -maxdepth 1 -type d -name '*-formal' | sort | tail -1)"
fi
if [ -z "$RUN" ] || [ ! -d "$RUN" ]; then
  echo "no formal run directory found" >&2
  exit 2
fi

mkdir -p report/figures
EXTRA=()
[ -f results/d4/timing.jsonl ] && EXTRA+=(--d4-raw results/d4)
[ -f results/cpu-metrics/summary.md ] && EXTRA+=(--cpu-summary results/cpu-metrics/summary.md)
python3 scripts/generate_narrative.py --run "$RUN" --out report/REPORT.md "${EXTRA[@]}"
cp -f "$RUN"/report.md report/auto-report.md
cp -f "$RUN"/complexity.json report/complexity.json 2>/dev/null || true
cp -f "$RUN"/figures/*.svg report/figures/ 2>/dev/null || true
cat > report/README.md <<'MD'
# Formal results

- `REPORT.md` — paper-style report generated from the raw run.
- `auto-report.md` — generated tables for wall time, RSS, throughput, scaling,
  ablation and complexity.
- `figures/` — figures 1–5 generated from the same raw files.
- `complexity.json` — static LOC/dependency/test metrics and clean-build time.
- Appendix A in `REPORT.md` — 100M-row D4 subset from `results/d4/`
  (stats rerun after the memory fix).
- Appendix B in `REPORT.md` — TSC/getrusage CPU indicators from
  `results/cpu-metrics/` (perf counters are blocked on the reference host).

The raw run directory is under `results/runs/<timestamp>-formal/` and is
intentionally not committed: it contains machine-specific measurements,
environment fingerprint and JSONL records. Re-run with
`scripts/run_formal.sh --profile standard`.
MD
echo "published $RUN -> report/REPORT.md"
