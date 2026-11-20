export module cppan.bench;
import std;
import cppan.json;
import cppan.project;
import cppan.util;

export {
// `bench`: the original's per-call cost against the port's, on the same cases.



namespace cppan::bench {

struct BenchReport {
  double original_ns = 0.0;
  double cpp_ns = 0.0;
  std::size_t calls = 0;
  std::size_t repeat = 0;
  /// What the original is: "python", "C", …
  std::string original_label;

  [[nodiscard]] double speedup() const { return cpp_ns > 0.0 ? original_ns / cpp_ns : 1e308; }
  [[nodiscard]] std::string_view verdict() const;
};

[[nodiscard]] Result<BenchReport> run(const Project& project,
                                      const std::filesystem::path& driver, std::size_t repeat);
/// The port's side only: the driver times the calls with the arguments decoded once.
[[nodiscard]] Result<std::pair<double, std::size_t>> run_port(
    const Project& project, const std::filesystem::path& driver, std::size_t repeat);
[[nodiscard]] std::string render(const BenchReport& report, const Project& project);
[[nodiscard]] Json to_json(const BenchReport& report, const Project& project);

}  // namespace cppan::bench
}
