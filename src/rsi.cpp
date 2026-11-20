// `rsi`: cpp-anything checking itself with its own loop. Every step either proves a
// shipped artefact still works or says exactly what drifted.
module;
#if !defined(_WIN32)
#include <unistd.h>
#endif

module cppan.rsi;
import cppan.types;
import cppan.build;
import cppan.args;
import cppan.codegen;
import cppan.diff;
import cppan.docs;
import cppan.json;
import cppan.matrix;
import cppan.project;
import cppan.python;
import cppan.util;
import std;

namespace cppan::rsi {
namespace {

struct Step {
  std::string name;
  bool ok = false;
  std::string detail;
};

struct GoldenCase {
  std::string_view name;
  std::vector<std::string> argv;
};

const GoldenCase kGolden[] = {
    GoldenCase{"matrix-features-gcc-13", {"matrix", "features", "--toolchain", "gcc", "--version", "13", "--std", "c++23"}},
    GoldenCase{"matrix-features-clang-20-json", {"matrix", "features", "--toolchain", "clang", "--version", "20", "--std", "c++23", "--json"}},
    GoldenCase{"matrix-features-msvc-19.44", {"matrix", "features", "--toolchain", "msvc", "--version", "19.44", "--std", "c++23"}},
    GoldenCase{"matrix-explain-expected", {"matrix", "explain", "__cpp_lib_expected"}},
    GoldenCase{"matrix-explain-print-json", {"matrix", "explain", "std::print", "--json"}},
    GoldenCase{"matrix-profile-gcc-13", {"matrix", "profile", "--toolchain", "gcc", "--version", "13", "--std", "c++23"}},
    GoldenCase{"matrix-profile-msvc-19.44", {"matrix", "profile", "--toolchain", "msvc", "--version", "19.44", "--std", "c++23"}},
    GoldenCase{"analyze-fixture-json", {"analyze", "tests/fixtures/python/textkit", "--json"}},
    GoldenCase{"matrix-docs-check", {"matrix", "docs", "--check"}},};

Result<std::string> run_cli(const std::filesystem::path& root,
                            const std::vector<std::string>& argv) {
  const Run run = run_command(cppan::current_executable().string(), argv, root);
  if (run.ok) return run.out;
  return run.out + run.err;
}

std::string normalize(std::string text, const std::filesystem::path& root) {
  const std::string root_text = root.string();
  std::size_t at = 0;
  while ((at = text.find(root_text, at)) != std::string::npos) {
    text.replace(at, root_text.size(), "<repo>");
    at += 7;
  }
  std::error_code error;
  const auto canonical = std::filesystem::canonical(root, error);
  if (!error) {
    const std::string canonical_text = canonical.string();
    if (canonical_text != root_text) {
      at = 0;
      while ((at = text.find(canonical_text, at)) != std::string::npos) {
        text.replace(at, canonical_text.size(), "<repo>");
        at += 7;
      }
    }
  }
  const std::string temp = std::filesystem::temp_directory_path(error).string();
  if (!error && !temp.empty()) {
    at = 0;
    while ((at = text.find(temp, at)) != std::string::npos) {
      text.replace(at, temp.size(), "<tmp>");
      at += 5;
    }
  }
  std::string out;
  for (const std::string_view line : split_lines(text)) {
    if (!out.empty()) out += '\n';
    if (line.starts_with("#define CPPAN_MATRIX_GENERATED_AT")) {
      out += "#define CPPAN_MATRIX_GENERATED_AT \"<date>\"";
    } else {
      std::string_view trimmed = line;
      while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.remove_suffix(1);
      }
      out += trimmed;
    }
  }
  return out;
}

Result<void> copy_dir(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(from, error);
  if (error) return std::unexpected(from.string() + ": " + error.message());
  for (const auto& entry : iterator) {
    const std::string name = entry.path().filename().string();
    if (name == "build" || name == "__pycache__" || name == ".git") continue;
    const std::filesystem::path target = to / entry.path().filename();
    if (entry.is_directory(error)) {
      std::filesystem::create_directories(target, error);
      if (error) return std::unexpected(target.string() + ": " + error.message());
      if (auto copied = copy_dir(entry.path(), target); !copied) return copied;
    } else {
      std::filesystem::copy_file(entry.path(), target,
                                 std::filesystem::copy_options::overwrite_existing, error);
      if (error) return std::unexpected(entry.path().string() + ": " + error.message());
    }
  }
  return {};
}

/// Example scaffolds were generated in another checkout and store its absolute
/// paths. Point them at this checkout before building, or a copied example links
/// a directory that does not exist here.
void relocate_example(const std::filesystem::path& scratch, const std::filesystem::path& root);

void relocate_paths(const std::filesystem::path& file, const std::filesystem::path& root) {
  const auto text = read_file(file);
  if (!text.has_value()) return;
  std::string out = *text;
  for (const std::string_view marker : {"/examples/", "/tests/"}) {
    std::size_t at = 0;
    while ((at = out.find(marker, at)) != std::string::npos) {
      std::size_t start = at;
      while (start > 0) {
        const char ch = out[start - 1];
        if (ch == '"' || ch == '\'' || ch == '(' || ch == ' ' || ch == '=' || ch == ',' ||
            ch == ':' || ch == '[') {
          break;
        }
        --start;
      }
      if (start == 0 || out[start] != '/') {
        at += marker.size();
        continue;
      }
      out.replace(start, at - start, root.string());
      at = start + root.string().size() + marker.size();
    }
  }
  (void)write_file(file, out);
}

void relocate_example(const std::filesystem::path& scratch, const std::filesystem::path& root) {
  relocate_paths(scratch / "cpp" / "app" / "routes.json", root);
  relocate_paths(scratch / "cpp" / "app" / "units.cmake", root);
  relocate_paths(scratch / "cpp" / "lib" / "units.cmake", root);
  relocate_paths(scratch / "cpp" / "app" / "src" / "fallback.cpp", root);
}

std::filesystem::path golden_path(const std::filesystem::path& root) {
  return root / "tests" / "golden" / "cli.jsonl";
}

Step golden_step(const std::filesystem::path& root) {
  const std::filesystem::path path = golden_path(root);
  if (!std::filesystem::exists(path)) {
    return Step{"cli goldens", false,
                "tests/golden/cli.jsonl is missing: run `cpp-anything rsi record`"};
  }
  const auto text = read_file(path);
  if (!text.has_value()) return Step{"cli goldens", false, path.string() + ": cannot read"};
  std::vector<Json> stored;
  for (const std::string_view line : split_lines(*text)) {
    bool empty = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
    }
    if (empty) continue;
    try {
      stored.push_back(parse(line));
    } catch (const JsonError& error) {
      return Step{"cli goldens", false, path.string() + ": " + error.what()};
    }
  }
  std::vector<std::string> drifted;
  for (const Json& entry : stored) {
    const Json* name = entry.find("name");
    const std::string_view label =
        name != nullptr && name->is_string() ? std::string_view(name->as_string()) : "?";
    std::vector<std::string> argv;
    if (const Json* list = entry.find("argv"); list != nullptr && list->is_array()) {
      for (const Json& item : list->as_array()) {
        if (item.is_string()) argv.push_back(item.as_string());
      }
    }
    if (argv.empty()) continue;
    const Json* expected_field = entry.find("stdout");
    const std::string expected =
        expected_field != nullptr && expected_field->is_string() ? expected_field->as_string() : "";
    auto output = run_cli(root, argv);
    if (!output) return Step{"cli goldens", false, output.error()};
    if (normalize(*output, root) != expected) drifted.emplace_back(label);
  }
  std::string detail;
  if (drifted.empty()) {
    detail = std::format("{} commands print what was recorded", stored.size());
  } else {
    detail = "output changed for ";
    for (std::size_t index = 0; index < drifted.size(); ++index) {
      if (index > 0) detail += ", ";
      detail += drifted[index];
    }
    detail += ": run `cpp-anything rsi record` if the change is intended";
  }
  return Step{"cli goldens", drifted.empty(), detail};
}

