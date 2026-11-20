export module cppan.codegen;
import std;
import cppan.project;
import cppan.util;

export {
// Code generation: the C++ project `bridge` writes, and the examples/tests it is
// verified against.
//
// The generated code is C++23 (see `render_profile`): `std::expected` for failure,
// `std::string_view` for text coming in, `std::span` for read-only sequences,
// `[[nodiscard]]` on everything, `std::format` in the driver.



namespace cppan::codegen {

/// The shared library the shadow bindings load: built by CMake next to the driver.
[[nodiscard]] std::filesystem::path shadow_library(const Project& project);
[[nodiscard]] std::string namespace_of(const Project& project);

/// Every generated file, relative to the project directory.
[[nodiscard]] Result<std::vector<std::pair<std::string, std::string>>> project_files(
    const Project& project);

/// Writes the project. Existing files are only replaced with `--force`; a port is
/// the user's work, and `bridge` must never overwrite it by accident.
[[nodiscard]] Result<std::vector<std::string>> write_project(Project& project, bool force);

/// Regenerates everything except `src/port.cpp`, which holds the user's work.
[[nodiscard]] Result<std::vector<std::string>> refresh(const Project& project);

}  // namespace cppan::codegen
}
