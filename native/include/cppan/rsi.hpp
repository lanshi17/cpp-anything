// `rsi`: cpp-anything checking itself with its own loop. Every step either proves a
// shipped artefact still works or says exactly what drifted.
#pragma once

#include "cppan/cli.hpp"
#include "cppan/util.hpp"

namespace cppan::rsi {

[[nodiscard]] Result<int> command(const cli::Parsed& args);

}  // namespace cppan::rsi
