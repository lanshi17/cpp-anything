// The command line. No argument-parser library: the surface is small enough to
// own, and `--help` stays in one place next to the code that implements it.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/util.hpp"

namespace cppan::cli {

struct Parsed {
  std::vector<std::string> positional;
  std::map<std::string, std::string> values;
  std::set<std::string> flags;
  /// Everything after `--`, verbatim: workloads and test commands.
  std::vector<std::string> trailing;
  /// Repeated options (`--agent claude --agent codex`), in order.
  std::map<std::string, std::vector<std::string>> multi;

  [[nodiscard]] std::optional<std::string> value(std::string_view name) const;
  [[nodiscard]] bool flag(std::string_view name) const;
  [[nodiscard]] std::vector<std::string> many(std::string_view name) const;
  [[nodiscard]] std::optional<std::uint64_t> number(std::string_view name) const;
};

[[nodiscard]] Result<Parsed> parse_args(const std::vector<std::string>& argv);

/// Runs one command; the value is the process exit code.
[[nodiscard]] Result<int> run(const std::vector<std::string>& argv);

/// The help text, also printed when `--help` appears after a command.
[[nodiscard]] std::string_view help_text();

}  // namespace cppan::cli
