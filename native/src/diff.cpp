#include "cppan/diff.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <format>
#include <map>

#include "cppan/recording.hpp"

namespace cppan::diff {
namespace {

std::string real_text(double value) {
  std::string text = std::format("{}", value);
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}

std::string kind_name(const Json& value) {
  switch (value.kind()) {
    case Json::Kind::Null: return "null";
    case Json::Kind::Bool: return "bool";
    case Json::Kind::Int: return "int";
    case Json::Kind::Real: return "float";
    case Json::Kind::String: return "string";
    case Json::Kind::Array: return "list";
    case Json::Kind::Object: return "object";
  }
  return "value";
}

std::string describe(const Json& expected, const Json& got) {
  if (kind_name(expected) != kind_name(got)) {
    return "type: expected a " + kind_name(expected) + ", the port returns a " + kind_name(got);
  }
  if (expected.is_string() && got.is_string()) {
    return "text: expected " + quote_debug(truncate_text(expected.as_string(), 70)) + ", got " +
           quote_debug(truncate_text(got.as_string(), 70));
  }
  if (expected.is_array() && got.is_array() &&
      expected.as_array().size() != got.as_array().size()) {
    return std::format("length: expected {} items, got {}", expected.as_array().size(),
                       got.as_array().size());
  }
  if (expected.is_real() && got.is_real()) {
    const double a = expected.as_real();
    const double b = got.as_real();
    if (std::bit_cast<std::uint64_t>(a) != std::bit_cast<std::uint64_t>(b) && a == b) {
      return std::format(
          "-0.0 vs 0.0: expected {}, got {} (bit-exact floats; use --tolerance)",
          real_text(a), real_text(b));
    }
  }
  return "value: expected " + truncate_text(dump(expected), 90) + ", got " +
         truncate_text(dump(got), 90);
}

void record_history(const Project& project, const DiffReport& report) {
  const std::filesystem::path path = project.dir / kMetaDir / "diff-history.jsonl";
  const Json entry = Json::object({
      {"at", Json::string(now_iso8601())},
      {"passed", Json::integer(static_cast<std::int64_t>(report.matched))},
      {"total", Json::integer(static_cast<std::int64_t>(report.total))},
      {"ok", Json::boolean(report.passed())},
  });
  std::string text = read_file(path).value_or(std::string());
  text += dump(entry);
  text += '\n';
  (void)write_file(path, text);
}

}  // namespace

Result<std::vector<Case>> load_cases(const std::filesystem::path& path) {
  const auto text = read_file(path);
  if (!text.has_value()) {
    return std::unexpected(path.string() + ": cannot read (run `cpp-anything record` first)");
  }
  std::vector<Case> cases;
  std::size_t number = 0;
  for (const std::string_view line : split_lines(*text)) {
    ++number;
    bool empty = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
    }
    if (empty) continue;
    Json doc;
    try {
      doc = parse(line);
    } catch (const JsonError& error) {
      return std::unexpected(path.string() + ":" + std::to_string(number) + ": " + error.what());
    }
    Case entry;
    if (const Json* id = doc.find("id"); id != nullptr && id->is_string()) {
      entry.id = id->as_string();
    }
    if (const Json* fn = doc.find("fn"); fn != nullptr && fn->is_string()) {
      entry.function = fn->as_string();
    }
    if (const Json* args = doc.find("args")) entry.args = *args;
    if (const Json* expect = doc.find("expect")) entry.expect = *expect;
    if (entry.id.empty() || entry.function.empty()) {
      return std::unexpected(path.string() + ":" + std::to_string(number) +
                             ": case without id or fn");
    }
    cases.push_back(std::move(entry));
  }
  if (cases.empty()) return std::unexpected(path.string() + ": no cases");
  return cases;
}

