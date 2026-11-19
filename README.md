<p align="center"><b>Rewrite your app in C++20/23, and prove it still behaves the same.</b></p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#the-loop">The loop</a> ·
  <a href="#compiler-and-standard-guides">Guides</a> ·
  <a href="docs/how-it-works.md">How it works</a> ·
  <a href="docs/porting.md">Porting rules</a> ·
  <a href="docs/tools.md">MCP tools</a> ·
  <a href="docs/README_ZH.md">简体中文</a>
</p>

`cpp-anything` records what your Python app does today, generates a C++23 project
that must do the same, and replays every recorded case through the port until they
all match. It never translates code: you or your agent write the C++, and the tool
supplies what an agent cannot guess reliably — where the edges are, the exact
behaviour to match, and a gate that says when the port is done.

```
$ cpp-anything analyze .                          # what is here, what porting costs
$ cpp-anything bridge . textkit.words             # a C++23 project per unit
$ cpp-anything record cpp/textkit-words           # 105 cases: what the original returns
$ $EDITOR cpp/textkit-words/src/port.cpp          # you (or your agent) write the C++
$ cpp-anything diff cpp/textkit-words             # 105/105 cases match
$ cpp-anything gate cpp/textkit-words             # done, and proven
$ cpp-anything bench cpp/textkit-words            # 333 ns -> 141 ns per call (2.37x)
```

## Quick start

Requirements: CMake 3.28+, a C++23 compiler (GCC 13+, Clang 17+, MSVC 19.33+), and
the Python 3 that runs the code you are porting.

```sh
cargo install --path .        # or: cargo build --release
cpp-anything doctor           # cmake, a C++ compiler, python3, the matrix, the guides
cpp-anything analyze .        # plan; ports one module at a time
```

Then ask your agent (Claude Code, Codex, Cursor, …) to do one unit:

> *"Port `textkit.words` to C++ with cpp-anything. Record the original, write
> `src/port.cpp`, and stop when `gate` passes."*

The CLI and the MCP server (`cpp-anything mcp`) expose the same steps, so an agent
that can run a terminal or call a tool gets the same loop.

## What it fixes for you

| An agent alone | With cpp-anything |
| --- | --- |
| Guesses what the code does from reading it | `record` runs the original on generated edge cases and saves what it returns or raises |
| Writes a port nobody can check | `diff` replays every case through the C++ driver, byte for byte |
| Guesses which C++ features exist | `matrix` answers from cppreference's compiler-support tables, and `profile.hpp` gates every optional facility |
| Says "done" when it compiles | `gate` fails on a stub body, a missing function, a warning-free build that still disagrees, or a toolchain that cannot express the port |
| Silently changes the public API | the generated `port.hpp` fixes the signatures the driver calls |

## Everything rust-anything does, for a C++ target

| Step | Command | What it gives you |
| --- | --- | --- |
| measure | `profile . -- python3 bench.py` | hot functions with callers and advice (batch vs port with callers vs third-party) |
| plan | `analyze . [--whole-app]` | units, waves, readiness/priority, risks, every dependency's C++ library, blocked/keep units |
| bridge | `bridge . unit` | a C++20/23 project with real signatures, a JSON driver and `profile.hpp` |
| record | `record cpp/unit [--from-tests -- pytest -q]` | generated edges **and** every call the original's own suite makes, plus coverage |
| port | `$EDITOR cpp/unit/src/port.cpp` | the only file you write |
| build / diff | `build`, `diff` | every recorded case, replayed through the C++ driver |
| shadow | `shadow cpp/unit` | a drop-in module: original returns, port runs second, disagreements logged |
| bench | `bench cpp/unit` | per-call timing on both sides |
| gate | `gate cpp/unit` | matrix, stubs, error paths, panics, size, staleness, coverage, build, dispatch, diff |
| whole app | `app record`, `app replay`, `app front-door`, `app wire`, `app lift` | a C++ front door that ships with everything falling back, then lifts to standalone |
| map | `map --write` | every unit's port, cases, last diff, and which originals changed upstream |
| setup | `setup` | MCP server + skill registered with Claude Code, Codex, Cursor, Gemini CLI, OpenCode |

