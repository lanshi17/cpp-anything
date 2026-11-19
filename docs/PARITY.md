# Parity with the reference tool

`rust-anything` is the reference: same loop, same artefacts, different target
language. This page is the honest checklist — what is implemented, what is adapted
because the target is C++, and what is not there yet. It is updated with the code.

## Commands

| Reference | cpp-anything | Status |
| --- | --- | --- |
| `analyze <root> [--json] [-o] [--lang python|c|cpp] [--whole-app]` | same flags | implemented: waves from resolved imports, readiness/priority, keep/blocked, dependency table with C++ libraries, `--whole-app` shape/entry points/decisions |
| `bridge <root> <unit> [--out] [--force] [--batch] [--keep] [--as-json] [--kind]` | `bridge` with `--out/--force/--batch/--name`-style flags; `--toolchain/--version/--std` choose the C++ target | implemented except `--keep`/`--as-json` (they are TS-export controls; Python units port whole) |
| `refresh [--batch]` | same | implemented; it also re-reads the source signatures |
| `record [--count] [--inputs] [--from-tests -- cmd] [--float-tolerance]` | same, plus `--seed`; C records through a fork-per-case harness | implemented; `--module`/`--resolve-from`/`--node-arg` are JS-specific and absent until the JS plugin |
| `build` | same | implemented (CMake) |
| `diff [--json] [--ignore-key-order] [--float-tolerance]` | `--json/--strict-errors/--tolerance/--only/--max-report` | implemented; objects already compare unordered, so `--ignore-key-order` is the default and the flag is unnecessary |
| `bench [--min-ms] [--json]` | `--repeat` (calls per case) `--json` | implemented with the same method: decode once, time only the calls, in the original's own process |
| `profile <root> -- <cmd>` | same | implemented for Python (cProfile); the other runtimes arrive with their plugins |
| `shadow [--out]` | `shadow [--report] [--check] [--json]` | implemented as a drop-in module over a C ABI; `--report`/`--check` summarise the log |
| `gate [--json]` | `--json/--clean/--cxx/--jobs/--bench` | implemented: matrix, stubs (ported), error paths, panics, size, recording staleness, coverage, build, dispatch, diff, optional bench |
| `map [--write] [--check] [--json] [--crates]` | same | implemented (`cpp/PORT_MAP.md`) |
| `app record [--http] [--inputs] [--normalize] [--no-traps] [--timeout] [--arg] [--fresh] [--pace] [--float-tolerance] [--effects] [--effect-env] [--db-reset] [--db-dump] [--json] -- cmd` | same | implemented for CLIs and servers |
| `app replay [--build] [--case] [--timeout] [--json]` | same | implemented, including servers |
| `app front-door [--no-check] [--library]` | `--no-check`; servers get a proxy front door | implemented for CLIs (routes + exec fallback) and servers (proxy); `--library` is not there yet |
| `app wire <unit> --command` | `--command NAME [--project DIR]` | implemented: routes, glue stub, per-unit library link, kill switch; HTTP routes are not wired yet |
| `app lift` | same | implemented for CLIs (refuses while any case falls back); servers refuse with the exact reason |
| `setup [--agent] [--remove] [--dry-run] [--json]` | same | implemented for Claude Code, Codex, Cursor, Gemini CLI, OpenCode |
| `doctor [--json]` | same | implemented: cmake, compiler, python3, matrix, guides, MCP self-answer, agent entries |
| `mcp` | same | 21 tools, 2 prompts, progress notifications, structured errors |
| `--help`, `--version` | `help`; version in `Cargo.toml` | implemented |

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
| Source languages | TypeScript/JavaScript, Go, Ruby, Java; PHP and C# whole-app only | Each is a plugin: analyzer, recorder, call mapping, wrapper. C and C-style C++ have one (analyze/bridge/record/diff/bench/gate, fork-per-case harness); C-style C++ adds the modernization scan and the `modern` gate check |
| Side effects | `--effects http,smtp` and `--effect-env` run local sinks; `--db-reset`/`--db-dump` bracket each case | implemented for CLIs and servers' requests; the SMTP sink is a minimal accepting server, not a full MTA |
| Library front door | `app front-door --library` | A C++ library front door that re-exports each ported unit |
| HTTP routing | `app wire` for servers | The server front door proxies everything; a native route cannot answer yet |
| TS-specific flags | `--kind napi/wasm`, `--keep`, `--as-json`, `--node-arg`, `--resolve-from` | They describe bindings to Node/WASM, which do not exist for a C++ target |
| `profile` for other runtimes | Node/Go/Java/Ruby profilers | Arrives with each plugin |

## Where the target changes the design

- **Bindings.** The reference generates napi/PyO3/cgo/JNI wrappers. cpp-anything
  generates one C ABI (`cppan_shadow_call`) plus ctypes/FFI modules for Python; other
  source languages get their own FFI modules when they land.
- **`unsafe`.** There is no `unsafe` in the target language to audit; the equivalent
  audit is "no abort/assert/throw where the original returns an error" (the `panics`
  gate check).
- **`cargo fmt`/`clippy`/`cargo test`.** The target's toolchain is CMake plus the
  project's compiler: `gate` builds with warnings on, runs the driver's dispatch
  check and the diff; a `.clang-format` ships with each project.
- **Crates.** `data/libs.json` maps Python packages to C++ libraries with an honest
  fit; unknown packages are reported as unknown.
