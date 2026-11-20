# The C++ implementation

cpp-anything is a C++23 program. It began as a Rust tool; this file records how
the port was done and what proves it. The Rust tree was removed when the C++
binary matched it byte for byte, and the CLI goldens in `tests/golden/cli.jsonl`
are the frozen transcript of that reference.

## Layout

| path | what |
| --- | --- |
| `CMakeLists.txt` | the C++ project: C++23, warnings, `CPPAN_SANITIZE`, install, CTest |
| `src/modules/*.cppm` | C++20 module interfaces (`export module cppan.*`), one per component |
| `src/` | implementation units (`module cppan.*;`) that `import std;` and their dependencies |
| `include/cppan/json.hpp` | the only header: it is embedded into generated projects and wrapped by `cppan.json` |
| `src/langs/` | the Go, Java, PHP, C#, Ruby and TS/JS plugins |
| `resources/` | the runtime files generated projects embed (wire header, driver, shadow, probe) |
| `tests/golden.py`, `tests/unit.cpp` | CLI golden replay and unit checks (CTest) |
| `cmake/Embed.cmake` | embeds `data/*.json`, `resources/*` and the skill |
| `build/` | a local build (`cmake -S . -B build -G Ninja`) |

## Ported

| module | status | verified by |
| --- | --- | --- |
| `json` | done | `include/cppan/json.hpp`, shared with generated projects |
| `util` | done: versions, paths, processes (POSIX and Windows), timestamps, hashing | unit checks, whole loop |
| `matrix` | done | `matrix features/explain/profile` goldens (6) |
| `docs` | done | `matrix docs --check` golden: 13 guides byte-identical |
| `cli` | done: every command | CLI goldens, per-command diffs against the reference |
| `libs` | done | analyze plans |
| `python` | done: probe, recorder, bench, `--from-tests` | fixture recordings identical |
| `analyze` | done | `analyze --json` golden and plan diff against the reference |
| `types`, `inventory`, `corpora`, `project` | done | used by the plugin ports |
| `langs` (ts/js, go, ruby, java, php, csharp) | analyze, record, bench, shadow for every plugin language | bridge+record+shadow byte-identical on all four fixtures |
| `langc` (C, C-style C++), `modern` | analyze, record, bench, harness | `examples/c-text`, `examples/legacy-cpp` |
| `codegen`, `modules`, `build`, `diff`, `bench`, `gate`, `shadow` | done | byte-identical generated projects, example gate output |
| `record` (Python/C/Ruby/Go/Java/TS) | done | cases.jsonl byte-identical |
| `portmap`, `recording` | done | `map` output and `--write` file identical |
| `setup`, `doctor`, `mcp`, `map` | done | dry-run/real config diffs, golden tool list |
| Python `record --from-tests` | done | cases.jsonl and recorded-from.json identical on the fixture suite |
| `profile` (Node, Ruby, Go, Java, Python collectors) | done | JSON/text parity per collector; Python workload vs the reference |
| whole-app (`app record/replay/front-door/wire/lift`, HTTP and library front doors) | done | slugify/effects/http-app trees and output identical |
| `rsi` | done | `rsi check` PASS: every step green |

## Rules

- Code is built as C++20 modules: interfaces live in `src/modules/*.cppm`,
  implementation units import them, and the standard library comes from
  `import std;`. Only the generated-project JSON codec stays a header. Behaviour that looks odd is copied deliberately until
  the reference changes; note it where it is not obvious.
- Errors are `cppan::Result<T>` (`std::expected<T, std::string>`), never
  exceptions across module boundaries. `JsonError` from the shared codec is
  caught at the edge.
- Output is compared byte for byte against the reference binary:
  `./target/release/cpp-anything <cmd>` vs `./build/dev/cpp-anything <cmd>`.
- `ctest --test-dir build/dev` replays the golden corpus.

## Float formatting

`cppan::format_real` in `include/cppan/json.hpp` reproduces Rust's `{:?}`
for floats (shortest digits, decimal for exponents in [-4, 16), exponents without
`+` or leading zeros). Recorded cases and `--json` reports compare as text, so
this is part of the wire contract.

## Verifying

```sh
cmake -S . -B build -G Ninja && cmake --build build -j   # GCC 15+ (std module)
ctest --test-dir build --output-on-failure
./build/cpp-anything rsi check
```