Step self_port_step(const std::filesystem::path& root) {
  const std::filesystem::path example = root / "examples" / "textkit-cpp";
  if (!std::filesystem::exists(example)) {
    return Step{"self port", false, short_path(example) + " is missing"};
  }
  auto project = Project::load(example);
  if (!project) return Step{"self port", false, project.error()};
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / ("cpp-anything-rsi-" + std::to_string(getpid()));
  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  std::filesystem::create_directories(scratch, error);
  if (error) return Step{"self port", false, scratch.string() + ": " + error.message()};

  Project rebuilt = *project;
  rebuilt.dir = scratch;
  auto written = codegen::write_project(rebuilt, true);
  if (!written) return Step{"self port", false, written.error()};
  for (const std::string_view relative : {"src/port.cpp", "cases.jsonl"}) {
    std::filesystem::copy_file(example / relative, scratch / relative,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      return Step{"self port", false, (example / relative).string() + ": " + error.message()};
    }
  }
  auto outcome = build::build_project(rebuilt, true, std::nullopt, 4);
  if (!outcome) return Step{"self port", false, outcome.error()};
  diff::Options options;
  options.max_report = 3;
  auto report = diff::run(rebuilt, outcome->driver, options);
  if (!report) return Step{"self port", false, report.error()};
  if (report->passed()) {
    std::filesystem::remove_all(scratch, error);
    return Step{"self port", true,
                std::format("generated {}, built it ({} warnings) and replayed {}/{} cases of "
                            "the shipped port",
                            project->module, outcome->warnings, report->matched, report->total)};
  }
  return Step{"self port", false,
              std::format("{} of {} cases mismatched; output kept in {}", report->mismatches.size(),
                          report->total, scratch.string())};
}

