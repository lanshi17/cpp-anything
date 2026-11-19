# Changelog

## 0.1.0

First release: the loop, the matrix and the self-check.

### The loop

- `analyze` — Python modules, functions, annotations and risks, as text or
  `cpp-anything/analyze@1` JSON.
- `bridge` — a C++20/23 project per unit: `port.hpp`, stub `port.cpp`, generated
  dispatch, JSON driver, CMake, `profile.hpp`, README.
- `record` — generated edge-case corpus plus seeded random arguments, recorded
  through the original's own interpreter; every return value and exception type.
- `diff` — replays every case through the C++ driver and compares; reports args,
  expected, got and the reason for each mismatch.
- `bench` — per-call timing on both sides, from the same cases.
- `gate` — matrix, no stubs, build, dispatch, diff (and optional bench) in one exit code.
- `refresh` — regenerates the contract after a signature change, never `port.cpp`.

### Compiler and standard matrix

- 651 features across C++11–C++26 from cppreference's compiler support tables, with
  first release per GCC, Clang/libc++, MSVC/MSVC STL, Apple Clang, EDG, ICC, NVCC.
- `matrix features`, `matrix explain`, `matrix profile`, `matrix docs`, `matrix check`.
- Generated guides: `docs/guides/**` for GCC, Clang/libc++, MSVC, LLVM, every
  standard, the full matrix and a toolchain-selection page.
- `scripts/sync_matrix.py` — fetch, parse, provenance (revision id + SHA-256);
  `--offline` parses the cache and keeps the last online provenance.

### Generated C++

- `cppan::Result<T>`: `std::expected` on C++23 targets, an equivalent for C++20.
- Dependency-free JSON codec that keeps ints and reals apart, round-trips `NaN`,
  `Infinity` and `-0.0`, and compares objects unordered.
- `profile.hpp` with `CPPAN_HAS_*` per facility from the matrix, plus a
  `static_assert` on the standard.
- Warnings on by default; `/Zc:__cplusplus` on MSVC.

### RSI and tooling

- `rsi check` — matrix provenance, guide drift, recorded CLI goldens, and a rebuilt
  example whose 105 recorded cases must match.
- `rsi record` — re-record the CLI goldens.
- `doctor` — cmake, C++ compiler, python3, matrix, guides.
- `mcp` — JSON-RPC over stdio: `matrix_features`, `matrix_explain`, `matrix_profile`,
  `analyze`, `bridge`, `record`, `build`, `diff`, `bench`, `gate`, `doctor`.
- `examples/textkit-cpp` — a complete port with its recording, and the fixture
  `tests/fixtures/python/textkit` it was generated from.
