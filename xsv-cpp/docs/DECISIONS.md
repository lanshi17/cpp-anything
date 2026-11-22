# Decisions

Each row is a choice that constrains the experiment. Revisit only with evidence.

## Scope and experiment design

| # | decision | reason |
| --- | --- | --- |
| D1 | P0 commands only for v1.0. | Nine commands bound the work and give a complete, comparable functional surface. P1/P2 would add weeks without changing the language-level comparison. |
| D2 | GCC only, version pinned. | A second compiler adds a compiler axis; the report is about language + implementation, not GCC vs Clang. |
| D3 | `base` and `all` are the headline pair; every other build is ablation. | Otherwise algorithm gains would be reported as language gains. |
| D4 | Five orthogonal axes, not one “optimized” switch. | The question is where the gain came from; independent switches answer it. |
| D5 | Portable build is the default; `-march=native` is a separate preset. | Comparing native C++ with portable Rust is invalid; both portable builds are the control. |
| D6 | No behavioral changes, even where the reference is odd. | `--intersect` union behavior, `join --right` column order and the case-folding quirk are part of the target. Fixing them would make differential replay impossible. |

## Implementation

| # | decision | reason |
| --- | --- | --- |
| D7 | C++23, no modules. | GCC module support still complicates CMake/CI; headers keep the build boring. Modules are not part of the hypothesis. |
| D8 | `std::expected<T, Error>` at every fallible boundary; no exceptions. | A command line tool needs exit codes, not stack unwinding; sanitizers stay simple and TBB callbacks cannot leak exceptions. |
| D9 | `csv::Reader` is a byte parser; fields are `string_view`. | The reference is byte-oriented (`ByteRecord`); transcoding would change semantics and add copies. |
| D10 | Unquoted fields are zero-copy; quoted fields are unescaped into one per-record scratch buffer. | Quotes are a minority, so the common path still touches no allocator. A clean quoted field could stay zero-copy, but one code path is easier to verify. |
| D11 | `std::stable_sort` for `sort`, including the parallel path. | Rust's `sort_by` is stable; `std::sort` is not. Stability is observable. |
| D12 | RE2 for `search`. | Closest linear-time, no-backreference engine to Rust `regex`; PCRE2 would accept patterns the reference rejects. |
| D13 | Custom flat hash keeps insertion-ordered duplicate chains. | `join` output order for duplicate keys follows the reference's `Vec<row>`; a plain unordered multimap does not guarantee it. |
| D14 | Index is memory-mapped, not loaded. | 100M records means an 800 MB index; loading it would distort the RSS measurement. |
| D15 | `mmap` and read-into-buffer share one `Mapping` API. | The ablation must change only the I/O policy, not every call site. |
| D16 | AVX2 is runtime-dispatched from a portable binary. | `-march=native` stays a separate axis; a portable binary can still use SIMD on the host. |
| D17 | Parallel reductions use fixed chunks merged in order. | Scheduling must not change float sums or output order. |
| D18 | Rust-compatible float formatting is owned code, not `ostringstream`. | `stats` values are a wire contract; default C++ formatting switches to scientific notation and breaks parity. |
| D19 | `count` is the tracer bullet. | It exercises mapping, parsing, headers, indexing, CLI and the differential harness with the smallest command. |

## Measurement

| # | decision | reason |
| --- | --- | --- |
| D20 | Process-level timing is primary; Google Benchmark is optional and separate. | The reported numbers are end-to-end CLI costs; microbenchmarks explain them, not replace them. |
| D21 | Randomized interleaving inside rounds, not “all Rust then all C++”. | Page cache, turbo and thermal drift bias sequential blocks. |
| D22 | Peak RSS from `/usr/bin/time -v`, allocation counts from heaptrack/PMR. | RSS is what a user pays; allocation counts explain the difference. |
| D23 | Formal runs on fixed hardware; CI is smoke-only. | Shared CI produces publishable-looking noise. |
| D24 | Results carry an environment fingerprint; mismatches refuse to append. | A results file that mixes hosts is worse than no results file. |
| D25 | Failed correctness gate blocks benchmarking. | A fast wrong answer is not a result. |

## Licensing

| # | decision | reason |
| --- | --- | --- |
| D26 | Keep upstream `LICENSE-MIT` and `UNLICENSE`; add attribution. | xsv is dual-licensed; the C++ implementation is derived from its behavior and structure. |
| D27 | Do not vendor upstream source in the repository. | `scripts/fetch_upstream.sh` pins the commit; vendoring would blur provenance and add 400 files of noise. |
| D28 | Third-party licenses are listed with versions in DEPENDENCIES.md. | A binary distribution must be able to reproduce the license set. |
| D29 | Sequential mapped commands drop consumed pages every 8 MiB. | mmap gives zero-copy reads but RSS otherwise tracks the whole file; the window keeps RSS proportional to working memory without changing bytes read. |
| D30 | PMR owns row bytes; row-offset arrays stay in the standard allocator. | A monotonic resource cannot reclaim a growing offset vector; PMR is applied to the large stable payload, not bookkeeping. |

| D15 | Report TSC/getrusage CPU indicators, not IPC, on this host. | `kernel.perf_event_paranoid=4` rejects perf hardware *and* software events. `tools/cpu_metrics.cpp` records wall time, TSC cycles, user/system CPU seconds, faults and context switches; the report labels them as substitutes and does not claim cycles/instructions/IPC. |
| D16 | D4 excludes the `base` build. | `base` reads the whole CSV into owned memory; 100M × 30 is ~38 GB on a 30 GB host. Rust and `all` stream/mmap with an 8 MiB window. D4 therefore measures the feasible production path, and the omission is stated in the report. |
