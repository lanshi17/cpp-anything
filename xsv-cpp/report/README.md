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
