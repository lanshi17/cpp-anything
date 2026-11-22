module;
#include <cmath>

module cppan.bench;
import cppan.json;
import cppan.types;
import cppan.util;
import cppan.python;
import std;

namespace cppan::bench {

std::string_view BenchReport::verdict() const {
  const double factor = speedup();
  if (factor >= 1.05) return "faster";
  if (factor <= 0.95) return "slower";
  return "about the same";
}

Result<std::pair<double, std::size_t>> run_port(const Project& project,
                                                const std::filesystem::path& driver,
                                                std::size_t repeat) {
  const std::filesystem::path cases = project.dir / "cases.jsonl";
  if (!std::filesystem::exists(cases)) {
    return std::unexpected(cases.string() + ": no recorded cases (run `cpp-anything record`)");
  }
  repeat = std::max<std::size_t>(repeat, 1);
  const std::vector<std::string> args = {"--cases", cases.string(), "--bench",
                                         std::to_string(repeat)};
  const Run run = run_command(driver.string(), args, project.dir);
  if (!run.ok) {
    return std::unexpected("the C++ driver's bench failed: " + run.failure());
  }
  std::string payload = run.out;
  const auto leading = [](std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.remove_prefix(1);
    }
    return text;
  };
  if (!leading(payload).starts_with('{')) payload = run.err;
  std::string last;
  for (const std::string_view line : split_lines(payload)) {
    if (leading(line).starts_with('{')) last = line;
  }
  Json cpp;
  try {
    cpp = parse(last);
  } catch (const JsonError& error) {
    return std::unexpected(std::string("driver bench output: ") + error.what());
  }
  const Json* nanos = cpp.find("ns_per_call");
  const Json* calls = cpp.find("calls");
  const double ns = nanos != nullptr && nanos->is_number() ? nanos->as_number() : 0.0;
  const std::size_t total =
      calls != nullptr && calls->is_integer() ? static_cast<std::size_t>(calls->as_int()) : 0;
  return std::pair{ns, total};
}

BenchReport make_report(std::vector<double> original_ns, std::vector<double> cpp_ns,
                        std::size_t calls, std::size_t repeat,
                        std::string original_label) {
  const auto series = [](std::vector<double> values) {
    struct Stats {
      double median = 0.0;
      double p95 = 0.0;
      double min = 0.0;
      double max = 0.0;
      std::size_t count = 0;
    } stats;
    stats.count = values.size();
    if (values.empty()) return stats;
    std::ranges::sort(values);
    const std::size_t middle = values.size() / 2;
    stats.median = values.size() % 2 == 0
                       ? (values[middle - 1] + values[middle]) / 2.0
                       : values[middle];
    const auto nearest_rank = [&](double percentile) {
      std::size_t index = static_cast<std::size_t>(
          std::ceil(percentile * static_cast<double>(values.size())));
      index = std::clamp<std::size_t>(index, 1, values.size());
      return values[index - 1];
    };
    stats.p95 = nearest_rank(0.95);
    stats.min = values.front();
    stats.max = values.back();
    return stats;
  };
  const auto original = series(std::move(original_ns));
  const auto port = series(std::move(cpp_ns));
  BenchReport report;
  report.original_ns = original.median;
  report.cpp_ns = port.median;
  report.original_p95_ns = original.p95;
  report.cpp_p95_ns = port.p95;
  report.original_min_ns = original.min;
  report.cpp_min_ns = port.min;
  report.original_max_ns = original.max;
  report.cpp_max_ns = port.max;
  report.samples = original.count;
  report.calls = calls;
  report.repeat = std::max<std::size_t>(repeat, 1);
  report.original_label = std::move(original_label);
  return report;
}

