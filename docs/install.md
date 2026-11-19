# Install

## Requirements

| Tool | Version | Why |
| --- | --- | --- |
| CMake | 3.28+ | generated projects; the `project()`/`target_compile_features` surface used |
| C++ compiler | GCC 13+, Clang 17+ (with libc++ 16+), MSVC 19.33+ | C++23: `std::expected`, `std::span`, ranges |
| Python | the interpreter that runs the code being ported (`CPPAN_PYTHON` overrides `python3`) | `analyze`, `record`, `bench` |
| Rust | 1.82+ | building cpp-anything itself |

`cpp-anything doctor` checks all of it, with the fix for each missing piece.

## From source

```sh
git clone https://github.com/cpp-anything/cpp-anything
cd cpp-anything
cargo install --path .
cpp-anything doctor
```

Or run it without installing:

```sh
cargo run --release -- matrix features --toolchain gcc --version 13 --std c++23
```

There are no crate dependencies: the build needs a Rust toolchain and nothing from
the network.

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

`cpp-anything` is one static binary with no dependencies, so CI is: install Rust,
build, run `rsi check` (which needs CMake and a C++ compiler for the self-port step),
and run the loop on your own project's units.

```yaml
- run: cargo build --release
- run: cargo test && cargo clippy --all-targets -- -D warnings
- run: ./target/release/cpp-anything matrix docs --check
- run: ./target/release/cpp-anything rsi check
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
cargo build                     # the binary embeds the matrix
cpp-anything rsi check          # guides, goldens and the self-port
```

`matrix sync --offline` parses the cached wikitext instead of fetching. When the
output legitimately changes, `cpp-anything rsi record` re-records the CLI goldens —
it is a reviewed diff in `tests/golden/cli.jsonl`, never an automatic update.

Offline: `scripts/sync_matrix.py --offline` parses the cached wikitext under
`scripts/.cache/` and keeps the provenance of the last online sync.
