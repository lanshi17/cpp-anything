# xsv-cpp

A C++23 reimplementation of [BurntSushi/xsv](https://github.com/BurntSushi/xsv)
0.13.0, built as a controlled performance experiment rather than a syntax
comparison.

The baseline is frozen at tag `0.13.0`, commit
`2b4cbaa0eecf7b507a612632fe00289b1b358c15` (2018-05-12). The target is the nine
P0 commands, GCC only, with five independently switchable optimization axes:

| axis | switch | mechanism |
| --- | --- | --- |
| memory | `XSV_WITH_PMR` | `std::pmr` monotonic arenas |
| SIMD | `XSV_WITH_SIMD` | AVX2 byte scanning, runtime dispatched |
| parallelism | `XSV_WITH_TBB` | oneTBB |
| hashing | `XSV_WITH_CUSTOM_HASH` | open-addressing multimap |
| I/O | `XSV_WITH_MMAP` | zero-copy `mmap` input |

`base` (all off) vs `all` (all on) is the language/implementation pair; every
single-axis build is an ablation, not a second language comparison.

<p align="center">
  <img src="docs/assets/xsv-cpp-advantages.gif" alt="24-second summary: C++ wins count, index, select, slice and join; Rust wins search and stats" width="840">
</p>

<p align="center"><sub>24s Remotion summary · <a href="report/REPORT.md">raw numbers and caveats</a> · <a href="docs/assets/xsv-cpp-advantages.mp4">MP4</a></sub></p>

## Status

| area | state |
| --- | --- |
| CMake project, 15 variant presets, sanitizer builds | done |
| CTest: 1 unit binary + 13 CLI smoke tests | done |
| CSV reader/writer, selection grammar, index format | done, unit-tested |
| PMR arena, flat hash, AVX2 scanner, TBB executor, RE2 wrapper, Rust-compatible formatting | done, unit-tested |
| Optimization integration: PMR `RecordStore`, AVX2 field scan, TBB stable sort, TBB indexed stats, flat-hash join, mmap | done |
| CLI + all nine P0 commands end-to-end | done, differential-tested |
| `headers`, `select`, `slice`, `search`, `sort`, `stats`, `join` | done, P0 gate green |
| dataset generator, differential runner, benchmark runner, report generator | done; formal standard-profile run and report generated |

The honest one-line version: **all nine P0 commands are implemented, the five
optimization axes are integrated, 144/144 differential cases pass on every
reported build, and a formal standard-profile measurement has been run.** See
[`report/REPORT.md`](report/REPORT.md) for the paper-style report and
[`report/auto-report.md`](report/auto-report.md) for the generated tables.

## Formal result (standard profile)

The formal run is in `results/runs/20261120T102645Z-formal/`; the narrative is
[`report/REPORT.md`](report/REPORT.md). Headline time ratios (`all`/Rust,
lower is faster): count 0.75, index 0.89, select 0.94, search 1.98, sort 1.25,
join 0.59, stats 1.16, slice 0.68. Peak RSS for streaming commands dropped from
~4.1 GB (`base`) to 13–27 MB (`all`) after the 8 MiB mapped-page window; sort is
645 MB (`all`) vs 624 MB (Rust), join 67 MB vs 28 MB. `perf` counters are
blocked by `kernel.perf_event_paranoid=4`, so IPC/cycles are not claimed.

The TSC/getrusage substitute (`scripts/run_cpu_metrics.sh`) confirms the same
shape: on the 10M-row count, `all` uses 2.23 s of CPU versus Rust 2.95 s and
`base` 8.03 s, while peak RSS falls from 4.10 GB to 24.9 MB; select is parity
(0.201 vs 0.210 s), search (0.666 vs 0.335 s), sort (0.757 vs 0.595 s) and stats
(0.928 vs 0.800 s) are slower, and join is faster (0.196 vs 0.338 s). These are
CPU-side indicators (wall, user/system seconds, faults, context switches, TSC
effective GHz), not IPC or retired instructions. Both join implementations are
serial; the C++ gain comes from the PMR-backed flat hash table.

The 100M-row D4 subset (`n=3`, 40.4 GB, base omitted) is mixed: count 25.6 s vs
Rust 34.1 s, index 28.9 vs 33.9 s, select 32.3 vs 36.1 s and slice 2.3 vs
3.5 ms, while search is 101.3 vs 55.4 s and stats 29.7 vs 12.9 s. Parallel
stats initially reached 25.4 GB RSS because worker chunks did not advise away
mapped pages; after the fix it peaks at 186 MB with unchanged timing. Sort and
join at 100M exceed the memory budget and are not run.

## C++ advantages in this refactor

The defensible claim is not “C++ is faster”. It is that **C++ gives the port
direct control over the data path**, and xsv shows where that control pays.
`base` is the C++ control with every axis off; `all` enables mmap, PMR, AVX2,
TBB and the flat hash table.

| lever | xsv example | measured effect | boundary |
| --- | --- | --- | --- |
| Zero-copy I/O (`mmap` + 8 MiB page window) | `count`, `index`, `select`, `slice` | D4 100M: count 25.6 s vs Rust 34.1 s, index 28.9 vs 33.9 s, select 32.3 vs 36.1 s, slice 2.3 vs 3.5 ms. The mmap axis alone is 2.21× over `base` on select and 125× on indexed slice. | C++ D4 streaming RSS is 237–259 MiB vs Rust 4.6–4.9 MiB; Rust still owns the memory result. |
| PMR arenas (`std::pmr`) | `RecordStore`, join build side | 10M count peak RSS falls from 4.10 GB (`base`) to 24.9 MB (`all`); the PMR axis is 1.08× on select and 1.07× on join over `base`. | This beats the naive C++ baseline, not Rust's buffered reader. Allocator lifetime rules remain manual. |
| AVX2 field scanning | CSV field/quote scan | SIMD axis alone is 1.21× on select, 1.06–1.07× on stats/sort over `base`; runtime-dispatched, so portable builds still run. | Regex and numeric aggregation dominate in `search`/`stats`, where SIMD is not the bottleneck. |
| oneTBB parallelism | indexed `stats`, stable `sort` | Indexed stats scales 1,053 ms → 122 ms from 1 to 32 threads (8.66×); the TBB axis is 1.16× on sort over `base`. | Rust scales slightly better on the same case (9.28×) and remains faster in absolute time. |
| Flat open-addressing hash | `join` | `all` join is 0.59× Rust; the hash axis alone is 1.27× over `base`. | Join is serial on both sides; the gain is the data structure, not parallelism. |
| Explicit build/toolchain control | CMake variants, sanitizers, ABI | 5 direct dependencies, 16.5 s clean build, ~2.0 MB binary; ASan/UBSan/TSan builds share the same source. | 5,634 source LOC and roughly 60 person-days; control is paid for in engineering cost. |

The pattern is consistent: **C++ wins when the bottleneck is bytes, layout or a
specialized container; it loses when the bottleneck is regex/statistics
semantics.** In this refactor `all` beats Rust on count, index, select, slice
and join, while Rust wins search (1.98× standard, 1.83× at 100M), sort (1.25×)
and stats (1.16× standard, 2.30× at 100M). `base` loses broadly, which is the
control that prevents calling the language itself the cause.

So C++ is the better choice for an xsv-like rewrite only under explicit
conditions:

1. the workload is dominated by large sequential scans, indexing or output;
2. the team can own allocator, SIMD, hash-table and parallel-runtime code;
3. the benchmark includes the commands that actually benefit, not only a
   favorite microbenchmark;
4. the maintenance budget covers the larger code surface: 5,634 C++ source
   lines plus 732 header lines, versus 4,164 Rust source lines, and 5 direct
   dependencies.

If those conditions do not hold, the mature Rust implementation is the safer
default. The report's conclusion is deliberately mixed because the data is:
this is a set of **implementation wins**, not a language verdict.

## Quick start

Requirements: GCC 14+ (tested 15.2.0), CMake 3.28+ (tested 4.2.3), Ninja,
Python 3.11+, and Rust for the reference binary. First configure fetches pinned
oneTBB, RE2, Abseil and fast_float sources unless system packages provide them.

```sh
# 1. C++ build (all axes, portable)
cmake --preset all
cmake --build --preset all -j
ctest --preset all

# 2. Reference binary, pinned
scripts/fetch_upstream.sh
# -> upstream/xsv/target/release/xsv, upstream/PIN.txt

# 3. P0 differential gate (all nine commands)
python3 scripts/run_correctness.py \
  --cpp-bin build/all/xsv \
  --rust-bin upstream/xsv/target/release/xsv \
  --gate p0

# 4. Smoke dataset + benchmark + report
python3 scripts/generate_datasets.py --profile smoke --force
python3 scripts/run_benchmarks.py \
  --rust-bin upstream/xsv/target/release/xsv \
  --cpp-variant all=build/all/xsv --profile smoke --out results/raw
python3 scripts/generate_report.py --raw results/raw --out results
```

Convenience wrapper:

```sh
scripts/build.sh base          # or pmr, simd, tbb, hash, mmap, ps, pt, st, pst, ht, all, native, asan, tsan
scripts/build.sh all
```

Formal measurement (standard datasets, 20 runs, ablation, 1–32 thread scaling,
complexity and report):

```sh
# build the variants once
scripts/run_formal.sh --profile standard --build

# or, after they are built
scripts/run_formal.sh --profile standard
# -> results/runs/<timestamp>-formal/report.md

# optional, separate measurements
scripts/run_cpu_metrics.sh         # TSC/getrusage substitute when perf is blocked
scripts/run_d4.sh                  # 100M-row count/index/select/search/stats/slice

scripts/publish_report.sh          # copies the narrative, tables, appendices and figures to report/
```

## Repository map

```
cmake/                 dependency pins, flags, sanitizers, variants
include/xsv/           public interfaces
src/                   implementation; src/commands/ has one unit per command
tests/                 CTest unit suite + CLI fixtures
evaluation/            differential cases and benchmark matrix
scripts/               upstream pin, data, replay, measurement, report
docs/                  PLAN, ARCHITECTURE, COMPATIBILITY, BENCHMARKS, DEPENDENCIES, DECISIONS
licenses/              upstream MIT/Unlicense texts
```

Key documents:

- [docs/PLAN.md](docs/PLAN.md) — task breakdown, dependencies, estimates, gates.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module graph, data structures,
  lifetimes, parser state machine.
- [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) — exact P0 behaviour and the
  differential oracle.
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md) — datasets, B01–B12, ablation,
  protocol, statistics.
