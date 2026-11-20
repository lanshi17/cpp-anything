#include "cppan/python.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "cppan/probe_py.hpp"
#include "cppan/sitecustomize_py.hpp"

#include <set>

namespace cppan::python {
namespace {

std::string trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

/// The probe is written to a stable path keyed by its own hash, so a run never
/// leaves a new file behind and an edit to probe.py shows up immediately.
Result<std::filesystem::path> probe_path() {
  std::error_code error;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path(error) / "cpp-anything";
  if (error) return std::unexpected(directory.string() + ": " + error.message());
  std::filesystem::create_directories(directory, error);
  if (error) return std::unexpected(directory.string() + ": " + error.message());
  const std::string_view source(data::probe_py, data::probe_py_size);
  const std::filesystem::path path = directory / ("probe-" + hash64(source) + ".py");
  if (!std::filesystem::exists(path, error)) {
    if (auto written = write_file(path, source); !written) return std::unexpected(written.error());
  }
  return path;
}

Result<std::string> probe(const std::vector<std::string>& args,
                          const std::optional<std::filesystem::path>& cwd = std::nullopt) {
  auto script = probe_path();
  if (!script) return std::unexpected(script.error());
  std::vector<std::string> argv;
  argv.reserve(args.size() + 1);
  argv.push_back(script->string());
  argv.insert(argv.end(), args.begin(), args.end());
  const Run run = run_command(interpreter(), argv, cwd);
  if (!run.ok) {
    const auto first_lines = [](const std::string& text, std::size_t limit) {
      std::string out;
      std::size_t seen = 0;
      for (std::size_t index = 0; index < text.size() && seen < limit;) {
        const std::size_t newline = text.find('\n', index);
        const std::size_t end = newline == std::string::npos ? text.size() : newline;
        if (!out.empty()) out += '\n';
        out += text.substr(index, end - index);
        ++seen;
        if (newline == std::string::npos) break;
        index = newline + 1;
      }
      return out;
    };
    std::string detail = first_lines(run.err, 30);
    if (trim(detail).empty()) detail = first_lines(run.out, 10);
    std::string headline = run.failure();
    if (const std::size_t colon = headline.find(':'); colon != std::string::npos) {
      headline.resize(colon);
    }
    return std::unexpected("python probe failed (" + headline + "):\n" + indent_text(detail, 2));
  }
  return run.out;
}

}  // namespace

std::string interpreter() {
  if (const char* chosen = std::getenv("CPPAN_PYTHON"); chosen != nullptr && *chosen != '\0') {
    return chosen;
  }
  return "python3";
}

std::optional<std::string> available() {
  const Run run = run_command(interpreter(), {"-c", "import sys; print(sys.version.split()[0])"});
  if (!run.ok) return std::nullopt;
  return trim(run.out);
}

Result<std::string> probe_command(const std::vector<std::string>& args) { return probe(args); }

Result<Unit> inventory(const std::filesystem::path& path, std::string_view module,
                       const std::optional<std::filesystem::path>& package_root) {
  std::vector<std::string> args = {"inventory", "--path", path.string(), "--module",
                                   std::string(module), "--unit", std::string(module)};
  if (package_root.has_value()) {
    args.push_back("--package-root");
    args.push_back(package_root->string());
  }
  auto output = probe(args);
  if (!output) return std::unexpected(output.error());
  try {
    return Unit::parse(parse(trim(*output)));
  } catch (const JsonError& failure) {
    return std::unexpected(std::string("probe returned invalid JSON: ") + failure.what());
  }
}

Result<double> bench(const Project& project, std::size_t repeat) {
  const std::filesystem::path cases = project.dir / "cases.jsonl";
  std::vector<std::string> args = {"bench",
                                   "--path",
                                   project.source_file.string(),
                                   "--module",
                                   project.module,
                                   "--cases",
                                   cases.string(),
                                   "--repeat",
                                   std::to_string(repeat)};
  if (project.package_root.has_value()) {
    args.push_back("--package-root");
    args.push_back(project.package_root->string());
  }
  auto output = probe(args, project.dir);
  if (!output) return std::unexpected(output.error());
  try {
    const Json doc = parse(trim(*output));
    if (const Json* nanos = doc.find("ns_per_call");
        nanos != nullptr && nanos->is_number()) {
      return nanos->as_number();
    }
  } catch (const JsonError& failure) {
    return std::unexpected(std::string("probe returned invalid JSON: ") + failure.what());
  }
  return std::unexpected("probe bench returned no ns_per_call: " + trim(*output));
}

Result<FromTests> record_from_tests(const Project& project,
                                   const std::vector<std::string>& command) {
  if (command.empty()) {
    return std::unexpected(
        "--from-tests needs the test command after `--`, e.g. `record . --from-tests -- pytest "
        "-q`");
  }
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / "cpp-anything" /
      ("from-tests-" + std::to_string(static_cast<long long>(getpid())));
  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  std::filesystem::create_directories(scratch, error);
  if (error) return std::unexpected(scratch.string() + ": " + error.message());
  const std::filesystem::path calls_path = scratch / "calls.jsonl";
  const std::filesystem::path coverage_path = scratch / "coverage.json";
  std::string hook(data::sitecustomize_py, data::sitecustomize_py_size);
  const auto replace_all = [](std::string& text, std::string_view needle,
                              std::string_view value) {
    std::size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
      text.replace(at, needle.size(), value);
      at += value.size();
    }
  };
  replace_all(hook, "{{MODULE}}", project.module);
  replace_all(hook, "{{FILE}}", project.source_file.string());
  replace_all(hook, "{{OUT}}", calls_path.string());
  replace_all(hook, "{{COVERAGE}}", coverage_path.string());
  if (auto written = write_file(scratch / "sitecustomize.py", hook); !written) {
    return std::unexpected(written.error());
  }

