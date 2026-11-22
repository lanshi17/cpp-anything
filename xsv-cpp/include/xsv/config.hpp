#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "xsv/common.hpp"

namespace xsv {

// Mirrors the parts of xsv's Config that the P0 commands use. stdin is
// represented by an empty path.
struct Config {
  std::optional<std::filesystem::path> path;
  bool no_headers = false;
  bool flexible = false;
  char delimiter = ',';
  char quote = '"';
  bool double_quote = true;
  std::optional<char> escape;
  bool quoting = true;

  static Result<Config> from_path(std::optional<std::string_view> input);

  [[nodiscard]] bool is_stdin() const { return !path.has_value(); }
  [[nodiscard]] std::filesystem::path index_path() const;
};

// xsv accepts "\t" or one ASCII byte. Anything else is a usage error.
Result<char> parse_delimiter(std::string_view text);

// Honours XSV_TOGGLE_HEADERS=1 exactly like the reference: it inverts the flag
// rather than forcing it.
bool toggled_no_headers(bool requested);

// Seconds since epoch, matching the reference's coarser index freshness check.
Result<std::uint64_t> mtime_seconds(const std::filesystem::path& path);

}  // namespace xsv
