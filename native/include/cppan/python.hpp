// Running the Python probe: analysis, recording, and benchmarking the original.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cppan/json.hpp"
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
