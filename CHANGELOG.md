# Changelog

## Unreleased

### Benchmark sampling

- `bench --samples N` runs the original and the port in N separate processes,
  alternates which side goes first, and reports median, P95, min/max and the
  speedup of the medians. The JSON schema is now `cpp-anything/bench@2`.
- `gate --bench` uses three process samples by default, so a single turbo or
  scheduler outlier no longer decides whether the port is "done".
- `bench` output now labels the unit (`ns`, `µs`, `ms`) and shows P95 instead of
  a bare number.

### Rust source language

- `--lang rust` (and `Cargo.toml` detection) analyzes free functions in a library
  crate: integers, floats, bool, `String`/`&str`, `Vec<T>`/`&[T]`, `Option<T>` and
  `HashMap<String, T>`. Generics, traits, impl methods, `async`/`const`/`unsafe`,
  `Result` and tuples are skipped with the reason.
- `record` builds a small Cargo harness that depends on the original crate and
  calls the functions; `bench` reuses it. Panics are recorded as the error type
  `panic` with the message.
- `shadow` emits a Cargo crate that re-exports the original functions, runs the
  C++ port over the C ABI, and logs value/error disagreements.

### The tool is C++23

- The Rust crate is gone: `cpp-anything` is a CMake/Ninja C++23 project with the
  same CLI, MCP server and golden outputs. No third-party dependencies: JSON,
  argv parsing, the compiler matrix and the language plugins are first-party.
- The implementation is built as C++20 modules (`src/modules/*.cppm`,
  `import std;`). Shared leaves (`cppan.args`, shadow paths in `cppan.util`) keep
  the module graph acyclic; only the JSON codec stays a header because it is also
  embedded into generated projects.
- Requires GCC 15+ (the `std` module), Clang 18+ with libc++ modules, or MSVC
  19.44+, and Ninja for module scanning. Generated projects keep the lower
  GCC 13+/Clang 17+/MSVC 19.33+ floor.

## 0.4.0

### Source language plugins

- TypeScript/JavaScript, Go, Ruby and Java each get an analyzer, a recorder and a
  shadow call mapping; the loop, wire format, app, map and gate were already
  language-independent.
- Analyzers map each language's types into the same dialect and report every
  unsupported construct (generics, variadics, structs, arrays, multiple results,
  keywords, symbols) as skipped with a reason.
- Recorders run the original out of process: a Node harness, a `reflect`-based Go
  harness inside the module, a Ruby `load` harness, and a generated `javac` program.
  Errors are recorded by the runtime's own exception class name.
- Bindings over the one C ABI: Node-API addon for TS/JS, cgo for Go, Fiddle (plus an
  optional magnus extension) for Ruby, JNI for Java. Each returns the original's
  value first and logs disagreements.
- `--keep name:reason`, `--as-json T`, `--kind napi`, `--node-arg`,
  `--resolve-from` and `--module` are implemented for TS/JS and rejected with a
  reason elsewhere; `project.json` persists the Node options across `record`.
- PHP and C# are whole-app only: the analyzer lists units, but there is no per-unit
  recorder.

### Whole-app

- `app front-door --library`: `cpp/lib` re-exports every port under `cppan_api`
  (`include/cppan/api.hpp`, `units.cmake`, static `cppan_api`), with no entry point.
- Native HTTP routes: `app wire <unit> --route "GET /path"` (`{param}`, `*`,
  any-method paths) generates a router and a decoded request/response handler stub;
  a handler returning false proxies that request and is counted in the fallback log.
- `app lift` for servers: replays with a fallback log, refuses while any request is
  proxied, then rebuilds with `CPPAN_LIFTED` (no original process; unwired paths 404).

### Profiling

- `profile` dispatches per runtime: Node `--cpu-prof`, Ruby TracePoint tracer, Go
  `go test -cpuprofile` + `go tool pprof -raw`, Java JFR `jdk.ExecutionSample`;
  Python stays on cProfile.

### Fixes

- `project.json` accepts relative source/package paths (and resolves them against the
  project directory), so fixtures and moved checkouts keep working.
