#include "cppan/python.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "cppan/probe_py.hpp"

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
