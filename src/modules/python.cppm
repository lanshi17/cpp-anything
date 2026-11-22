export module cppan.python;
import std;
import cppan.inventory;
import cppan.json;
import cppan.project;
import cppan.util;

export {
// Running the Python probe: analysis, recording, and benchmarking the original.



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
[[nodiscard]] Result<BenchMeasurement> bench(const Project& project, std::size_t repeat);

struct FromTests {
  std::vector<std::string> command;
  std::size_t calls = 0;
  std::size_t added = 0;
  std::optional<std::pair<std::size_t, std::size_t>> coverage;
  bool suite_ok = false;
  std::string suite_error;
};

/// Runs the original's own test suite with a hook that records every call it makes
/// to the unit. The hook is injected through PYTHONPATH (`sitecustomize.py`), so it
/// works for pytest, unittest and plain scripts without touching the project.
[[nodiscard]] Result<FromTests> record_from_tests(const Project& project,
                                                  const std::vector<std::string>& command);

/// Records generated edge cases for every ported function; returns how many.
[[nodiscard]] Result<std::size_t> record(const Project& project, std::size_t cases,
                                         std::uint64_t seed,
                                         const std::optional<std::filesystem::path>& inputs);

}  // namespace cppan::python
}
