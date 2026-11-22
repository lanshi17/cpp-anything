#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT/build"
g++ -O2 -std=c++23 -Wall -Wextra -o "$ROOT/build/cpu-metrics" "$ROOT/tools/cpu_metrics.cpp"
echo "$ROOT/build/cpu-metrics"
