// C++20 modules: whether the target can build them, and whether this machine can
// build them at all.
//
// CMake scans module interfaces only with Ninja (or Visual Studio generators), so
// `--modules on` needs a Ninja on PATH. The compiler side is a curated minimum
// ahead of the matrix's "partial" rows: the matrix records that GCC 11 ships
// modules, and GCC 13 still fails on a two-file module, so the tool only claims
// what it has verified.
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "cppan/matrix.hpp"
#include "cppan/project.hpp"
#include "cppan/util.hpp"

namespace cppan::modules {

[[nodiscard]] std::optional<std::filesystem::path> ninja();
[[nodiscard]] bool compiler_ready(const Target& target);
/// Resolves `--modules auto|on|off` into the project's mode.
[[nodiscard]] Result<bool> resolve(std::string_view mode, const Target& target,
                                   const Matrix& matrix);

}  // namespace cppan::modules