  const char* existing_path = std::getenv("PYTHONPATH");
  const std::string existing = existing_path != nullptr ? existing_path : "";
#if defined(_WIN32)
  constexpr char kSeparator = ';';
#else
  constexpr char kSeparator = ':';
#endif
  const std::string pythonpath =
      existing.empty() ? scratch.string() : scratch.string() + kSeparator + existing;
  const std::string program = command.front();
  std::vector<std::string> rest(command.begin() + 1, command.end());
  const std::filesystem::path cwd =
      project.package_root.value_or(project.source_file.parent_path());
  const Run run = run_command(program, rest, cwd, {{"PYTHONPATH", pythonpath}});

  std::size_t calls = 0;
  std::size_t added = 0;
  std::set<std::string> seen;
  const std::filesystem::path cases_path = project.dir / "cases.jsonl";
  if (const auto existing_cases = read_file(cases_path); existing_cases.has_value()) {
    for (const std::string_view line : split_lines(*existing_cases)) {
      try {
        const Json doc = parse(line);
        const Json* fn = doc.find("fn");
        const Json* args = doc.find("args");
        const std::string key = std::string(fn != nullptr && fn->is_string() ? fn->as_string() : "") +
                                "\x01" + (args != nullptr ? dump(*args) : "null");
        seen.insert(key);
      } catch (const JsonError&) {
        continue;
      }
    }
  }
  std::string appended;
  if (const auto text = read_file(calls_path); text.has_value()) {
    for (const std::string_view line : split_lines(*text)) {
      Json doc;
      try {
        doc = parse(line);
      } catch (const JsonError&) {
        continue;
      }
      const Json* function = doc.find("fn");
      if (function == nullptr || !function->is_string()) continue;
      const Json* args_field = doc.find("args");
      const Json args = args_field != nullptr ? *args_field : Json::array();
      const std::string key = function->as_string() + "\x01" + dump(args);
      ++calls;
      if (!seen.insert(key).second) continue;
      ++added;
      const Json* expect = doc.find("expect");
      const Json entry = Json::object({
          {"id", Json::string(function->as_string() + ":suite-" + std::to_string(added))},
          {"fn", Json::string(function->as_string())},
          {"args", args},
          {"expect", expect != nullptr ? *expect : Json::null()},
      });
      appended += dump(entry);
      appended += '\n';
    }
  }
  if (!appended.empty()) {
    std::string text = read_file(cases_path).value_or(std::string());
    if (!text.empty() && !text.ends_with('\n')) text += '\n';
    text += appended;
    if (auto written = write_file(cases_path, text); !written) {
      return std::unexpected(written.error());
    }
  }

