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