- `--lang` help and error messages list the plugin languages; the analyze plan names
  the file to bridge for non-Python units.
- `rsi check` now gates every plugin fixture, rebuilds the library front door, and
  proves the server fixture lifts with native routes.

## 0.3.0

### C++20 modules

- `bridge --modules auto|on|off` (default `auto`) generates a real module unit:
  `src/port.cppm` (`export module <ns>;`, `export namespace <ns>`) instead of
  `include/<ns>/port.hpp`, `src/port.cpp` as the implementation unit, and
  `import <ns>;` in the dispatch, batch and shadow translation units.
- Generated CMake compiles the interface once with `-fmodules-ts` into `gcm.cache/`
  for GCC (its CMake scanner writes the BMI where a second translation unit cannot
  find it) and makes every importer depend on the compiled module; MSVC and Clang
  use CMake's scanner. Building needs Ninja; `doctor` reports it.
- Curated support minimums ahead of the matrix's "partial" rows: gcc 15+, clang 17+,
  apple-clang 16+, msvc 19.36+, C++20 or later. `on` fails with the missing piece;
  `auto` falls back to a header and says so.
- `import std;` is not emitted: GCC 15 has no prebuilt std module.
- Verified end to end: `examples/c-text` bridged to gcc 15 with modules builds,
  replays 75/75 cases and passes `gate`.
- `gate`'s stub check ignores the generated comment that mentions `CPPAN_STUB`.

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

### C-style C++ -> modern C++

- `--lang cpp` (aliases `c++`, `cxx`, `c-style`, `modernize`): analyze, bridge,
  record, diff, bench and gate for C++ files written in a C style. The analyzer maps
  every C idiom it finds to the C++20/23 facility that replaces it (and the
  feature-test macro that facility needs); the generated README restates the policy;
  `gate` adds a `modern` check that refuses C casts, `NULL`, `malloc`/`free`, raw
  `new`/`delete`, `char*` string calls, `printf` and `typedef struct` in
  `src/port.cpp` unless annotated `// C-STYLE: why`.
- The generated CMake now sets `POSITION_INDEPENDENT_CODE` on the port library so the
  shadow and batch libraries can link it; bench prints its numbers on stderr so an
  original that prints cannot corrupt the measurement.

### Source languages

- C: `analyze --lang c` (top-level definitions with supported signatures),
  `bridge --lang c` (a C++ project whose port is the rewrite), `record` through a
  generated harness that links the original with renamed symbols and forks per case
  (a segfault is recorded as a crash and accepts any port behaviour), `diff`, `bench`
  and `gate`. Signatures outside scalars/`char*`/`void` are reported as skipped.

### Side effects

- `app record --effects http,smtp` runs local sinks: an HTTP proxy that answers every
  request and records it, and an SMTP server that accepts a message and records the
  envelope and body. `--effect-env VAR=http-url|smtp-host|smtp-port|smtp-url` points
  the app's own settings at them. Replay compares the effects in order; sink ports
  are masked, and the normalizers apply.
- `--db-reset` runs before each case and `--db-dump` after, both in the case's
  sandbox; the dump is stored with the effects so what the app wrote is part of the
  contract.

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

## Unreleased — cpp-anything is C++

cpp-anything has been rewritten in C++23. The Rust implementation is gone; the
tool, its JSON codec and the generated projects are now one C++ codebase.

- **Build**: `cmake -S . -B build -G Ninja && cmake --build build`; tests run under
  CTest (`ctest --test-dir build`). The tool itself is built as C++20 modules and
  uses `import std;`, so it requires GCC 15+ / Clang 18+ with libc++ modules /
  MSVC 19.44+, plus Ninja for module scanning.
- **No behaviour change by design**: recorded CLI output, generated projects,
  `cases.jsonl`, shadow bindings and `rsi check` are byte-compatible with 0.4.0.
- **Portability**: process execution has a POSIX and a Windows implementation;
  the shared JSON codec reproduces Rust's `{:?}` float text exactly.
