# Parity with the reference tool

`rust-anything` is the reference: same loop, same artefacts, different target
language. This page is the honest checklist — what is implemented, what is adapted
because the target is C++, and what is not there yet. It is updated with the code.

## Commands

| Reference | cpp-anything | Status |
| --- | --- | --- |
| `analyze <root> [--json] [-o] [--lang python|c|cpp] [--whole-app]` | same flags, plus `ts/js/go/rust/ruby/java/php/csharp` | implemented: waves from resolved imports, readiness/priority, keep/blocked, dependency table with C++ libraries, `--whole-app` shape/entry points/decisions |
| `bridge <root> <unit> [--out] [--force] [--batch] [--keep] [--as-json] [--kind]` | same, plus `--toolchain/--version/--std/--modules` for the C++ target | implemented: `--keep name:reason` leaves an export in the original, `--as-json T` crosses a type parameter as JSON, `--kind napi` is the Node binding (WASM has no C++ target) |
| `refresh [--batch]` | same | implemented; it also re-reads the source signatures |
| `record [--count] [--inputs] [--from-tests -- cmd] [--float-tolerance]` | same, plus `--seed`, `--module/--resolve-from/--node-arg` for TS/JS | implemented per language: probe for Python, fork-per-case harness for C, out-of-process harnesses for TS/JS, Go, Rust, Ruby, Java; `--from-tests` remains Python-only |
| `build` | same | implemented (CMake) |
| `diff [--json] [--ignore-key-order] [--float-tolerance]` | `--json/--strict-errors/--tolerance/--only/--max-report` | implemented; objects already compare unordered, so `--ignore-key-order` is the default and the flag is unnecessary |
| `bench [--min-ms] [--json]` | `--repeat` (calls per case) `--json` | implemented with the same method: decode once, time only the calls, in the original's own process |
| `profile <root> -- <cmd>` | same | implemented per runtime: Python (cProfile), Node (`--cpu-prof`), Ruby (TracePoint tracer), Go (`go test -cpuprofile` + `go tool pprof -raw`), Java (JFR `jdk.ExecutionSample`) |
| `shadow [--out]` | `shadow [--report] [--check] [--json]` | implemented as a drop-in module over a C ABI; `--report`/`--check` summarise the log |
| `gate [--json]` | `--json/--clean/--cxx/--jobs/--bench` | implemented: matrix, stubs (ported), error paths, panics, size, recording staleness, coverage, build, dispatch, diff, optional bench |
| `map [--write] [--check] [--json] [--crates]` | same | implemented (`cpp/PORT_MAP.md`) |
| `app record [--http] [--inputs] [--normalize] [--no-traps] [--timeout] [--arg] [--fresh] [--pace] [--float-tolerance] [--effects] [--effect-env] [--db-reset] [--db-dump] [--json] -- cmd` | same | implemented for CLIs and servers |
| `app replay [--build] [--case] [--timeout] [--json]` | same | implemented, including servers |
| `app front-door [--no-check] [--library]` | same | implemented for CLIs (routes + exec fallback), servers (native routes + proxy) and libraries (`cpp/lib`, `cppan_api`, no entry point) |
| `app wire <unit> --command` / `--route` | `--command NAME` for CLIs, `--route "METHOD /path"` (repeatable, `{param}`, `*`) for servers | implemented: routes, glue stubs, per-unit library link, kill switch, fallback counting |
| `app lift` | same | implemented for CLIs and servers: refuses while any case/request falls back, then rebuilds without the original (`CPPAN_LIFTED`); libraries have nothing to lift |
| `setup [--agent] [--remove] [--dry-run] [--json]` | same | implemented for Claude Code, Codex, Cursor, Gemini CLI, OpenCode |
| `doctor [--json]` | same | implemented: cmake, compiler, python3, matrix, guides, MCP self-answer, agent entries |
| `mcp` | same | 21 tools, 2 prompts, progress notifications, structured errors |
| `--help`, `--version` | `help`; version in `CMakeLists.txt` | implemented |

## Semantics that carry over

| Behaviour | Status |
| --- | --- |
| Record twice, mark unstable cases, normalise timestamps/UUIDs/key order | implemented (app cases); unit recordings are deterministic input corpora |
| Errors compared by type, messages optional (`--strict-errors`) | implemented |
| Floats bit-exact by default, tolerance saved with the recording | implemented |
| Objects unordered, arrays ordered | implemented |
| Missing result is a failure | implemented |
| Fallback is counted and `lift` refuses while it happens | implemented for CLIs |
| Drift detection: recorded fingerprint vs current source | implemented (`recorded-from.json`, surfaced by `map` and `gate`) |
| Kill switches: `CPPAN_FORCE_FALLBACK`, `CPPAN_UNIT_<NAME>=original` | implemented |
| Fallback log through `CPPAN_FALLBACK_LOG` | implemented |

## Not there yet

| Area | What is missing | Why it matters |
| --- | --- | --- |
| PHP and C# per-unit ports | both are analyzed as whole apps; there is no per-unit analyzer/recorder harness for them | PHP and C# have no fixture-verified recorder yet; `app` still records and fronts a whole CLI/server in either language because it execs the original |
| `record --from-tests` outside Python | the flag runs a Python suite and imports the probe | Go `-run`, JUnit and Ruby test discovery need their own hooks; `--inputs` covers real samples today |
| SMTP sink completeness | a minimal accepting server, not a full MTA | a port that relies on MTA behaviour (relay, DSN, TLS) needs a real sink |
| Plugin fixture depth | each plugin's supported signature subset is documented and tested on its fixture | bugs in exotica (generics, structs, unions, variadics) are reported as skipped, not ported |

## Where the target changes the design

- **Bindings.** The reference generates napi/PyO3/cgo/JNI wrappers over its Rust
  port. cpp-anything generates one C ABI (`cppan_shadow_call`/`cppan_shadow_free`)
  and a thin binding per runtime: ctypes (Python), Node-API addon (TS/JS), cgo
  (Go), Fiddle with an optional magnus extension (Ruby), JNI (Java). The ABI is
  the same for every language, so a port is written and gated once.
- **`unsafe`.** There is no `unsafe` in the target language to audit; the equivalent
  audit is "no abort/assert/throw where the original returns an error" (the `panics`
  gate check).
- **`cargo fmt`/`clippy`/`cargo test`.** The target's toolchain is CMake plus the
  project's compiler: `gate` builds with warnings on, runs the driver's dispatch
  check and the diff; a `.clang-format` ships with each project.
- **Crates.** `data/libs.json` maps Python packages to C++ libraries with an honest
  fit; unknown packages are reported as unknown.
- **Modules.** There is no Rust equivalent of `bridge --modules`: the target's build
  system decides. cpp-anything generates a C++20 module unit when the toolchain can
  build one (gcc 15+, clang 17+, apple-clang 16+, msvc 19.36+) and a header
  otherwise; the interface compiles once into `gcm.cache/` so a second translation
  unit can import it.
