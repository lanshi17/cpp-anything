// `mcp`: the same steps as MCP tools over stdio, so an agent can drive the loop.
//
// Hand-written JSON-RPC: the stdio transport needs `initialize`, `tools/list`,
// `prompts/list`, `prompts/get`, `tools/call` and `ping`, and every tool is the CLI
// itself in a subprocess, so an MCP call and a terminal call can never drift apart.
#pragma once

#include "cppan/util.hpp"

namespace cppan::mcp {

[[nodiscard]] Result<int> serve();

}  // namespace cppan::mcp