## The loop

1. **analyze** — modules, functions, and the risks each one carries: unicode
   offsets, `int()` parsing, float formatting, unstable sorts, unbounded integers.
2. **bridge** — a C++23 project: `port.hpp` with real C++ types, `port.cpp` with
   stub bodies, a JSON driver, a dispatch table, CMake, and `include/cppan/profile.hpp`
   describing exactly what the target toolchain supports.
3. **record** — the original, imported as it normally runs, called on generated edge
   cases and a few seeded random ones. Every result, every exception type, into
   `cases.jsonl`.
4. **write the port** — the only file you edit is `src/port.cpp`.
5. **diff** — the C++ driver replays every case; the numbers, strings, lists, dicts,
   `NaN`, `-0.0` and exception types must agree.
6. **bench** — the same cases, timed in both processes.
7. **gate** — matrix, no stubs, build, dispatch, every case. Non-zero until it all passes.

`refresh` regenerates the declarations and dispatch after a signature change, and
never touches `src/port.cpp`.

## Shadow: the port beside the original on real traffic

`shadow` generates a drop-in Python module that calls the original first and returns
its result, then runs the C++ port through a C ABI and logs every disagreement. The
port cannot change what a caller sees, so shadow traffic can run in staging — or
production — while the port proves itself.

```sh
cpp-anything shadow cpp/textkit-words
# sys.path.insert(0, "cpp/textkit-words/shadow"); import textkit_words_shadow as words
cpp-anything shadow cpp/textkit-words --report
```

## Whole app: a C++ front door, then lift

```sh
cpp-anything app record . -- python3 cli.py      # docs examples, --inputs, semantic traps, twice each
cpp-anything app replay . --build <build>        # per-case report, exit code
cpp-anything app front-door .                    # cpp/app: routes first, exec the original otherwise
cpp-anything app wire . textkit.words --command slug --project cpp/textkit-words
cpp-anything app lift .                          # refuses while any case falls back
```

Fallback is explicit: `CPPAN_UNIT_<NAME>=original` forces a unit back, the fallback
log counts what ran the original, and `app lift` will not call an app standalone
while a case still needs it.

## Servers

`app record --http` starts the server, sends the `curl` lines from its docs (plus
`--inputs` and semantic traps), and stores what it answered. `app replay` starts any
build the same way and compares status, content type and body. `app front-door`
generates a proxy that owns `PORT` and runs the original behind it, so a server
ships immediately and every request is accounted for in the fallback log; `app lift`
stays refused until the proxy has native HTTP routes.

```sh
cpp-anything app record . --http -- python3 server.py
cpp-anything app replay . --build ./cpp/app/build/cppan_app
```

## Map, setup, and the agent loop

```sh
cpp-anything map --write        # cpp/PORT_MAP.md: ports, cases, last diff, changed upstream
cpp-anything setup              # MCP server + skill for the agents found on this machine
cpp-anything mcp                # 21 tools + 2 prompts over stdio
```

## What it can't catch

