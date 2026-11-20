export module cppan.gate;
import std;
import cppan.json;
import cppan.project;
import cppan.util;

export {
// `gate`: everything a finished port has to satisfy, in one exit code.



namespace cppan::gate {

struct Step {
  std::string name;
  bool ok = false;
  std::string detail;
};

struct GateReport {
  std::vector<Step> steps;

  [[nodiscard]] bool passed() const {
    return std::ranges::all_of(steps, [](const Step& step) { return step.ok; });
  }
};

struct Options {
  bool clean = false;
  std::optional<std::string> cxx;
  std::size_t jobs = 4;
  /// (repeat, minimum records) for --bench
  std::optional<std::pair<std::size_t, std::size_t>> relevance;
};

[[nodiscard]] Result<GateReport> run(const Project& project, const Options& options);
[[nodiscard]] std::string render(const GateReport& report, const Project& project);
[[nodiscard]] Json to_json(const GateReport& report, const Project& project);

}  // namespace cppan::gate
}
