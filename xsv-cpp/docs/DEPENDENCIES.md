# Dependencies and versions

Every number in the final report is tied to this table. A version bump is a
separate commit that reruns the entire correctness gate and the smoke benchmark;
mixing a dependency bump with a code change makes the attribution unfalsifiable.

## 1. Toolchain

| component | pinned / tested | minimum | role |
| --- | --- | --- | --- |
| OS | Ubuntu 26.04 (resolute), kernel 6.x | Ubuntu 24.04 | fixed runner |
| GCC | **15.2.0** (Ubuntu 15.2.0-16ubuntu1) | 14 | only C++ compiler |
| CMake | **4.2.3** | 3.28 | configure, presets, CTest |
| Ninja | **1.13.2** | 1.11 | build |
| Python | **3.14.4** | 3.11 | dataset generator, replay, measurement |
| Rust / Cargo | **1.99.1** (2026-10-13) | 1.85 | build the reference xsv |
| Hyperfine | **2.0.0** | 1.18 | optional independent timing check |
| perf | **7.0.14** | 6.8 | hardware counters |
| heaptrack | 1.5+ (optional) | 1.4 | allocation profiles |
| Valgrind/Massif | 3.22+ (optional) | 3.20 | heap profiles |

ASan/UBSan instrument only `xsv_cpp` translation units: UBSan breaks Abseil's
constexpr function-pointer checks, so instrumenting the dependency tree would
make the ASan build uncompilable. TSan is the exception: oneTBB is built with
TSan as well, because an uninstrumented scheduler hides its own synchronization
and produces false races inside TBB instead of checking the code under test.

GCC is enforced in `CMakeLists.txt`: any other compiler is a configure-time
error. This is deliberate. Clang/GCC comparisons would add a compiler axis to a
language/optimization experiment and make the report unreadable.

## 2. Linked C++ dependencies

| library | version | commit | license | why |
| --- | --- | --- | --- | --- |
| [oneTBB](https://github.com/oneapi-src/oneTBB) | `v2022.3.0` | `f1862f38f83568d96e814e469ab61f88336cc595` | Apache-2.0 | task parallelism, `parallel_for`, `global_control` |
| [RE2](https://github.com/google/re2) | `2025-11-05` | `927f5d53caf8111721e734cf24724686bb745f55` | BSD-3-Clause | linear-time regex, closest analogue of Rust `regex` |
| [Abseil](https://github.com/abseil/abseil-cpp) | `20260817.0` | `2065f4ded0558c6f89fee67c8e5228feb4eb960e` | Apache-2.0 | required by RE2 |
| [fast_float](https://github.com/fastfloat/fast_float) | `v8.3.3` | `dbeabeeb3e6791cb96994c2a7a084c36e62ccfb2` | MIT OR Apache-2.0 | float parsing matching Rust's grammar |
| [Google Benchmark](https://github.com/google/benchmark) | `v1.10.0` (optional) | `a6b4a9f00b36c19af9997d68a1cf6e0b5b3d032c` | Apache-2.0 | core microbenchmarks only (`XSV_BUILD_MICROBENCH=ON`) |

Acquisition order in `cmake/Dependencies.cmake`:

1. `find_package(TBB CONFIG)`, `find_package(re2 CONFIG)`,
   `find_package(FastFloat CONFIG)` — a distribution package of the exact
   version is preferred;
2. otherwise `FetchContent` from the commit above;
3. `-DXSV_FETCH_DEPS=OFF` turns a missing package into a configure error, which
   is how the offline audit runs.

The fetched source trees are not committed. `scripts/collect_env.py` records the
resolved commit for each dependency by reading the FetchContent stamp files, so
a build cannot silently use another revision.

## 3. Reference project

| item | value |
| --- | --- |
| repository | `https://github.com/BurntSushi/xsv` |
| tag | `0.13.0` (annotated) |
| commit | `2b4cbaa0eecf7b507a612632fe00289b1b358c15` |
| commit date | 2018-05-12 |
| license | Unlicense / MIT |
| build | `cargo build --release` (lockfile from the tag) |
| binary | `target/release/xsv` |
| build toolchain | Rust 1.99.1 for the recorded baseline |

`scripts/fetch_upstream.sh` verifies the tag's commit and writes
`upstream/PIN.txt`:

```
tag=0.13.0
commit=2b4cbaa0eecf7b507a612632fe00289b1b358c15
cargo_lock_sha256=...
rustc=...
binary_sha256=...
built_at=...
```

The upstream `Cargo.lock` is the 2018 lockfile. It pins `csv 1.0.0`,
`csv-core 0.1.4`, `csv-index 0.1.5`, `regex 1.0.x`, `docopt 1.0.0`, and so on.
The exact resolved versions and checksums are part of the reference build, not
of the C++ build.

## 4. External tools (not linked)

| tool | pin | use |
| --- | --- | --- |
| `hyperfine` | 2.0.0 | independent wall-clock cross-check |
| `perf` | distro 7.0.14 | `cycles`, `instructions`, `IPC`, cache misses, `perf record` |
| `/usr/bin/time -v` | coreutils | `Maximum resident set size` |
| `heaptrack` | 1.5+ | allocation counts and peak heap |
| `valgrind --tool=massif` | 3.22+ | heap profile fallback |
| `FlameGraph` (`stackcollapse-perf.pl`, `flamegraph.pl`) | current `master` recorded in env JSON | SVG flamegraphs |
| `py-spy` | optional | profiling the Python dataset generator only |

These tools are not part of the binary and their licenses do not affect the
project's license. Versions and paths are recorded by `collect_env.py`.

## 5. Build matrix and dependency activation

| preset | PMR | SIMD | TBB | hash | mmap | RE2 |
| --- | --- | --- | --- | --- | --- | --- |
| `base` | – | – | – | – | – | ✅ |
| `pmr` | ✅ | – | – | – | – | ✅ |
| `simd` | – | ✅ | – | – | – | ✅ |
| `tbb` | – | – | ✅ | – | – | ✅ |
| `hash` | – | – | – | ✅ | – | ✅ |
| `mmap` | – | – | – | – | ✅ | ✅ |
| `ps` | ✅ | ✅ | – | – | – | ✅ |
| `pt` | ✅ | – | ✅ | – | – | ✅ |
| `st` | – | ✅ | ✅ | – | – | ✅ |
| `pst` | ✅ | ✅ | ✅ | – | – | ✅ |
| `ht` | – | – | ✅ | ✅ | – | ✅ |
| `all` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `native` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ + `-march=native` |
| `asan` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ + ASan/UBSan |
| `tsan` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ + TSan |

RE2 is not an optimization axis; it is required by the `search` command. A
build with `XSV_WITH_RE2=OFF` is a core/unit-test build, not a benchmark
variant.

## 6. Reproducibility rules

1. `upstream/PIN.txt` must exist before any differential run.
2. `build/<variant>/CMakeCache.txt` and `compile_commands.json` are archived with
   the results.
3. The binary SHA-256 and the `--version` string (which includes the variant
   name) are recorded per run.
4. Dataset SHA-256 comes from the manifest, not from the file at measurement
   time; a mismatch aborts the run.
5. Network access is not needed during measurement. `XSV_FETCH_DEPS=OFF` proves
   this for the build.
6. A dependency update invalidates every stored result; `scripts/run_benchmarks.py`
   refuses to append to a results file whose environment SHA differs.
