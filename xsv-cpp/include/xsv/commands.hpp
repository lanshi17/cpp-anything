#pragma once

#include <span>
#include <string_view>

#include "xsv/common.hpp"

namespace xsv::commands {

// P0 command entry points. Each takes the argv slice after the command name,
// mirrors the reference CLI's flags, and returns a process exit code.
Status count(std::span<const std::string_view> args);
Status index(std::span<const std::string_view> args);
Status headers(std::span<const std::string_view> args);
Status select(std::span<const std::string_view> args);
Status slice(std::span<const std::string_view> args);
Status search(std::span<const std::string_view> args);
Status sort(std::span<const std::string_view> args);
Status stats(std::span<const std::string_view> args);
Status join(std::span<const std::string_view> args);

}  // namespace xsv::commands