Step app_step(const std::filesystem::path& root) {
  const std::filesystem::path example = root / "examples" / "slugify-cli";
  if (!std::filesystem::exists(example / "cpp" / "app-cases")) {
    return Step{"self app", false, "examples/slugify-cli has no recorded app cases"};
  }
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() /
      ("cpp-anything-rsi-app-" + std::to_string(getpid()));
  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  std::filesystem::create_directories(scratch, error);
  if (error) return Step{"self app", false, scratch.string() + ": " + error.message()};
  if (auto copied = copy_dir(example, scratch); !copied) {
    return Step{"self app", false, copied.error()};
  }
  relocate_example(scratch, root);
  auto front = run_cli(scratch, {"app", "front-door", "."});
  if (!front) return Step{"self app", false, front.error()};
  if (front->find("✗") != std::string::npos || front->find("passed") == std::string::npos) {
    return Step{"self app", false,
                "app front-door did not replay: " + truncate_text(*front, 200)};
  }
  auto lift = run_cli(scratch, {"app", "lift", "."});
  if (!lift) return Step{"self app", false, lift.error()};
  const bool ok = lift->find("standalone") != std::string::npos &&
                  lift->find("0 fallback") != std::string::npos;
  if (ok) {
    std::filesystem::remove_all(scratch, error);
    return Step{"self app", true,
                "recorded the CLI, rebuilt the front door and lifted it with 0 fallback"};
  }
  return Step{"self app", false, "app lift did not reach standalone: " + truncate_text(*lift, 200)};
}

Step plugins_step(const std::filesystem::path& root) {
  std::vector<std::string> checked;
  std::vector<std::string> failures;
  for (const std::string_view language : {"ruby", "go", "java", "ts", "rust"}) {
    const std::filesystem::path cpp =
        root / "tests" / "fixtures" / std::string(language) / "cpp";
    std::error_code error;
    std::filesystem::directory_iterator iterator(cpp, error);
    if (error) continue;
    for (const auto& entry : iterator) {
      const std::filesystem::path project = entry.path();
      if (!std::filesystem::exists(project / "cases.jsonl") ||
          !std::filesystem::exists(project / ".cppan")) {
        continue;
      }
      auto gate = run_cli(root, {"gate", project.string()});
      if (!gate) continue;
      const std::string name = project.filename().string();
      if (gate->find("PASS") != std::string::npos) {
        checked.push_back(std::string(language) + "/" + name);
      } else {
        failures.push_back(std::string(language) + "/" + name + ": " + truncate_text(*gate, 160));
      }
    }
  }
  if (failures.empty() && !checked.empty()) {
    std::string joined;
    for (const std::string& name : checked) {
      if (!joined.empty()) joined += ", ";
      joined += name;
    }
    return Step{"self plugins", true,
                std::format("{} plugin fixture(s) pass gate ({})", checked.size(), joined)};
  }
  if (checked.empty()) {
    return Step{"self plugins", false,
                "no plugin fixture has a recorded project under tests/fixtures"};
  }
  std::string joined;
  for (const std::string& failure : failures) {
    if (!joined.empty()) joined += "; ";
    joined += failure;
  }
  return Step{"self plugins", false, joined};
}

Step library_step(const std::filesystem::path& root) {
  const std::filesystem::path example = root / "examples" / "c-text";
  if (!std::filesystem::exists(example / "cpp" / "textkit-c")) {
    return Step{"self library", false, "examples/c-text has no bridged unit"};
  }
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() /
      ("cpp-anything-rsi-lib-" + std::to_string(getpid()));
  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  std::filesystem::create_directories(scratch, error);
  if (error) return Step{"self library", false, scratch.string() + ": " + error.message()};
  if (auto copied = copy_dir(example, scratch); !copied) {
    return Step{"self library", false, copied.error()};
  }
  relocate_example(scratch, root);
  auto output = run_cli(scratch, {"app", "front-door", "--library", "."});
  if (!output) return Step{"self library", false, output.error()};
  const std::string text =
      read_file(scratch / "cpp" / "lib" / "include/cppan/api.hpp").value_or(std::string());
  const bool ok = text.find("namespace textkit") != std::string::npos &&
                  output->find("built") != std::string::npos;
  if (ok) {
    return Step{"self library", true,
                "app front-door --library re-exported the C unit and built the library"};
  }
  return Step{"self library", false,
              truncate_text(*output, 200) + " (header: " +
                  (text.empty() ? "missing" : "no textkit namespace") + ")"};
}