  std::optional<std::pair<std::size_t, std::size_t>> coverage;
  if (const auto text = read_file(coverage_path); text.has_value()) {
    try {
      const Json doc = parse(*text);
      if (const Json* reached = doc.find("reached"); reached != nullptr && reached->is_array()) {
        std::size_t lines = 0;
        if (const auto source = read_file(project.source_file); source.has_value()) {
          for (const std::string_view line : split_lines(*source)) {
            std::string_view trimmed = line;
            while (!trimmed.empty() &&
                   std::isspace(static_cast<unsigned char>(trimmed.front()))) {
              trimmed.remove_prefix(1);
            }
            if (!trimmed.empty() && !trimmed.starts_with('#')) ++lines;
          }
        }
        coverage = std::pair{reached->as_array().size(), lines};
      }
    } catch (const JsonError&) {
      coverage = std::nullopt;
    }
  }

  Json::Object from_tests;
  Json::Array command_json;
  for (const std::string& part : command) command_json.push_back(Json::string(part));
  from_tests.emplace_back("command", Json::array(std::move(command_json)));
  from_tests.emplace_back("calls", Json::integer(static_cast<std::int64_t>(calls)));
  from_tests.emplace_back("cases", Json::integer(static_cast<std::int64_t>(added)));
  from_tests.emplace_back("at", Json::string(now_iso8601()));
  const Json recorded_from = Json::object({
      {"schema", Json::string("cpp-anything/recorded-from@1")},
      {"source_hash", Json::string(hash64(read_file(project.source_file).value_or(std::string())))},
      {"from_tests", Json::object(std::move(from_tests))},
      {"coverage", coverage.has_value()
                       ? Json::object({{"reached", Json::integer(static_cast<std::int64_t>(coverage->first))},
                                       {"lines", Json::integer(static_cast<std::int64_t>(coverage->second))}})
                       : Json::null()},
  });
  if (auto written = write_file(project.dir / kMetaDir / "recorded-from.json",
                                dump(recorded_from, true));
      !written) {
    return std::unexpected(written.error());
  }
  return FromTests{command, calls, added, coverage, run.ok,
                   run.ok ? std::string() : run.failure()};
}

Result<std::size_t> record(const Project& project, std::size_t cases, std::uint64_t seed,
                           const std::optional<std::filesystem::path>& inputs) {
  const std::filesystem::path out = project.dir / "cases.jsonl";
  std::vector<std::string> args = {"record",
                                   "--path",
                                   project.source_file.string(),
                                   "--module",
                                   project.module,
                                   "--out",
                                   out.string(),
                                   "--cases",
                                   std::to_string(cases),
                                   "--seed",
                                   std::to_string(seed)};
  if (project.package_root.has_value()) {
    args.push_back("--package-root");
    args.push_back(project.package_root->string());
  }
  if (inputs.has_value()) {
    args.push_back("--inputs");
    args.push_back(inputs->string());
  }
  auto output = probe(args, project.dir);
  if (!output) return std::unexpected(output.error());
  try {
    const Json doc = parse(trim(*output));
    if (const Json* count = doc.find("cases"); count != nullptr && count->is_integer()) {
      return static_cast<std::size_t>(count->as_int());
    }
    return 0;
  } catch (const JsonError& failure) {
    return std::unexpected(std::string("probe returned invalid JSON: ") + failure.what());
  }
}

Result<Json> analyze(const std::filesystem::path& root) {
  // Absolute, and no cwd override: a relative root plus a cwd would make the
  // probe walk `<root>/<root>`.
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::canonical(root, error);
  if (error) {
    return std::unexpected(root.string() + ": " + error.message());
  }
  auto output = probe({"analyze", "--root", absolute.string()});
  if (!output) return std::unexpected(output.error());
  try {
    return parse(trim(*output));
  } catch (const JsonError& failure) {
    return std::unexpected(std::string("probe returned invalid JSON: ") + failure.what());
  }
}

}  // namespace cppan::python
