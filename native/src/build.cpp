#include "cppan/build.hpp"

#include <algorithm>
#include <format>

#include "cppan/modules.hpp"

namespace cppan::build {
namespace {

std::string tail(std::string_view text, std::size_t lines) {
  const auto all = split_lines(text);
  const std::size_t start = all.size() > lines ? all.size() - lines : 0;
  std::string out;
  for (std::size_t index = start; index < all.size(); ++index) {
    if (!out.empty()) out += '\n';
    out += all[index];
  }
  return out;
}

}  // namespace

std::filesystem::path driver_path(const Project& project) {
#if defined(_WIN32)
  return project.dir / "build" / "cppan_driver.exe";
#else
  return project.dir / "build" / "cppan_driver";
#endif
}

Result<BuildOutcome> build_project(const Project& project, bool clean,
                                   const std::optional<std::string>& cxx, std::size_t jobs) {
  const std::filesystem::path build_dir = project.dir / "build";
  std::error_code error;
  if (clean && std::filesystem::exists(build_dir, error)) {
    std::filesystem::remove_all(build_dir, error);
    if (error) return std::unexpected(build_dir.string() + ": " + error.message());
  }
  if (!which_on_path("cmake").has_value()) {
    return std::unexpected(
        "cmake is not on PATH: install CMake 3.28 or newer (alternatives: `apt install cmake`, "
        "`brew install cmake`, `winget install Kitware.CMake`)");
  }

  std::vector<std::string> configure = {"-S", project.dir.string(), "-B", build_dir.string(),
                                        "-DCMAKE_BUILD_TYPE=Release"};
  if (project.modules) {
    const auto ninja = modules::ninja();
    if (!ninja.has_value()) {
      return std::unexpected(
          "this project uses C++20 modules, and CMake needs Ninja to scan them: install ninja "
          "(`python3 -m pip install --user ninja`) or regenerate with `bridge --modules off`");
    }
    configure.emplace_back("-G");
    configure.emplace_back("Ninja");
    configure.push_back("-DCMAKE_MAKE_PROGRAM=" + ninja->string());
  }
  if (cxx.has_value()) configure.push_back("-DCMAKE_CXX_COMPILER=" + *cxx);
  const Run configured = run_command("cmake", configure, project.dir);
  if (!configured.ok) {
    return std::unexpected("cmake configure failed: " + configured.failure() + "\n" +
                           indent_text(configured.err, 2));
  }

  std::vector<std::string> make = {"--build", build_dir.string(), "-j",
                                   std::to_string(std::max<std::size_t>(jobs, 1))};
  if (cxx.has_value()) make.emplace_back("--");
  const Run built = run_command("cmake", make, project.dir);
  std::size_t warnings = 0;
  for (const std::string_view line : split_lines(built.err)) {
    if (line.find("warning:") != std::string_view::npos) ++warnings;
  }
  for (const std::string_view line : split_lines(built.out)) {
    if (line.find("warning:") != std::string_view::npos) ++warnings;
  }
  if (!built.ok) {
    return std::unexpected("build failed: " + built.failure() + "\n" +
                           indent_text(tail(built.out + built.err, 40), 2));
  }
  const auto driver = driver_path(project);
  if (!std::filesystem::exists(driver)) {
    return std::unexpected("build succeeded but " + driver.string() + " is missing");
  }
  return BuildOutcome{driver, warnings};
}

}  // namespace cppan::build