Step http_native_step(const std::filesystem::path& root) {
  const std::filesystem::path fixture = root / "tests" / "fixtures" / "http";
  if (!std::filesystem::exists(fixture / "cpp" / "app-cases")) {
    return Step{"self http routes", false,
                "tests/fixtures/http has no recorded server cases"};
  }
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() /
      ("cpp-anything-rsi-http-" + std::to_string(getpid()));
  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  std::filesystem::create_directories(scratch, error);
  if (error) return Step{"self http routes", false, scratch.string() + ": " + error.message()};
  if (auto copied = copy_dir(fixture, scratch); !copied) {
    return Step{"self http routes", false, copied.error()};
  }
  std::filesystem::remove_all(scratch / "cpp" / "app" / "build", error);
  relocate_example(scratch, root);
  auto front = run_cli(scratch, {"app", "front-door", "."});
  if (!front) return Step{"self http routes", false, front.error()};
  if (front->find("✗") != std::string::npos ||
      front->find("cases passed") == std::string::npos) {
    return Step{"self http routes", false,
                "the routed front door did not replay: " + truncate_text(*front, 200)};
  }
  auto lift = run_cli(scratch, {"app", "lift", "."});
  if (!lift) return Step{"self http routes", false, lift.error()};
  const bool ok = lift->find("answered natively") != std::string::npos &&
                  lift->find("still go to the original") == std::string::npos;
  if (ok) {
    return Step{"self http routes", true,
                "native routes answered every recorded request and lift dropped the original"};
  }
  return Step{"self http routes", false,
              "lift did not reach native-only: " + truncate_text(*lift, 200)};
}

Step cpp_step(const std::filesystem::path& root) {
  const std::filesystem::path project =
      root / "examples" / "legacy-cpp" / "cpp" / "legacy-cpp";
  if (!std::filesystem::exists(project / "cases.jsonl")) {
    return Step{"self cpp", false, "examples/legacy-cpp has no recorded cases"};
  }
  auto build = run_cli(root, {"build", project.string()});
  if (!build) return Step{"self cpp", false, build.error()};
  auto gate = run_cli(root, {"gate", project.string()});
  if (!gate) return Step{"self cpp", false, gate.error()};
  const bool ok = gate->find("PASS") != std::string::npos &&
                  gate->find("modern") != std::string::npos;
  if (!ok) return Step{"self cpp", false, truncate_text(*build, 80) + truncate_text(*gate, 200)};
  std::string diff_line;
  for (const std::string_view line : split_lines(*gate)) {
    if (line.find("diff ") != std::string_view::npos) {
      std::string_view trimmed = line;
      while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.remove_prefix(1);
      }
      diff_line = trimmed;
      break;
    }
  }
  return Step{"self cpp", true, "modernized C-style C++ passes gate (" + diff_line + ")"};
}

Step c_step(const std::filesystem::path& root) {
  const std::filesystem::path project =
      root / "examples" / "c-text" / "cpp" / "textkit-c";
  if (!std::filesystem::exists(project / "cases.jsonl")) {
    return Step{"self c", false, "examples/c-text has no recorded cases"};
  }
  auto build = run_cli(root, {"build", project.string()});
  if (!build) return Step{"self c", false, build.error()};
  auto diff = run_cli(root, {"diff", project.string()});
  if (!diff) return Step{"self c", false, diff.error()};
  const bool ok = diff->find("match") != std::string::npos &&
                  diff->find("MISMATCH") == std::string::npos;
  if (ok) {
    const auto lines = split_lines(*diff);
    const std::string first = lines.empty() ? std::string() : std::string(lines.front());
    return Step{"self c", true, "rebuilt the C unit's port: " + truncate_text(first, 120)};
  }
  return Step{"self c", false, truncate_text(*build, 80) + truncate_text(*diff, 160)};
}

