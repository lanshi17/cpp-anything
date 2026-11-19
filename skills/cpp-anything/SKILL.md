---
name: cpp-anything
description: Port Python (and, as plugins land, other languages) to C++20/23 with the cpp-anything CLI or MCP tools: analyze a repository, bridge one unit, record the original's behaviour, write the port, diff, bench and gate it. Use when asked to rewrite, port or migrate code or an app to C++ or C++23, to plan such a migration, to make slow Python faster by moving hot code to C++, or to check which compiler release supports a C++ feature.
---

# Migration to C++ with cpp-anything

cpp-anything gives you ground truth; you write the C++. Port one unit at a time and
call a port done only when `gate` passes.

Tools: the `cpp-anything` CLI, or the same steps as MCP tools (`cpp-anything mcp`):
`matrix_features`, `matrix_explain`, `matrix_profile`, `analyze`, `bridge`, `record`,
`build`, `diff`, `bench`, `gate`, `doctor`. If the MCP tools are missing, use the
CLI. If `cpp-anything` is not installed, ask the user to run
`cargo install --path .` (or `cargo build --release`) and wire `cpp-anything mcp`.

## 0. Check the toolchain before promising anything

```sh
cpp-anything doctor
cpp-anything matrix features --toolchain gcc --version 13 --std c++23
```

- The generated code needs C++20 at minimum and prefers C++23 (`std::expected`,
  `std::print`, `std::mdspan`). If the target cannot do C++23, say so and use
  `--std c++20`: the port source is the same, `cppan::Result` replaces
  `std::expected`.
- If the user's compiler is older than the matrix's first complete release, show
  them the numbers (`cpp-anything matrix first-release` is the guides' table) instead
  of writing code that will not compile.

## 1. Plan, then stop

```sh
cpp-anything analyze <repo>            # people
cpp-anything analyze <repo> --json     # you: schema cpp-anything/analyze@1
```

Present the units in "start here" order, and each unit's risks (unicode offsets,
`int()` parsing, float formatting, unbounded ints, regex). Ask which unit to port
first, and stop for approval. Do not start with a unit whose functions cannot be
called as plain functions (decorated, `async`, `*args`): `bridge` reports those.

## 2. Bridge and record

```sh
cpp-anything bridge <repo> <module>     # cpp/<unit>; never overwrites without --force
cpp-anything bridge <repo> <file.c> --lang c
cpp-anything bridge <repo> <file.cpp> --lang cpp   # C-style C++: modernize as you port
cpp-anything record cpp/<unit>          # the original's behaviour -> cases.jsonl
```

- If the generated cases cannot reach the interesting paths, add real inputs with
  `--inputs cases.jsonl` (`{"fn": name, "args": [...]}` per line).
- Keep `cases.jsonl`: it is the characterization suite, and the only thing that
  makes "behaves the same" checkable.

## 3. Write the port

Edit `src/port.cpp` only. Everything else is generated; `cpp-anything refresh`
regenerates declarations and dispatch after a signature change and never touches
your file.

- Signatures: `cppan::Result<T>` returns, `std::string_view` in, `std::string` out,
  `std::span<const T>` for read-only lists, `std::optional` for `T | None`.
- Failures: `return cppan::unexpected(PortError{"ValueError", "..."});` — the type is
  the Python exception class and the diff compares it; the message is not compared
  unless the user asks for `--strict-errors`.
- Read the unit's risks in the generated README at their file:line before writing.
- Unicode: Python counts code points, C++ counts bytes. Every offset that reaches
  output needs a UTF-8 aware helper (see `examples/textkit-cpp/src/port.cpp`).
- Ints: `int(text)` accepts whitespace, signs, underscores and unicode digits;
  `std::from_chars` does not. Python ints are unbounded; `std::int64_t` is not.
- Floats: format with `std::to_chars`/`std::format`, never `std::to_string`.
- Containers: Python dicts become `std::map` (sorted); Python sorts are stable,
  `std::sort` is not.
- Use `CPPAN_HAS_*` from `include/cppan/profile.hpp` for optional facilities; do not
  guess what the compiler has.
- No `unsafe`, no C casts, no `malloc`; the driver owns memory. For `--lang cpp`,
  `gate`'s `modern` check refuses C idioms in `src/port.cpp`: use `string_view`,
  `span`, ranges and the `CPPAN_HAS_*`-gated formatting facilities, or annotate
  `// C-STYLE: why` when one has to stay.

## 4. Diff, then gate

```sh
cpp-anything build cpp/<unit>
cpp-anything diff cpp/<unit>            # every recorded case must match
cpp-anything bench cpp/<unit>           # optional: is it worth shipping?
cpp-anything gate cpp/<unit>            # done means this passes
```

`diff` reports the first mismatches with args, expected and got, and why (error type,
text, length, `-0.0`). Fix the port, not the recording: a mismatch is the original's
behaviour, unless the user says the recording is wrong.

Do not report a port as finished while any step fails. If `gate` fails on
`no stubs`, functions are still unported; on `dispatch`, the driver and the analyzed
signatures disagree (run `refresh`); on `matrix`, the target toolchain cannot express
the port.

## 5. Ship

- The port is a static library plus a driver; wire it into the app where the Python
  call was, or keep the driver as the test harness and call the library directly.
- Keep `cpp-anything gate` in CI for every ported unit.
- When cpp-anything itself needs to change: `cargo test`, `cargo clippy --all-targets`,
  `cpp-anything rsi check` (data, guides, CLI goldens, and a rebuilt example).
