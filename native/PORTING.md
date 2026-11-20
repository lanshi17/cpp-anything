# The C++ port of cpp-anything

The tool is being rewritten as a modern C++23 program. The Rust tree at the
repository root stays until the port reaches parity; it is the reference
implementation and the source of the CLI goldens in `tests/golden/cli.jsonl`.

## Layout

| path | what |
| --- | --- |
| `CMakeLists.txt` | the C++ project: C++23, warnings, `CPPAN_SANITIZE`, CTest |
| `native/include/cppan/` | headers, one per module |
| `native/src/` | implementations |
| `native/tests/golden.py` | replays the CLI goldens through the binary |
| `cmake/Embed.cmake` | embeds `data/*.json` and the Python probe |
| `build/dev/` | a local build (`cmake -S . -B build/dev -G Ninja`) |

## Ported

| module | status | verified by |
| --- | --- | --- |
| `json` | done | `native/include/cppan/json.hpp`, shared with generated projects |
| `util` | growing; versions, paths, processes, timestamps | goldens, later tests |
| `matrix` | done | `matrix features/explain/profile` goldens (6) |
| `docs` | done | `matrix docs --check` golden: 13 guides byte-identical |
| `cli` (matrix, analyze) | done | `matrix docs --check`, `analyze` goldens |
| `libs` | done | analyze plans |
| `python` | probe driver only | `analyze` |
| `analyze` | done | `analyze --json` golden and plan diff against the reference |
| `types`, `inventory`, `corpora`, `project` | done | used by the plugin ports |
| `langs` (ts/js, go, ruby, java, php, csharp) | analyze, record, bench, shadow for every plugin language | bridge+record+shadow byte-identical on all four fixtures |
| `langc` (C, C-style C++), `modern` | analyze, record, bench, harness | `examples/c-text`, `examples/legacy-cpp` |
| `codegen`, `modules`, `build`, `diff`, `bench`, `gate`, `shadow` | done | byte-identical generated projects, example gate output |
| `record` (Python/C/Ruby/Go/Java/TS) | done | cases.jsonl byte-identical |
| `setup`, `doctor`, `mcp`, `map` | done | dry-run/real config diffs, golden tool list |
| Python `record --from-tests` | not ported | needs the sitecustomize hook |
| `profile` (Node/Ruby/Go/Java collectors) | not ported | |
| whole-app (`app record/replay/front-door/wire/lift`) | not ported | |
| `rsi` | not ported | |

## Rules

- One Rust module becomes one header plus one translation unit, same function
  names, same output text. Behaviour that looks odd is copied deliberately until
  the reference changes; note it where it is not obvious.
- Errors are `cppan::Result<T>` (`std::expected<T, std::string>`), never
  exceptions across module boundaries. `JsonError` from the shared codec is
  caught at the edge.
- Output is compared byte for byte against the reference binary:
  `./target/release/cpp-anything <cmd>` vs `./build/dev/cpp-anything <cmd>`.
- `ctest --test-dir build/dev` replays the golden corpus.

## Float formatting

`cppan::format_real` in `native/include/cppan/json.hpp` reproduces Rust's `{:?}`
for floats (shortest digits, decimal for exponents in [-4, 16), exponents without
`+` or leading zeros). Recorded cases and `--json` reports compare as text, so
this is part of the wire contract.

## Verifying

```sh
cmake -S . -B build/dev -G Ninja && cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
cargo test                       # the reference still builds and passes
```