Step http_step(const std::filesystem::path& root) {
  const std::filesystem::path example = root / "examples" / "http-app";
  if (!std::filesystem::exists(example / "cpp" / "app-cases")) {
    return Step{"self http", false, "examples/http-app has no recorded cases"};
  }
  auto output = run_cli(example, {"app", "replay", "."});
  if (!output) return Step{"self http", false, output.error()};
  const bool ok = output->find("cases passed") != std::string::npos &&
                  output->find("✗") == std::string::npos;
  return Step{"self http", ok,
              ok ? "started the server, sent every recorded request and matched the responses"
                 : truncate_text(*output, 200)};
}

Result<int> record(const std::filesystem::path& root) {
  std::vector<std::string> lines;
  for (const GoldenCase& golden : kGolden) {
    auto output = run_cli(root, golden.argv);
    if (!output) return std::unexpected(output.error());
    Json::Array argv;
    for (const std::string& arg : golden.argv) argv.push_back(Json::string(arg));
    const Json entry = Json::object({
        {"name", Json::string(std::string(golden.name))},
        {"argv", Json::array(std::move(argv))},
        {"stdout", Json::string(normalize(*output, root))},
    });
    lines.push_back(dump(entry));
  }
  std::string text;
  for (const std::string& line : lines) {
    text += line;
    text += '\n';
  }
  const std::filesystem::path path = golden_path(root);
  if (auto written = write_file(path, text); !written) return std::unexpected(written.error());
  std::print("recorded {} golden transcripts in {}\n", lines.size(), short_path(path));
  return 0;
}

}  // namespace

Result<int> command(const cli::Parsed& args);
Result<int> command_impl(const cli::Parsed& args, bool json_output) {
  const std::string sub = args.positional.empty() ? "check" : args.positional[0];
  const auto root = repo_root();
  if (!root.has_value()) {
    return std::unexpected(
        "rsi runs inside the cpp-anything checkout: no data/matrix.json found above the current "
        "directory");
  }
  if (sub == "check") {
    std::vector<Step> steps;
    const Matrix& matrix = Matrix::get();
    std::size_t sourced = 0;
    for (const Source& source : matrix.sources) {
      if (source.revision.has_value()) ++sourced;
    }
    steps.push_back(Step{"matrix data",
                         matrix.features.size() >= 600 && sourced == matrix.sources.size(),
                         std::format("{} features, {} standard(s), {}/{} source pages with a "
                                     "revision id",
                                     matrix.features.size(), matrix.standards.size(), sourced,
                                     matrix.sources.size())});
    {
      auto drifted = docs::check(*root / "docs/guides");
      if (!drifted) return std::unexpected(drifted.error());
      std::string detail;
      if (drifted->empty()) {
        detail = std::format("{} guides match data/matrix.json", docs::all().size());
      } else {
        detail = "run `cpp-anything matrix docs`; drifted: ";
        for (std::size_t index = 0; index < drifted->size(); ++index) {
          if (index > 0) detail += ", ";
          detail += (*drifted)[index];
        }
      }
      steps.push_back(Step{"guides", drifted->empty(), detail});
    }
    steps.push_back(golden_step(*root));
    steps.push_back(self_port_step(*root));
    steps.push_back(app_step(*root));
    steps.push_back(http_step(*root));
    steps.push_back(c_step(*root));
    steps.push_back(cpp_step(*root));
    steps.push_back(plugins_step(*root));
    steps.push_back(library_step(*root));
    steps.push_back(http_native_step(*root));

    bool passed = true;
    for (const Step& step : steps) {
      if (!step.ok) passed = false;
    }
    if (json_output) {
      Json::Array entries;
      for (const Step& step : steps) {
        entries.push_back(Json::object({
            {"name", Json::string(step.name)},
            {"ok", Json::boolean(step.ok)},
            {"detail", Json::string(step.detail)},
        }));
      }
      std::print("{}\n", dump(Json::object({
                                 {"schema", Json::string("cpp-anything/rsi@1")},
                                 {"passed", Json::boolean(passed)},
                                 {"steps", Json::array(std::move(entries))},
                             }),
                              true));
    } else {
      std::print("cpp-anything rsi: {}\n\n", passed ? "PASS" : "FAIL");
      std::size_t width = 10;
      for (const Step& step : steps) width = std::max(width, step.name.size());
      for (const Step& step : steps) {
        std::print("  {}  {}  {}\n", step.ok ? "✓" : "✗", pad_right(step.name, width + 2),
                   step.detail);
      }
    }
    return passed ? 0 : 1;
  }
  if (sub == "record") return record(*root);
  return std::unexpected("unknown rsi command " + quote_debug(sub) +
                         "; try `rsi check` or `rsi record`");
}

}  // namespace cppan::rsi

namespace cppan::rsi {

Result<int> command(const cli::Parsed& args) { return command_impl(args, args.flag("json")); }

}  // namespace cppan::rsi
