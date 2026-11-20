module;
#include <cstdlib>

module cppan.modules;
import cppan.json;
import cppan.types;
import cppan.util;
import std;

namespace cppan::modules {

std::optional<std::filesystem::path> ninja() {
  if (const auto which = which_on_path("ninja"); which.has_value()) return which;
  const char* home = std::getenv("HOME");
  std::vector<std::filesystem::path> candidates;
  if (home != nullptr) candidates.push_back(std::filesystem::path(home) / ".local/bin/ninja");
  candidates.emplace_back("/usr/local/bin/ninja");
  candidates.emplace_back("/opt/homebrew/bin/ninja");
  for (const auto& path : candidates) {
    if (std::filesystem::is_regular_file(path)) return path;
  }
  return std::nullopt;
}

bool compiler_ready(const Target& target) {
  const std::string_view minimum = target.toolchain == "gcc"        ? "15"
                                   : target.toolchain == "clang"    ? "17"
                                   : target.toolchain == "aclang"   ? "16"
                                   : target.toolchain == "msvc"     ? "19.36"
                                                                    : "";
  if (minimum.empty()) return false;
  return version_at_least(target.version, minimum);
}

Result<bool> resolve(std::string_view mode, const Target& target, const Matrix& matrix) {
  const std::string_view number =
      target.standard.starts_with("c++") ? std::string_view(target.standard).substr(3)
                                         : std::string_view(target.standard);
  std::uint32_t std_number = 0;
  for (const char ch : number) {
    if (ch < '0' || ch > '9') break;
    std_number = std_number * 10 + static_cast<std::uint32_t>(ch - '0');
  }
  if (std_number < 20) {
    if (mode == "on") {
      return std::unexpected(std::format("--modules needs C++20 or later, not {}: pass "
                                         "`--std c++23`",
                                         target.standard));
    }
    return false;
  }
  const MacroStatus matrix_says =
      matrix.macro_available("__cpp_modules", target.toolchain, target.version, target.standard);
  const bool compiler = compiler_ready(target);
  const bool has_ninja = ninja().has_value();
  if (mode == "off") return false;
  if (mode == "on") {
    if (!compiler) {
      std::string detail = "not recorded";
      if (matrix_says.kind == MacroStatusKind::TooOld) {
        detail = "needs " + matrix_says.release;
      } else if (matrix_says.kind == MacroStatusKind::Partial) {
        detail = "partial since " + matrix_says.release;
      }
      return std::unexpected(std::format(
          "{} does not support C++20 modules well enough to generate them: the tool needs gcc "
          "15+, clang 17+, apple-clang 16+ or msvc 19.36+ for a module unit (the matrix row "
          "says: {}). Pass `--modules off` to generate a header instead.",
          target.toolchain, detail));
    }
    if (!has_ninja) {
      return std::unexpected(
          "CMake scans C++20 module interfaces only with Ninja: install it "
          "(`python3 -m pip install --user ninja`, `apt install ninja-build`, "
          "`brew install ninja`) or pass `--modules off`");
    }
    return true;
  }
  return compiler && has_ninja;
}

}  // namespace cppan::modules
