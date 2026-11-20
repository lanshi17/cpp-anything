#include "cppan/gate.hpp"

#include <algorithm>
#include <format>

#include "cppan/bench.hpp"
#include "cppan/build.hpp"
#include "cppan/diff.hpp"
#include "cppan/matrix.hpp"

namespace cppan::gate {
namespace {

std::string debug_list(const std::vector<std::string>& items) {
  std::string out = "[";
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index > 0) out += ", ";
    out += quote_debug(items[index]);
  }
  out += "]";
  return out;
}

std::string_view code_of(std::string_view line) {
  const std::size_t comment = line.find("//");
  return comment == std::string_view::npos ? line : line.substr(0, comment);
}

bool explained_above(const std::vector<std::string_view>& lines, std::size_t index,
                     std::string_view marker) {
  for (std::size_t above = index; above > 0; --above) {
    const std::string_view text = lines[above - 1];
    std::string_view trimmed = text;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
      trimmed.remove_prefix(1);
    }
    if (!trimmed.starts_with("//")) break;
    if (text.find(marker) != std::string_view::npos) return true;
  }
  return false;
}

}  // namespace

Result<GateReport> run(const Project& project, const Options& options) {
  GateReport report;
  const Matrix& matrix = Matrix::get();
  std::uint32_t std_number = 0;
  const std::string_view std_text = project.target.standard;
  for (std::size_t index = std_text.starts_with("c++") ? 3 : 0; index < std_text.size();
       ++index) {
    const char ch = std_text[index];
    if (ch < '0' || ch > '9') break;
    std_number = std_number * 10 + static_cast<std::uint32_t>(ch - '0');
  }
  const auto known = matrix.resolve_toolchain(project.target.toolchain);
  const MacroStatus expected = matrix.macro_available(
      "__cpp_lib_expected", project.target.toolchain, project.target.version,
      project.target.standard);
  bool ok = false;
  std::string detail;
  if (!known.has_value()) {
    detail = "unknown toolchain " + quote_debug(project.target.toolchain) +
             "; `cpp-anything matrix` lists the ones it knows";
  } else if (std_number < 20) {
    detail = project.target.standard + " is below C++20; cpp-anything generates C++20/23";
  } else {
    ok = true;
    if (expected.kind == MacroStatusKind::Available) {
      detail = std::format("{} {} with {}: std::expected since {}", *known,
                           project.target.version, project.target.standard, expected.release);
    } else {
      detail = std::format("{} {} with {}: no std::expected, the port uses cppan::Result", *known,
                           project.target.version, project.target.standard);
    }
  }
  report.steps.push_back(Step{"matrix", ok, detail});

  const std::filesystem::path port_path = project.dir / "src" / "port.cpp";
  const auto port_file = read_file(port_path);
  if (!port_file.has_value()) return std::unexpected(port_path.string() + ": cannot read");
  std::string code;
  {
    bool first = true;
    for (const std::string_view line : split_lines(*port_file)) {
      if (!first) code += '\n';
      first = false;
      code += code_of(line);
    }
  }
  std::size_t stubs = 0;
  for (std::size_t at = 0; (at = code.find("CPPAN_STUB", at)) != std::string::npos;
       at += std::string_view("CPPAN_STUB").size()) {
    ++stubs;
  }
  report.steps.push_back(Step{"no stubs", stubs == 0,
                              stubs == 0 ? "every function has a real body"
                                         : std::format("{} function(s) still return CPPAN_STUB",
                                                       stubs)});

  auto cases = diff::load_cases(project.dir / "cases.jsonl");
  std::vector<const Function*> failing;
  for (const Function& function : project.functions) {
    if (function.raises) failing.push_back(&function);
  }
  if (failing.empty()) {
    report.steps.push_back(
        Step{"error cases", true, "the original fails on purpose nowhere"});
  } else {
    std::vector<std::string> missing;
    for (const Function* function : failing) {
      bool found = false;
      if (cases.has_value()) {
        for (const diff::Case& entry : *cases) {
          if (entry.function == function->name && entry.expect.find("err") != nullptr) {
            found = true;
            break;
          }
        }
      }
      if (!found) missing.push_back(function->name);
    }
    std::string joined;
    for (const std::string& name : missing) {
      if (!joined.empty()) joined += ", ";
      joined += name;
    }
    report.steps.push_back(Step{
        "error cases", missing.empty(),
        missing.empty()
            ? std::format("each of the {} function(s) that fail in the original has a recorded "
                          "failing case",
                          failing.size())
            : "no recorded case reaches these error paths: " + joined +
                  " (add inputs that fail: `record --from-tests` or `--inputs`)"});
  }

  std::vector<std::string> crashes;
  if (!failing.empty()) {
    const auto lines = split_lines(*port_file);
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const std::string_view line = lines[index];
      const std::string_view source_code = code_of(line);
      std::optional<std::string_view> token;
      for (const std::string_view candidate : {"std::abort", "std::terminate", "assert(",
                                               "throw "}) {
        if (source_code.find(candidate) != std::string_view::npos) {
          token = candidate;
          break;
        }
      }
      if (!token.has_value()) continue;
      const bool explained = line.find("INFALLIBLE:") != std::string_view::npos ||
                             explained_above(lines, index, "INFALLIBLE:");
      if (!explained) {
        crashes.push_back(std::format("src/port.cpp:{} `{}`", index + 1, *token));
      }
    }
  }
  {
    std::string joined;
    for (const std::string& entry : crashes) {
      if (!joined.empty()) joined += ", ";
      joined += entry;
    }
    report.steps.push_back(Step{
        "panics", crashes.empty(),
        crashes.empty()
            ? (failing.empty() ? "no abort/assert/throw to review"
                               : "no abort, assert or throw where the original returns an error")
            : "return `cppan::unexpected(PortError{...})` instead, or mark an impossible state "
              "with `// INFALLIBLE: <why>`: " +
                  joined});
  }

  if (project.source_lang == "cpp") {
    const auto lines = split_lines(*port_file);
    std::vector<std::string> c_style;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const std::string_view line = lines[index];
      const std::string_view source_code = code_of(line);
      std::optional<std::string_view> needle;
      for (const std::string_view candidate :
           {"NULL", "malloc(", "free(", "strcpy", "strlen", "memcpy", "printf(", "sprintf(",
            "(char*)", "(int)", "(double)", "typedef struct"}) {
        if (source_code.find(candidate) != std::string_view::npos) {
          needle = candidate;
          break;
        }
      }
      if (!needle.has_value()) continue;
      const bool explained = line.find("C-STYLE:") != std::string_view::npos ||
                             explained_above(lines, index, "C-STYLE:");
      if (!explained) {
        c_style.push_back(std::format("src/port.cpp:{} `{}`", index + 1, *needle));
      }
    }
    std::string joined;
    for (const std::string& entry : c_style) {
      if (!joined.empty()) joined += ", ";
      joined += entry;
    }
    report.steps.push_back(Step{
        "modern", c_style.empty(),
        c_style.empty()
            ? "no C idioms left in the port: string_view, spans, ranges and the profile-gated "
              "facilities"
            : "replace these with the modern facility the analyzer named, or annotate `// "
              "C-STYLE: why it stays`: " +
                  joined});
  }

  const std::size_t port_lines = split_lines(*port_file).size();
  const std::size_t source_lines = project.lines;
  if (source_lines > 0) {
    const double growth = static_cast<double>(port_lines) / static_cast<double>(source_lines);
    const double allowed = 3.0 * static_cast<double>(source_lines) + 50.0;
    report.steps.push_back(Step{
        "size", static_cast<double>(port_lines) <= allowed,
        std::format("{} lines of C++ for {} lines of {} ({:.1f}x)", port_lines, source_lines,
                    project.source_lang, growth)});
  }

  const std::filesystem::path recorded_from = project.dir / kMetaDir / "recorded-from.json";
  std::optional<Json> recorded_doc;
  if (const auto text = read_file(recorded_from); text.has_value()) {
    try {
      recorded_doc = parse(*text);
    } catch (const JsonError&) {
      recorded_doc = std::nullopt;
    }
  }
  const std::string source_hash = hash64(read_file(project.source_file).value_or(std::string()));
  std::string stored_hash;
  if (recorded_doc.has_value()) {
    if (const Json* hash = recorded_doc->find("source_hash");
        hash != nullptr && hash->is_string()) {
      stored_hash = hash->as_string();
    }
  }
  report.steps.push_back(Step{
      "recording", stored_hash.empty() || stored_hash == source_hash,
      stored_hash.empty()
          ? "no recorded-from.json: record again after editing the original"
          : (stored_hash == source_hash
                 ? "the cases were recorded from this version of the original"
                 : short_path(project.source_file) +
                       " changed since it was recorded: `cpp-anything record " +
                       project.dir.string() + "` again")});

  if (recorded_doc.has_value()) {
    if (const Json* coverage = recorded_doc->find("coverage"); coverage != nullptr) {
      const Json* reached_field = coverage->find("reached");
      const Json* lines_field = coverage->find("lines");
      const std::int64_t reached = reached_field != nullptr && reached_field->is_integer()
                                       ? reached_field->as_int()
                                       : 0;
      const std::int64_t lines = lines_field != nullptr && lines_field->is_integer()
                                     ? lines_field->as_int()
                                     : 0;
      if (lines > 0) {
        const std::int64_t percent = static_cast<std::int64_t>(
            std::llround(static_cast<double>(reached) / static_cast<double>(lines) * 100.0));
        report.steps.push_back(Step{
            "coverage", true,
            std::format("the recorded cases ran {} of {} source lines ({}%)", reached, lines,
                        percent)});
      }
    }
  }

  auto outcome = build::build_project(project, options.clean, options.cxx, options.jobs);
  std::optional<std::filesystem::path> driver;
  if (outcome) {
    report.steps.push_back(Step{
        "build", true,
        std::format("{} ({} warning{})", short_path(outcome->driver), outcome->warnings,
                    outcome->warnings == 1 ? "" : "s")});
    driver = outcome->driver;
  } else {
    report.steps.push_back(Step{"build", false, outcome.error()});
  }

  if (driver.has_value()) {
    const Run listed = run_command(driver->string(), {"--list"}, project.dir);
    std::vector<std::string> actual;
    for (const std::string_view line : split_lines(listed.out)) {
      std::string text(line);
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
      }
      if (!text.empty()) actual.push_back(text);
    }
    std::vector<std::string> expected_names;
    for (const Function& function : project.functions) expected_names.push_back(function.name);
    std::ranges::sort(expected_names);
    std::vector<std::string> sorted_actual = actual;
    std::ranges::sort(sorted_actual);
    const bool matched = listed.ok && sorted_actual == expected_names;
    report.steps.push_back(Step{
        "dispatch", matched,
        matched
            ? std::format("{} function(s) reachable from the driver", expected_names.size())
            : "expected " + debug_list(expected_names) + " from the driver, got " +
                  debug_list(actual)});
  }

  if (driver.has_value() && cases.has_value()) {
    diff::Options diff_options;
    diff_options.max_report = 5;
    auto report_diff = diff::run(project, *driver, diff_options);
    if (report_diff) {
      std::string recorded;
      if (project.recorded.has_value()) {
        recorded = std::format(" (recorded {}, {} cases)", project.recorded->at,
                               project.recorded->cases);
      }
      report.steps.push_back(Step{
          "diff", report_diff->passed(),
          std::format("{}/{} cases match{}", report_diff->matched, report_diff->total,
                      recorded)});
    } else {
      report.steps.push_back(Step{"diff", false, report_diff.error()});
    }
  } else if (cases.has_value()) {
    report.steps.push_back(Step{"diff", false, "skipped: no build"});
  } else {
    report.steps.push_back(Step{"diff", false, cases.error()});
  }

  if (options.relevance.has_value() && driver.has_value()) {
    auto bench = bench::run(project, *driver, options.relevance->first);
    if (bench) {
      const bool fast = bench->speedup() >= 1.0;
      report.steps.push_back(Step{
          "bench", fast,
          std::format("{:.2f}x ({:.0f} -> {:.0f} ns/call; asked for at least {}x)",
                      bench->speedup(), bench->original_ns, bench->cpp_ns,
                      options.relevance->second)});
    } else {
      report.steps.push_back(Step{"bench", false, bench.error()});
    }
  }

  return report;
}

