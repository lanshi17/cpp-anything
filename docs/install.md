# Install

## Requirements

| Tool | Version | Why |
| --- | --- | --- |
| CMake | 3.28+ | module scanning (`FILE_SET CXX_MODULES`) and generated projects |
| Ninja | 1.10+ | CMake scans module interfaces with Ninja (or Visual Studio) |
| C++ compiler | GCC 15+, Clang 18+ with libc++ modules, MSVC 19.44+ | building cpp-anything itself (C++20 modules, `import std;`) |
| C++ compiler for generated projects | GCC 13+, Clang 17+ (libc++ 16+), MSVC 19.33+ | what `matrix` says each release supports |
| Python | the interpreter that runs the code being ported (`CPPAN_PYTHON` overrides `python3`) | `analyze`, `record`, `bench` |
| C++23 compiler | the one above | building cpp-anything itself |
| Rust (`cargo`, `rustc`) | any stable release | `record`, `bench` and the generated shadow crate for `--lang rust`; only needed for Rust sources |

`cpp-anything doctor` checks all of it, with the fix for each missing piece.

## From source

```sh
git clone https://github.com/lanshi17/cpp-anything
cd cpp-anything
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build        # or run ./build/cpp-anything directly
cpp-anything doctor
```

Or run it without installing:

```sh
./build/cpp-anything matrix features --toolchain gcc --version 13 --std c++23
```

There are no third-party dependencies: the build needs CMake, a C++23 compiler and
nothing from the network.

## Platforms

- **Linux** — any distribution with GCC 13+ or Clang 17+; `apt install g++ cmake python3`.
- **macOS** — Apple Clang ships libc++; check `cpp-anything matrix features --toolchain apple-clang`.
  `brew install cmake`.
- **Windows** — MSVC 19.33+ with `/std:c++latest`; CMake picks the toolset.
  The generated `CMakeLists.txt` passes `/Zc:__cplusplus` because MSVC otherwise
  reports `199711L` for `__cplusplus`.

## Wiring an agent

Add the MCP server and point the agent at the skill:

```json
{ "mcpServers": { "cpp-anything": { "command": "cpp-anything", "args": ["mcp"] } } }
```

Copy [`skills/cpp-anything/SKILL.md`](../skills/cpp-anything/SKILL.md) into the
directory your agent reads skills from (`~/.claude/skills/`, `~/.codex/skills/`,
`~/.config/opencode/skills/`, …). The skill is a workflow, not documentation:
analyze → plan → bridge → record → port → diff → gate, one unit at a time.

## CI

`cpp-anything` is one dependency-free binary, so CI is: configure and build with
CMake and a C++23 compiler that ships the `std` module (GCC 15+), run the CTest
suite, then `rsi check`.

```yaml
- run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
- run: cmake --build build -j
- run: ctest --test-dir build --output-on-failure
- run: ./build/cpp-anything matrix docs --check
- run: ./build/cpp-anything rsi check
```

For a user repository, the same shape per unit:

```yaml
- run: cpp-anything build cpp/textkit-words
- run: cpp-anything diff cpp/textkit-words
- run: cpp-anything gate cpp/textkit-words
```

## Updating the compiler matrix

The data is refreshed explicitly, never in the background:

```sh
cpp-anything matrix sync        # fetch cppreference's tables + re-render docs/guides
cmake --build build             # the binary embeds the matrix
cpp-anything rsi check          # guides, goldens and the self-port
```

`matrix sync --offline` parses the cached wikitext instead of fetching. When the
output legitimately changes, `cpp-anything rsi record` re-records the CLI goldens —
it is a reviewed diff in `tests/golden/cli.jsonl`, never an automatic update.

Offline: `scripts/sync_matrix.py --offline` parses the cached wikitext under
`scripts/.cache/` and keeps the provenance of the last online sync.