Result<void> compare(const Json& expected, const Json& got, const Options& options) {
  // A recorded crash is undefined behaviour: any port behaviour is acceptable there.
  if (expected.find("crash") != nullptr) return {};
  const Json* expected_err = expected.find("err");
  const Json* got_err = got.find("err");
  if (expected_err != nullptr && got_err != nullptr) {
    const Json* expected_type_field = expected_err->find("type");
    const Json* actual_type_field = got_err->find("type");
    const std::string_view expected_type =
        expected_type_field != nullptr && expected_type_field->is_string()
            ? std::string_view(expected_type_field->as_string())
            : std::string_view("?");
    const std::string_view actual_type =
        actual_type_field != nullptr && actual_type_field->is_string()
            ? std::string_view(actual_type_field->as_string())
            : std::string_view("?");
    if (expected_type != actual_type) {
      return std::unexpected("error type: the original raises " + std::string(expected_type) +
                             ", the port raises " + std::string(actual_type));
    }
    if (options.strict_errors) {
      const Json* expected_msg_field = expected_err->find("msg");
      const Json* actual_msg_field = got_err->find("msg");
      const std::string_view expected_msg =
          expected_msg_field != nullptr && expected_msg_field->is_string()
              ? std::string_view(expected_msg_field->as_string())
              : std::string_view();
      const std::string_view actual_msg =
          actual_msg_field != nullptr && actual_msg_field->is_string()
              ? std::string_view(actual_msg_field->as_string())
              : std::string_view();
      if (expected_msg != actual_msg) {
        return std::unexpected("error message: expected " + quote_debug(truncate_text(expected_msg, 80)) +
                               ", got " + quote_debug(truncate_text(actual_msg, 80)));
      }
    }
    return {};
  }
  if (expected_err != nullptr) {
    const Json* type = expected_err->find("type");
    const std::string expected_name =
        type != nullptr && type->is_string() ? type->as_string() : "an error";
    const Json* ok = got.find("ok");
    return std::unexpected("the original raises " + expected_name + ", the port returns " +
                           truncate_text(ok != nullptr ? dump(*ok) : dump(got), 60));
  }
  if (got_err != nullptr) {
    const Json* type = got_err->find("type");
    const std::string actual_name =
        type != nullptr && type->is_string() ? type->as_string() : "an error";
    return std::unexpected("the port raises " + actual_name +
                           ", the original returns a value");
  }
  const Json* expected_value = expected.find("ok");
  const Json* actual_value = got.find("ok");
  const Json& left = expected_value != nullptr ? *expected_value : expected;
  const Json& right = actual_value != nullptr ? *actual_value : got;
  if (Json::equal(left, right, options.tolerance)) return {};
  return std::unexpected(describe(left, right));
}

Result<DiffReport> run(const Project& project, const std::filesystem::path& driver,
                       const Options& options) {
  const std::filesystem::path cases_path = project.dir / "cases.jsonl";
  auto cases = load_cases(cases_path);
  if (!cases) return std::unexpected(cases.error());
  const std::filesystem::path results_path = project.dir / "build" / "results.jsonl";
  std::vector<std::string> args = {"--cases",
                                   cases_path.string(),
                                   "--out",
                                   results_path.string(),
                                   "--quiet"};
  if (options.only.has_value()) {
    args.emplace_back("--only");
    args.push_back(*options.only);
  }
  const Run invoked = run_command(driver.string(), args, project.dir);
  if (!invoked.ok) {
    return std::unexpected("the driver failed: " + invoked.failure() + "\n" +
                           indent_text(invoked.err, 2));
  }
  const auto text = read_file(results_path);
  if (!text.has_value()) return std::unexpected(results_path.string() + ": cannot read");
  std::map<std::string, Json> results;
  for (const std::string_view line : split_lines(*text)) {
    bool empty = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
    }
    if (empty) continue;
    Json doc;
    try {
      doc = parse(line);
    } catch (const JsonError& error) {
      return std::unexpected(std::string("results: ") + error.what());
    }
    const Json* id = doc.find("id");
    const Json* result = doc.find("result");
    if (id != nullptr && id->is_string() && result != nullptr) {
      results[id->as_string()] = *result;
    }
  }

  DiffReport report;
  std::map<std::string, std::pair<std::size_t, std::size_t>> counts;
  for (const Case& entry : *cases) {
    if (options.only.has_value() && *options.only != entry.function) continue;
    ++report.total;
    auto& count = counts[entry.function];
    ++count.first;
    if (entry.expect.is_null()) {
      ++report.skipped;
      continue;
    }
    const auto found = results.find(entry.id);
    if (found == results.end()) {
      report.missing.push_back(entry.id);
      continue;
    }
    auto compared = compare(entry.expect, found->second, options);
    if (compared) {
      ++report.matched;
      ++count.second;
    } else {
      report.mismatches.push_back(Mismatch{entry.id, entry.function, entry.args, entry.expect,
                                           found->second, compared.error()});
    }
  }
  for (const auto& [name, pair] : counts) {
    report.per_function.emplace_back(name, pair.first, pair.second);
  }
  record_history(project, report);
  return report;
}

