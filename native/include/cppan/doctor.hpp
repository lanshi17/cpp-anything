// `doctor`: the tools a port needs, whether they are there, and how to install them.
#pragma once

#include "cppan/cli.hpp"
#include "cppan/util.hpp"

namespace cppan::doctor {

[[nodiscard]] Result<int> command(const cli::Parsed& args);

}  // namespace cppan::doctor
