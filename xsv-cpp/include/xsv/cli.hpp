#pragma once

#include <span>
#include <string_view>

#include "xsv/common.hpp"

namespace xsv::cli {

struct Command {
  std::string_view name;
  std::string_view summary;
  Status (*run)(std::span<const std::string_view> args);
};

[[nodiscard]] std::span<const Command> commands();
[[nodiscard]] const Command* find_command(std::string_view name);

int main(int argc, char** argv);

}  // namespace xsv::cli
