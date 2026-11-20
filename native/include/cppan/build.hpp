// Configuring and building a generated project with CMake.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

#include "cppan/project.hpp"
#include "cppan/util.hpp"

namespace cppan::build {

struct BuildOutcome {
  std::filesystem::path driver;
  std::size_t warnings = 0;
};

[[nodiscard]] std::filesystem::path driver_path(const Project& project);
[[nodiscard]] Result<BuildOutcome> build_project(const Project& project, bool clean,
                                                 const std::optional<std::string>& cxx,
                                                 std::size_t jobs);

}  // namespace cppan::build
