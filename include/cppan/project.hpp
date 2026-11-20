// Project metadata: `.cppan/project.json` is the record of what was bridged.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cppan/inventory.hpp"
#include "cppan/json.hpp"
#include "cppan/util.hpp"

namespace cppan {

inline constexpr std::string_view kProjectSchema = "cpp-anything/project@1";
inline constexpr std::string_view kMetaDir = ".cppan";
inline constexpr std::string_view kMetaFile = "project.json";

struct Target {
  std::string toolchain = "gcc";
  std::string version = "13";
  std::string standard = "c++23";
};

struct Recorded {
  std::size_t cases = 0;
  std::string at;
};

struct Batch {
  std::string function;
  std::string param;
};

struct Project {
  std::string unit;
  std::string module;
  std::filesystem::path source_file;
  std::optional<std::filesystem::path> package_root;
  std::string source_lang = "python";
  Target target;
  std::vector<Function> functions;
  std::vector<std::pair<std::string, std::string>> skipped;
  std::vector<std::string> risks;
  std::vector<std::string> imports;
  std::uint32_t lines = 0;
  std::string created_at;
  std::optional<Recorded> recorded;
  /// Saved by `record --float-tolerance`: what `diff` and `gate` compare with.
  double tolerance = 0.0;
  /// `--batch function:param`: functions answered over many items in one call.
  std::vector<Batch> batches;
  /// Generate a C++20 module (`export module <ns>;`) instead of a header.
  bool modules = false;
  std::vector<std::string> node_args;
  std::optional<std::string> resolve_from;
  std::optional<std::string> source_module;
  std::filesystem::path dir;

  [[nodiscard]] static Project from_unit(const Unit& unit, Target target,
                                         const std::filesystem::path& dir);
  [[nodiscard]] static Result<Project> load(const std::filesystem::path& dir);
  [[nodiscard]] Result<void> save() const;
  [[nodiscard]] Json to_json() const;
};

}  // namespace cppan