std::string render(const DiffReport& report, const Project& project, const Options& options) {
  std::string out = std::format("cpp-anything diff: {} — {}/{} cases {}", project.module,
                                report.matched, report.total,
                                report.passed() ? "match" : "MISMATCH");
  if (report.skipped > 0) {
    out += std::format(" ({} skipped: nothing recorded)", report.skipped);
  }
  out += '\n';
  std::size_t width = 8;
  for (const auto& [name, total, passed] : report.per_function) {
    (void)total;
    (void)passed;
    std::size_t chars = 0;
    for (const char ch : name) {
      if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) ++chars;
    }
    width = std::max(width, chars);
  }
  out += "\n  " + pad_right("function", width) + "  cases  pass\n";
  for (const auto& [name, total, passed] : report.per_function) {
    out += "  " + pad_right(name, width) + "  " + pad_left(std::to_string(total), 5) + "  " +
           pad_left(std::to_string(passed), 4) + "\n";
  }
  if (!report.missing.empty()) {
    out += std::format("\n  {} cases produced no result\n", report.missing.size());
  }
  if (!report.mismatches.empty()) {
    out += std::format("\n{} mismatches (first {}):\n", report.mismatches.size(),
                       std::min(options.max_report, report.mismatches.size()));
    std::size_t shown = 0;
    for (const Mismatch& mismatch : report.mismatches) {
      if (shown++ >= options.max_report) break;
      std::string args;
      if (mismatch.args.is_array()) {
        for (const Json& arg : mismatch.args.as_array()) {
          if (!args.empty()) args += ", ";
          args += dump(arg);
        }
      }
      out += std::format("\n  {}  {}({})\n    {}\n    expected: {}\n    got:      {}\n",
                         mismatch.id, mismatch.function, args, mismatch.reason,
                         truncate_text(dump(mismatch.expected), 140),
                         truncate_text(dump(mismatch.got), 140));
    }
    if (report.mismatches.size() > options.max_report) {
      out += std::format("\n  … {} more (raise --max-report to see them)\n",
                         report.mismatches.size() - options.max_report);
    }
  }
  if (report.passed()) {
    out +=
        "\nevery recorded case matches: the port behaves like the original on what was recorded.\n";
  }
  return out;
}

Json to_json(const DiffReport& report, const Project& project) {
  Json::Array missing;
  for (const std::string& id : report.missing) missing.push_back(Json::string(id));
  Json::Array functions;
  for (const auto& [name, total, passed] : report.per_function) {
    functions.push_back(Json::object({
        {"name", Json::string(name)},
        {"cases", Json::integer(static_cast<std::int64_t>(total))},
        {"passed", Json::integer(static_cast<std::int64_t>(passed))},
    }));
  }
  Json::Array mismatches;
  for (const Mismatch& mismatch : report.mismatches) {
    mismatches.push_back(Json::object({
        {"id", Json::string(mismatch.id)},
        {"fn", Json::string(mismatch.function)},
        {"args", mismatch.args},
        {"expected", mismatch.expected},
        {"got", mismatch.got},
        {"reason", Json::string(mismatch.reason)},
    }));
  }
  return Json::object({
      {"schema", Json::string("cpp-anything/diff@1")},
      {"unit", Json::string(project.unit)},
      {"module", Json::string(project.module)},
      {"cases", Json::integer(static_cast<std::int64_t>(report.total))},
      {"matched", Json::integer(static_cast<std::int64_t>(report.matched))},
      {"skipped", Json::integer(static_cast<std::int64_t>(report.skipped))},
      {"passed", Json::boolean(report.passed())},
      {"missing", Json::array(std::move(missing))},
      {"functions", Json::array(std::move(functions))},
      {"mismatches", Json::array(std::move(mismatches))},
  });
}

}  // namespace cppan::diff
