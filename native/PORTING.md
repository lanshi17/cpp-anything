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

Not ported yet: language plugins, `bridge`/codegen, `record`/`diff`/`bench`/
`gate`/`shadow`, `profile`, the whole-app commands, `doctor`, `setup`, `mcp`,
`rsi`, `map`.

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

## Verifying

```sh
cmake -S . -B build/dev -G Ninja && cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
cargo test                       # the reference still builds and passes
```
