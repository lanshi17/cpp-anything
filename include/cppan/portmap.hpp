// `map`: the port map — what is ported, what still is not, which recordings went
// stale, and where the front door still falls back.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cppan/json.hpp"
#include "cppan/util.hpp"

namespace cppan::portmap {

inline constexpr std::string_view kMapFile = "cpp/PORT_MAP.md";

struct Port {
  std::string unit;
  std::string dir;
  std::string module;
  std::string language;
  std::vector<std::string> functions;
  std::vector<std::string> kept;
  std::size_t cases = 0;
  std::optional<std::pair<std::int64_t, std::int64_t>> last_diff;
  std::optional<std::string> changed_upstream;
};

struct Glue {
  std::string file;
  bool falls_back = false;
};

struct Map {
  std::filesystem::path root;
  std::vector<Port> ports;
  std::vector<Json> not_ported;
  std::vector<Glue> glue;
  std::optional<std::string> analyze_error;

  [[nodiscard]] std::size_t changed() const;
  [[nodiscard]] std::string markdown() const;
  [[nodiscard]] Json to_json() const;
};

[[nodiscard]] Result<Map> build(const std::filesystem::path& repo,
                                const std::vector<std::filesystem::path>& extra);

}  // namespace cppan::portmap
