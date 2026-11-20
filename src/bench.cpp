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

Result<BenchReport> run(const Project& project, const std::filesystem::path& driver,
                        std::size_t repeat) {
  repeat = std::max<std::size_t>(repeat, 1);
  auto port = run_port(project, driver, repeat);
  if (!port) return std::unexpected(port.error());
  auto original = python::bench(project, repeat);
  if (!original) return std::unexpected(original.error());
  BenchReport report;
  report.original_ns = *original;
  report.cpp_ns = port->first;
  report.calls = port->second;
  report.repeat = repeat;
  report.original_label = "python";
  return report;
}

namespace {

std::string format_ns(double nanos) {
  if (nanos >= 1'000'000.0) return std::format("{:.2f} ms", nanos / 1'000'000.0);
  if (nanos >= 1'000.0) return std::format("{:.2f} µs", nanos / 1'000.0);
  return std::format("{:.1f}", nanos);
}

}  // namespace

std::string render(const BenchReport& report, const Project& project) {
  const std::size_t cases = report.repeat > 0 ? report.calls / report.repeat : 0;
  std::string out = std::format(
      "cpp-anything bench: {} — {} calls over {} recorded cases, {}x repeated\n\n",
      project.module, report.calls, cases, report.repeat);
  out += std::format("  {:<26} {:>12}/call\n",
                     std::format("original ({})", report.original_label),
                     format_ns(report.original_ns));
  out += std::format("  {:<26} {:>12}/call\n", "port (C++)", format_ns(report.cpp_ns));
  out += std::format("\n  speedup: {:.2f}x — the port is {}\n", report.speedup(),
                     report.verdict());
  if (report.verdict() == "slower") {
    out +=
        "\n  One call per small item usually costs more than the work: batch the loop\n  (call "
        "once over a list) before concluding the port is not worth it.\n";
  }
  return out;
}

Json to_json(const BenchReport& report, const Project& project) {
  return Json::object({
      {"schema", Json::string("cpp-anything/bench@1")},
      {"unit", Json::string(project.unit)},
      {"calls", Json::integer(static_cast<std::int64_t>(report.calls))},
      {"repeat", Json::integer(static_cast<std::int64_t>(report.repeat))},
      {"original_ns_per_call", Json::real(report.original_ns)},
      {"original_language", Json::string(report.original_label)},
      {"port_ns_per_call", Json::real(report.cpp_ns)},
      {"speedup", Json::real(report.speedup())},
      {"verdict", Json::string(std::string(report.verdict()))},
  });
}

}  // namespace cppan::bench