- [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) — versions, commits, licenses.
- [docs/DECISIONS.md](docs/DECISIONS.md) — why the constraints exist.

## Build variants

| preset | PMR | SIMD | TBB | hash | mmap |
| --- | --- | --- | --- | --- | --- |
| `base` | – | – | – | – | – |
| `pmr` / `simd` / `tbb` / `hash` / `mmap` | one axis | | | | |
| `ps` / `pt` / `st` / `ht` | pairs | | | | |
| `pst` | ✅ | ✅ | ✅ | – | – |
| `all` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `native` | all + `-march=native` | | | | |
| `asan` / `tsan` | all + sanitizer, LTO off | | | | |

Each binary prints its variant:

```
$ build/all/xsv --version
xsv-cpp 0.1.0 (xsv 0.13.0 compatible, variant all)
```

## Verification

Current evidence (run on the pinned toolchain):

```
$ ctest --preset all
100% tests passed, 0 tests failed out of 14

$ build/all/xsv count tests/data/basic.csv
3

$ python3 scripts/run_correctness.py \
    --cpp-bin build/all/xsv \
    --rust-bin upstream/xsv/target/release/xsv \
    --gate p0
144/144 cases matched in 0.11s
  count    32/32
  headers  12/12
  select   16/16
  slice    14/14
  search   10/10
  index    12/12
  sort     12/12
  stats    17/17
  join     19/19
```

The differential runner compares stdout bytes, exit codes, `-o` output files and
help text; stderr is compared by class. See COMPATIBILITY.md §13.

## License

The C++ implementation is dual-licensed under MIT or the Unlicense, matching the
upstream project. Upstream copyright and license texts are in `licenses/`; see
[ATTRIBUTION.md](ATTRIBUTION.md).