Result<BenchReport> run_samples(const Project& project, const std::filesystem::path& driver,
                                std::size_t repeat, std::size_t samples,
                                std::string original_label,
                                const OriginalBench& original) {
  repeat = std::max<std::size_t>(repeat, 1);
  samples = std::max<std::size_t>(samples, 1);
  std::vector<double> original_ns;
  std::vector<double> cpp_ns;
  original_ns.reserve(samples);
  cpp_ns.reserve(samples);
  std::size_t calls = 0;
  for (std::size_t sample = 0; sample < samples; ++sample) {
    const bool port_first = sample % 2 != 0;
    const auto take_original = [&]() -> std::optional<Error> {
      auto value = original(repeat);
      if (!value) return value.error();
      original_ns.push_back(*value);
      return std::nullopt;
    };
    if (!port_first) {
      if (auto error = take_original(); error.has_value()) return std::unexpected(*error);
    }
    auto port = run_port(project, driver, repeat);
    if (!port) return std::unexpected(port.error());
    cpp_ns.push_back(port->first);
    calls = port->second;
    if (port_first) {
      if (auto error = take_original(); error.has_value()) return std::unexpected(*error);
    }
  }
  return make_report(std::move(original_ns), std::move(cpp_ns), calls, repeat,
                     std::move(original_label));
}

Result<BenchReport> run(const Project& project, const std::filesystem::path& driver,
                        std::size_t repeat, std::size_t samples) {
  return run_samples(project, driver, repeat, samples, "python",
                     [&](std::size_t calls) { return python::bench(project, calls); });
}

namespace {

std::string format_ns(double nanos) {
  if (nanos >= 1'000'000.0) return std::format("{:.2f} ms", nanos / 1'000'000.0);
  if (nanos >= 1'000.0) return std::format("{:.2f} µs", nanos / 1'000.0);
  return std::format("{:.1f} ns", nanos);
}

}  // namespace

std::string render(const BenchReport& report, const Project& project) {
  const std::size_t cases = report.repeat > 0 ? report.calls / report.repeat : 0;
  std::string out = std::format(
      "cpp-anything bench: {} — {} calls over {} recorded cases, {}x repeated, "
      "{} process sample(s)\n\n",
      project.module, report.calls, cases, report.repeat, report.samples);
  out += std::format("  {:<22} {:>14}/call   P95 {:>14}/call\n",
                     std::format("original ({})", report.original_label),
                     format_ns(report.original_ns), format_ns(report.original_p95_ns));
  out += std::format("  {:<22} {:>14}/call   P95 {:>14}/call\n", "port (C++)",
                     format_ns(report.cpp_ns), format_ns(report.cpp_p95_ns));
  out += std::format("\n  speedup (median): {:.2f}x — the port is {}\n", report.speedup(),
                     report.verdict());
  if (report.samples > 1) {
    out += std::format(
        "\n  Median of {} process-level samples; each sample times {} calls per case.\n",
        report.samples, report.repeat);
  }
  if (report.verdict() == "slower") {
    out +=
        "\n  One call per small item usually costs more than the work: batch the loop\n  (call "
        "once over a list) before concluding the port is not worth it.\n";
  }
  return out;
}

Json to_json(const BenchReport& report, const Project& project) {
  return Json::object({
      {"schema", Json::string("cpp-anything/bench@2")},
      {"unit", Json::string(project.unit)},
      {"samples", Json::integer(static_cast<std::int64_t>(report.samples))},
      {"calls", Json::integer(static_cast<std::int64_t>(report.calls))},
      {"repeat", Json::integer(static_cast<std::int64_t>(report.repeat))},
      {"original_ns_per_call", Json::real(report.original_ns)},
      {"original_p95_ns_per_call", Json::real(report.original_p95_ns)},
      {"original_min_ns_per_call", Json::real(report.original_min_ns)},
      {"original_max_ns_per_call", Json::real(report.original_max_ns)},
      {"original_language", Json::string(report.original_label)},
      {"port_ns_per_call", Json::real(report.cpp_ns)},
      {"port_p95_ns_per_call", Json::real(report.cpp_p95_ns)},
      {"port_min_ns_per_call", Json::real(report.cpp_min_ns)},
      {"port_max_ns_per_call", Json::real(report.cpp_max_ns)},
      {"speedup", Json::real(report.speedup())},
      {"verdict", Json::string(std::string(report.verdict()))},
  });
}

}  // namespace cppan::bench
