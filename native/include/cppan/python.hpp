// Running the Python probe: analysis, recording, and benchmarking the original.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cppan/inventory.hpp"
#include "cppan/json.hpp"
#include "cppan/project.hpp"
#include "cppan/util.hpp"

namespace cppan::python {

/// `$CPPAN_PYTHON`, else `python3`.
[[nodiscard]] std::string interpreter();

/// The interpreter's version when it runs, for a clear "not on PATH" error.
[[nodiscard]] std::optional<std::string> available();

/// Runs the embedded probe with `args`; its stdout is the answer.
[[nodiscard]] Result<std::string> probe_command(const std::vector<std::string>& args);

/// The probe's `analyze` mode over a repository.
[[nodiscard]] Result<Json> analyze(const std::filesystem::path& root);

}  // namespace cppan::python

namespace cppan::python {

/// The probe's `inventory` mode: functions, types, defaults, risks for one unit.
[[nodiscard]] Result<Unit> inventory(const std::filesystem::path& path, std::string_view module,
                                     const std::optional<std::filesystem::path>& package_root);

/// Times the original over the recorded cases; nanoseconds per call.
[[nodiscard]] Result<double> bench(const Project& project, std::size_t repeat);

/// Records generated edge cases for every ported function; returns how many.
[[nodiscard]] Result<std::size_t> record(const Project& project, std::size_t cases,
                                         std::uint64_t seed,
                                         const std::optional<std::filesystem::path>& inputs);

}  // namespace cppan::python