std::string render(const GateReport& report, const Project& project) {
  std::string out = std::format("cpp-anything gate: {} — {}\n\n", project.module,
                                report.passed() ? "PASS" : "FAIL");
  std::size_t width = 6;
  for (const Step& step : report.steps) width = std::max(width, step.name.size());
  for (const Step& step : report.steps) {
    const auto lines = split_lines(step.detail);
    out += "  " + std::string(step.ok ? "✓" : "✗") + "  " +
           pad_right(step.name, width + 2) + "  " +
           (lines.empty() ? std::string() : std::string(lines.front())) + "\n";
    for (std::size_t index = 1; index < lines.size(); ++index) {
      out += "     " + std::string(width + 2, ' ') + "  " + std::string(lines[index]) + "\n";
    }
  }
  if (!report.passed()) {
    out += "\nnot done: fix the ✗ steps above, then run gate again.\n";
  } else {
    out +=
        "\ndone: the port matches every recorded case and the toolchain supports what it uses.\n";
  }
  return out;
}

Json to_json(const GateReport& report, const Project& project) {
  Json::Array steps;
  for (const Step& step : report.steps) {
    steps.push_back(Json::object({
        {"name", Json::string(step.name)},
        {"ok", Json::boolean(step.ok)},
        {"detail", Json::string(step.detail)},
    }));
  }
  return Json::object({
      {"schema", Json::string("cpp-anything/gate@1")},
      {"unit", Json::string(project.unit)},
      {"passed", Json::boolean(report.passed())},
      {"steps", Json::array(std::move(steps))},
  });
}

}  // namespace cppan::gate
