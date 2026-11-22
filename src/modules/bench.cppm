export module cppan.bench;
import std;
import cppan.json;
import cppan.project;
import cppan.util;

export {
// `bench`: the original's per-call cost against the port's, on the same cases.



namespace cppan::bench {

struct BenchSample {
  double ns_per_call = 0.0;
  std::uint64_t rss_kb = 0;
};

struct PortBench {
  double ns_per_call = 0.0;
  std::size_t calls = 0;
  std::uint64_t rss_kb = 0;
};

struct BenchReport {
  /// Median over process-level samples.
  double original_ns = 0.0;
  double cpp_ns = 0.0;
  double original_p95_ns = 0.0;
  double cpp_p95_ns = 0.0;
  double original_min_ns = 0.0;
  double cpp_min_ns = 0.0;
  double original_max_ns = 0.0;
  double cpp_max_ns = 0.0;
  /// 95% bootstrap interval for the median.
  double original_ci_low_ns = 0.0;
  double original_ci_high_ns = 0.0;
  double cpp_ci_low_ns = 0.0;
  double cpp_ci_high_ns = 0.0;
  /// Peak RSS is the maximum across samples, not a median.
  std::uint64_t original_rss_kb = 0;
  std::uint64_t cpp_rss_kb = 0;
  std::size_t samples = 1;
  std::size_t calls = 0;
  std::size_t repeat = 0;
  /// What the original is: "python", "C", …
  std::string original_label;

  [[nodiscard]] double speedup() const { return cpp_ns > 0.0 ? original_ns / cpp_ns : 1e308; }
  [[nodiscard]] std::string_view verdict() const;
};

/// Turn per-process timings and RSS into medians, P95, ranges and peak RSS.
[[nodiscard]] BenchReport make_report(std::vector<BenchSample> original,
                                      std::vector<BenchSample> cpp, std::size_t calls,
                                      std::size_t repeat, std::string original_label);

using OriginalBench = std::function<Result<BenchMeasurement>(std::size_t repeat)>;

/// Run the original and the port `samples` times; each sample times `repeat`
/// calls per case. The driver decodes cases once inside each sample.
[[nodiscard]] Result<BenchReport> run_samples(const Project& project,
                                              const std::filesystem::path& driver,
                                              std::size_t repeat, std::size_t samples,
                                              std::string original_label,
                                              const OriginalBench& original);
[[nodiscard]] Result<BenchReport> run(const Project& project,
                                      const std::filesystem::path& driver, std::size_t repeat,
                                      std::size_t samples = 5);
/// The port's side only: the driver times the calls with the arguments decoded once.
[[nodiscard]] Result<PortBench> run_port(const Project& project,
                                         const std::filesystem::path& driver,
                                         std::size_t repeat);
[[nodiscard]] std::string render(const BenchReport& report, const Project& project);
[[nodiscard]] Json to_json(const BenchReport& report, const Project& project);

}  // namespace cppan::bench
}
