#!/usr/bin/env bash
# Write a SHA-256 index for one formal run and its published report.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RUN="${1:-}"
if [ -z "$RUN" ] || [ ! -d "$RUN" ]; then
  echo "usage: scripts/archive_run.sh results/runs/<timestamp>-formal" >&2
  exit 2
fi
{
  find "$RUN" -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum
  if [ -f report/REPORT.md ]; then
    sha256sum report/REPORT.md report/auto-report.md report/complexity.json report/figures/*.svg
  fi
} > "$RUN/SHA256SUMS"
cp "$RUN/SHA256SUMS" report/SHA256SUMS
echo "wrote $RUN/SHA256SUMS and report/SHA256SUMS"