The recording is the contract. A branch no generated input reaches is not checked,
so `diff` passing means "behaves like the original on what was recorded", not
"equivalent". `record --inputs` takes real inputs (production samples, failing
cases) when the generated corpus cannot reach the interesting paths. Errors are
compared by exception type, not message, unless you pass `--strict-errors`; float
comparison is bit-exact unless you pass `--tolerance`. Reads
[what it can't catch](docs/how-it-works.md#what-it-cant-catch).

## Compiler and standard guides

The matrix is the factual backbone: 651 features across C++11–C++26, with the first
GCC, Clang/libc++, MSVC/MSVC STL and Apple Clang release that implements each one,
its feature-test macro and its paper. It is extracted from cppreference's compiler
support tables, with the page revision and fetch time recorded.

- [Guides index](docs/guides/README.md) — which toolchain to pick, and why
- [GCC](docs/guides/compilers/gcc.md) · [Clang + libc++](docs/guides/compilers/clang.md) ·
  [MSVC](docs/guides/compilers/msvc.md) · [LLVM releases](docs/guides/compilers/llvm.md)
- Standards: [C++17](docs/guides/standards/c++17.md) · [C++20](docs/guides/standards/c++20.md) ·
  [C++23](docs/guides/standards/c++23.md) · [C++26](docs/guides/standards/c++26.md)
- [Full matrix](docs/guides/compiler-matrix.md)

```sh
cpp-anything matrix features --toolchain clang --version 17 --std c++23
cpp-anything matrix explain __cpp_lib_expected
cpp-anything matrix profile  --toolchain msvc --version 19.44 --std c++23
```

The guides are generated from the data (`cpp-anything matrix docs`) and
`matrix docs --check` fails when they drift. `cpp-anything matrix sync` refreshes the
data from cppreference and re-renders the guides, recording the page revision it read.

## Rules the generated code follows

- **C++23 by default, C++20 supported**: `cppan::Result<T>` for failures
  (`std::expected` on C++23 targets), `std::string_view` in, `std::string` out,
  `std::span` for read-only sequences, `[[nodiscard]]` everywhere, ranges in the driver.
- **Feature-gated, never guessed**: `CPPAN_HAS_EXPECTED`, `CPPAN_HAS_PRINT`,
  `CPPAN_HAS_MDSPAN`, `CPPAN_HAS_MODULES`, … come from the matrix for your
  toolchain, and `bridge` refuses a standard it does not generate for.
- **No stubs survive gate**, and no `unsafe` is needed: the driver owns the JSON,
  the port owns the logic.

[Porting rules](docs/porting.md) covers unicode offsets, `int()` versus
`std::from_chars`, unbounded Python integers, dict order, float formatting, regex
engines and exception types.

## RSI: the tool verifies itself the same way

`cpp-anything rsi check` applies the loop to cpp-anything:

- the matrix data still parses and carries provenance for every page;
- the guides are exactly what the data renders to;
- nine recorded CLI transcripts still print what they printed (`rsi record` re-records);
- the generator still emits a project for the shipped example that **compiles and
  replays 105/105 cases**.

A change that breaks the generator, the wire format or the data fails `rsi check`
before it can reach your port. That is the recursive part: the same machinery that
gates a port gates the tool.

## Languages

| Source | Status |
| --- | --- |
| Python | the whole loop, shadow, whole-app (CLI and HTTP), batch, from-tests |
| C | analyze, bridge, record (fork-per-case harness), diff, bench, gate; supported signatures are scalars, `char*` and `void`, others are reported as skipped |
| Go, TypeScript, Java, C#, Ruby | planned, one plugin each: the loop, wire format, shadow, app, map and gate are language-independent; the analyzer, recorder and call mapping are not |

[`docs/PARITY.md`](docs/PARITY.md) is the command-by-command checklist against the
reference tool, including what is not there yet.

The target is C++20/23. Porting *C++* to another language is a different tool.

## Learn more

- [How it works](docs/how-it-works.md) — the wire format, comparison rules, risks, limits
- [Porting rules](docs/porting.md) — the C++ side of a port
- [MCP tools](docs/tools.md) — the same steps for agents
- [Install](docs/install.md) — from source, agent wiring, CI
- [FAQ](docs/faq.md) · [Decision log](DECISIONS.md) · [Changelog](CHANGELOG.md)

## Contributing

Issues and pull requests are welcome. Before sending one: `cargo test`,
`cargo clippy --all-targets`, and `cpp-anything rsi check` must pass — that last one
is the tool checking itself.

## License

MIT. Ports and generated code in your repository are yours. The matrix data comes
from cppreference.com (CC-BY-SA 4.0 / GFDL); the guides say so at the top.
