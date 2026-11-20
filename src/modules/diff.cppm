export module cppan.diff;
import std;
import cppan.json;
import cppan.project;
import cppan.util;

export {
// `diff`: replay every recorded case through the C++ port and compare.
//
// Errors are compared by exception type, not message: messages are rarely a
// contract, and a port would fail on wording forever. `--strict-errors` opts in.



namespace cppan::diff {

struct Mismatch {
  std::string id;
  std::string function;
  Json args;
  Json expected;
  Json got;
  std::string reason;
};

struct DiffReport {
  std::size_t total = 0;
  std::size_t matched = 0;
  std::size_t skipped = 0;
  std::vector<std::string> missing;
  std::vector<Mismatch> mismatches;
  std::vector<std::tuple<std::string, std::size_t, std::size_t>> per_function;

  [[nodiscard]] bool passed() const {
    return total > 0 && matched + skipped == total && missing.empty();
  }
};

struct Options {
  double tolerance = 0.0;
  bool strict_errors = false;
  std::optional<std::string> only;
  std::size_t max_report = 8;
};

struct Case {
  std::string id;
  std::string function;
  Json args;
  Json expect;
};

[[nodiscard]] Result<std::vector<Case>> load_cases(const std::filesystem::path& path);
[[nodiscard]] Result<DiffReport> run(const Project& project, const std::filesystem::path& driver,
                                     const Options& options);
[[nodiscard]] Result<void> compare(const Json& expected, const Json& got, const Options& options);
[[nodiscard]] std::string render(const DiffReport& report, const Project& project,
                                 const Options& options);
[[nodiscard]] Json to_json(const DiffReport& report, const Project& project);

}  // namespace cppan::diff
}
