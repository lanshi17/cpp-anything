#include "cppan/shadow.hpp"

#include <algorithm>
#include <format>
#include <map>

#include "cppan/codegen.hpp"

namespace cppan::shadow {

std::filesystem::path log_path(const Project& project) {
  return project.dir / "shadow" /
         (codegen::namespace_of(project) + ".shadow.jsonl");
}

Result<Report> read_report(const Project& project) {
  const std::filesystem::path path = log_path(project);
  Report report;
  if (!std::filesystem::exists(path)) return report;
  const auto text = read_file(path);
  if (!text.has_value()) return std::unexpected(path.string() + ": cannot read");
  std::map<std::string, std::size_t> counts;
  for (const std::string_view line : split_lines(*text)) {
    bool empty = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
    }
    if (empty) continue;
    ++report.lines;
    Json doc;
    try {
      doc = parse(line);
    } catch (const JsonError&) {
      continue;
    }
    Mismatch mismatch;
    if (const Json* fn = doc.find("fn"); fn != nullptr && fn->is_string()) {
      mismatch.function = fn->as_string();
    } else {
      mismatch.function = "?";
    }
    ++counts[mismatch.function];
    if (const Json* args = doc.find("args")) mismatch.args = *args;
    if (const Json* expected = doc.find("expected")) mismatch.expected = *expected;
    if (const Json* got = doc.find("got")) mismatch.got = *got;
    if (const Json* reason = doc.find("reason"); reason != nullptr && reason->is_string()) {
      mismatch.reason = reason->as_string();
    }
    if (const Json* at = doc.find("at"); at != nullptr && at->is_string()) {
      mismatch.at = at->as_string();
    }
    report.mismatches.push_back(std::move(mismatch));
  }
  for (const auto& [name, count] : counts) report.by_function.emplace_back(name, count);
  return report;
}

std::string render(const Report& report, const Project& project) {
  if (report.lines == 0) {
    return std::format(
        "cpp-anything shadow: {} — no mismatches logged yet\n\nimport the generated module where "
        "the original was imported and run the app:\n  sys.path.insert(0, \"{}\")\n  import "
        "{}_shadow as ...\n",
        project.module, (project.dir / "shadow").string(), codegen::namespace_of(project));
  }
  std::string out = std::format("cpp-anything shadow: {} — {} mismatch(es) over {} call(s)\n\n",
                                project.module, report.mismatches.size(), report.lines);
  std::size_t width = 8;
  for (const auto& [name, count] : report.by_function) {
    (void)count;
    width = std::max(width, name.size());
  }
  for (const auto& [name, count] : report.by_function) {
    out += std::format("  {}  {}\n", pad_right(name, width), count);
  }
  out += "\nfirst mismatches:\n";
  std::size_t shown = 0;
  for (const Mismatch& mismatch : report.mismatches) {
    if (shown++ >= 5) break;
    std::string args;
    if (mismatch.args.is_array()) {
      for (const Json& arg : mismatch.args.as_array()) {
        if (!args.empty()) args += ", ";
        args += dump(arg);
      }
    }
    out += std::format("\n  {} {}({})\n    {}\n    expected: {}\n    got:      {}\n", mismatch.at,
                       mismatch.function, args, mismatch.reason,
                       truncate_text(dump(mismatch.expected), 140),
                       truncate_text(dump(mismatch.got), 140));
  }
  out += "\nshadow traffic never replaces the original's result: fix the port, keep the traffic "
         "on, then `gate`.\n";
  return out;
}

Json to_json(const Report& report, const Project& project) {
  Json::Array by_function;
  for (const auto& [name, count] : report.by_function) {
    by_function.push_back(Json::object({
        {"name", Json::string(name)},
        {"count", Json::integer(static_cast<std::int64_t>(count))},
    }));
  }
  Json::Array first;
  for (std::size_t index = 0; index < report.mismatches.size() && index < 20; ++index) {
    const Mismatch& mismatch = report.mismatches[index];
    first.push_back(Json::object({
        {"at", Json::string(mismatch.at)},
        {"fn", Json::string(mismatch.function)},
        {"args", mismatch.args},
        {"expected", mismatch.expected},
        {"got", mismatch.got},
        {"reason", Json::string(mismatch.reason)},
    }));
  }
  return Json::object({
      {"schema", Json::string("cpp-anything/shadow@1")},
      {"unit", Json::string(project.unit)},
      {"calls", Json::integer(static_cast<std::int64_t>(report.lines))},
      {"mismatches", Json::integer(static_cast<std::int64_t>(report.mismatches.size()))},
      {"by_function", Json::array(std::move(by_function))},
      {"first", Json::array(std::move(first))},
  });
}

}  // namespace cppan::shadow
