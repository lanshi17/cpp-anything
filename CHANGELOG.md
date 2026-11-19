# Changelog

## 0.2.0

Full parity with the reference tool's loop, for a C++ target.

### Measurement and planning

- `profile` — runs the workload under the language's own profiler, ranks hot
  functions with callers, and advises batch vs port-with-callers vs third-party.
- `analyze --whole-app` — the app's shape, entry points, every dependency with its
  C++ library and fit (`good`/`partial`/`none`/`unknown`), units in dependency waves
  with readiness and priority, `keep`/`blocked` status, and the decisions to confirm.
- `data/libs.json` — the curated Python -> C++ library table; unknown packages are
  reported as unknown, never guessed.

### Batch API

- `bridge --batch function:param` / `refresh --batch` generate a C++ batch class per
  function: the items cross once and stay in C++, `run(*args)` answers for all of
  them split across threads, `rank(*args, min=…)` returns the item indexes best
  first. A Python module binds it over the same C ABI as shadow.

### Recording and verification

- `record --from-tests` — a `sitecustomize` hook captures every call the original's
  own suite makes (any runner), appends them to `cases.jsonl`, and records line
  coverage; `recorded-from.json` fingerprints the source so a stale recording fails
  `gate`.
- `record --float-tolerance` is saved with the project and used by `diff` and `gate`.
- `gate` adds error-path, panic/abort, size, staleness and coverage checks.
- `shadow` — a generated drop-in module that returns the original's result, runs the
  C++ port second through a C ABI, logs disagreements, and reports them.

### Whole app

- `app record` — black-box CLI cases from docs examples, `--inputs`, semantic traps,
  run twice in a sandbox with files/stdin/env, normalisation, unstable-case marking.
- `app replay` — any build against the cases, per-case report, `--case`, JSON.
- `app front-door` — a C++ binary that owns the entry point, routes wired commands,
  execs the original for the rest, with a fallback log and kill switches.
- `app record --http` / `app replay` for servers: the server is started on a free
  port, requests come from docs `curl` lines, `--inputs` and traps, and responses
  are compared by status, content type and body (normalisable). `--fresh`/`--pace`
  supported; the front door for a server is a C++ proxy that owns `PORT` and runs
  the original behind it.
- `app wire` — routes, glue stub, per-unit library linking, kill switch.
- `app lift` — refuses while any case falls back; marks the app standalone at zero.

### Around the loop

- `map` — `cpp/PORT_MAP.md`: ports, cases, last diff, not-ported units, glue that
  still falls back, and which originals changed since they were recorded.
- `setup` — registers the MCP server (and skill) with Claude Code, Codex, Cursor,
  Gemini CLI and OpenCode, preserves other settings, backs up before writing,
  idempotent, `--remove`/`--dry-run`/`--json`.
- `mcp` — 21 tools, 2 prompts, progress notifications and structured error codes.
- `doctor` — cmake, compiler, python3, matrix, guides, the MCP server's own answer,
  and every agent's entry.

### RSI

- The self-check now also records, rebuilds and lifts the shipped CLI example with
  zero fallback, on top of the unit example and the CLI goldens.

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
