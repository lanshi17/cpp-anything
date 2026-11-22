# MCP tools

`cpp-anything mcp` is a stdio MCP server that exposes the CLI. Each tool call runs
`cpp-anything <command>` in a subprocess and returns its stdout, so the terminal and
an agent can never drift apart.

```json
{
  "mcp": {
    "servers": {
      "cpp-anything": { "type": "local", "command": ["cpp-anything", "mcp"] }
    }
  }
}
```

Claude Code (`.mcp.json` in the project, or `claude mcp add`):

```json
{
  "mcpServers": {
    "cpp-anything": { "command": "cpp-anything", "args": ["mcp"] }
  }
}
```

Codex (`~/.codex/config.toml`):

```toml
[mcp_servers.cpp-anything]
command = "cpp-anything"
args = ["mcp"]
```

## Tools

| Tool | Arguments | Does |
| --- | --- | --- |
| `matrix_features` | `toolchain`, `version`, `std`, `kind?` | supported / partial / missing features for a compiler release |
| `matrix_explain` | `query` | one feature by name or feature-test macro: first release per compiler, notes |
| `matrix_profile` | `toolchain`, `version`, `std` | the `profile.hpp` the project would use |
| `analyze` | `repo` | modules, functions, risks |
| `bridge` | `repo`, `unit`, `lang?` (`c`/`cpp`/`ts`/`js`/`go`/`ruby`/`java`), `out?`, `toolchain?`, `version?`, `std?`, `modules?` (`auto`/`on`/`off`), `keep?`, `as_json?`, `kind?` (`napi`), `force?`, `module_root?` | generate the C++ project, as a C++20 module unit when the toolchain can build one |
| `record` | `project`, `cases?`, `seed?`, `inputs?`, `module?`, `resolve_from?`, `node_args?` | record the original's behaviour (Node options apply to TS/JS) |
| `build` | `project`, `clean?`, `cxx?`, `jobs?` | CMake configure + build |
| `diff` | `project`, `json?`, `strict_errors?`, `tolerance?`, `only?`, `max_report?` | replay and compare |
| `bench` | `project`, `repeat?`, `samples?`, `json?` | median/P95 per-call timing, both sides |
| `gate` | `project`, `json?`, `clean?`, `bench?` | matrix, stubs, build, dispatch, diff |
| `doctor` | — | cmake, compiler, ninja, python3, matrix, guides |

Failures come back with `isError: true` and the CLI's stderr, so an agent sees the
same message a human would.

## Protocol

JSON-RPC 2.0 over stdio, one message per line. Supported methods:
`initialize`, `notifications/initialized` (ignored), `ping`, `tools/list`,
`tools/call`. Unknown methods return `-32601`; unparseable lines return `-32700`.
Protocol version: `2024-11-05`.

```sh
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"matrix_explain","arguments":{"query":"__cpp_lib_expected"}}}' \
  | cpp-anything mcp
```

## A session an agent can follow

```text
analyze(repo=".")                                  -> start_here module
matrix_features(toolchain="gcc", version="13", std="c++23")
bridge(repo=".", unit="textkit.words")             -> cpp/textkit-words
record(project="cpp/textkit-words")                -> 105 cases
# agent writes src/port.cpp
diff(project="cpp/textkit-words")                  -> mismatches, if any
gate(project="cpp/textkit-words")                  -> done when it passes
```

The agent should stop at `analyze` and confirm the plan with the user before
`bridge`: porting is a decision about which unit matters, not a formality.
