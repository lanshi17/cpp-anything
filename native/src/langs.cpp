#include "cppan/langs.hpp"

#include "cppan/corpora.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>


namespace cppan::langs {
namespace {

std::string relative_text(const std::filesystem::path& path, const std::filesystem::path& repo) {
  std::error_code error;
  const auto relative = std::filesystem::relative(path, repo, error);
  return error ? path.string() : relative.string();
}

std::size_t count_files(const std::filesystem::path& repo,
                        const std::vector<std::string_view>& wanted) {
  std::size_t count = 0;
  std::vector<std::pair<std::filesystem::path, std::size_t>> stack = {{repo, 0}};
  while (!stack.empty()) {
    auto [directory, depth] = stack.back();
    stack.pop_back();
    if (depth > 3) continue;
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) continue;
    for (const auto& entry : iterator) {
      const std::string name = entry.path().filename().string();
      if (entry.is_directory(error)) {
        if (name != "build" && name != "node_modules" && name != "target" &&
            name != "vendor" && name != ".git" && name != "cpp" && name != "dist") {
          stack.emplace_back(entry.path(), depth + 1);
        }
        continue;
      }
      const std::string extension = entry.path().extension().string().substr(
          entry.path().extension().string().empty() ? 0 : 1);
      for (const std::string_view candidate : wanted) {
        if (extension == candidate) ++count;
      }
    }
  }
  return count;
}

Json module_json(Lang lang, const std::filesystem::path& path, const std::filesystem::path& repo,
                 const Analysis& analysis) {
  const std::string relative = relative_text(path, repo);
  Json::Array functions;
  for (const Found& found : analysis.functions) {
    functions.push_back(Json::object({
        {"name", Json::string(found.name)},
        {"line", Json::integer(found.line)},
        {"params", Json::integer(static_cast<std::int64_t>(found.params.size()))},
        {"returns", Json::string(found.returns)},
        {"doc", Json::string(found.doc)},
    }));
  }
  Json::Array imports;
  for (const std::string& import : analysis.imports) imports.push_back(Json::string(import));
  Json::Array risks;
  for (const std::string_view risk : language_risks(lang)) risks.push_back(Json::string(std::string(risk)));
  return Json::object({
      {"module", Json::string(module_name(std::filesystem::path(relative)))},
      {"unit", Json::string(relative)},
      {"file", Json::string(relative)},
      {"lines", Json::integer(analysis.lines)},
      {"functions", Json::array(std::move(functions))},
      {"imports", Json::array(std::move(imports))},
      {"risks", Json::array(std::move(risks))},
      {"language", Json::string(std::string(id(lang)))},
  });
}

}  // namespace

std::string_view id(Lang lang) {
  switch (lang) {
    case Lang::Ts: return "ts";
    case Lang::Js: return "js";
    case Lang::Go: return "go";
    case Lang::Ruby: return "ruby";
    case Lang::Java: return "java";
    case Lang::Php: return "php";
    case Lang::Csharp: return "csharp";
  }
  return "ts";
}

std::string_view label(Lang lang) {
  switch (lang) {
    case Lang::Ts: return "TypeScript";
    case Lang::Js: return "JavaScript";
    case Lang::Go: return "Go";
    case Lang::Ruby: return "Ruby";
    case Lang::Java: return "Java";
    case Lang::Php: return "PHP";
    case Lang::Csharp: return "C#";
  }
  return "TypeScript";
}

bool whole_app_only(Lang lang) { return lang == Lang::Php || lang == Lang::Csharp; }

std::vector<std::string_view> extensions(Lang lang) {
  switch (lang) {
    case Lang::Ts: return {"ts", "mts", "cts"};
    case Lang::Js: return {"js", "mjs", "cjs", "jsx"};
    case Lang::Go: return {"go"};
    case Lang::Ruby: return {"rb"};
    case Lang::Java: return {"java"};
    case Lang::Php: return {"php"};
    case Lang::Csharp: return {"cs"};
  }
  return {};
}

bool unit_is_directory(Lang lang) { return lang == Lang::Go; }

bool is_ts_family(Lang lang) { return lang == Lang::Ts || lang == Lang::Js; }

std::optional<Lang> parse(std::string_view name) {
  if (name == "ts" || name == "typescript") return Lang::Ts;
  if (name == "js" || name == "javascript" || name == "node") return Lang::Js;
  if (name == "go" || name == "golang") return Lang::Go;
  if (name == "rb" || name == "ruby") return Lang::Ruby;
  if (name == "java") return Lang::Java;
  if (name == "php") return Lang::Php;
  if (name == "cs" || name == "c#" || name == "csharp" || name == "dotnet") return Lang::Csharp;
  return std::nullopt;
}

std::optional<Lang> detect(const std::filesystem::path& repo) {
  const auto has = [&](std::string_view name) {
    return std::filesystem::is_regular_file(repo / std::string(name));
  };
  std::optional<Lang> manifest;
  if (has("go.mod")) {
    manifest = Lang::Go;
  } else if (has("pom.xml") || has("build.gradle") || has("build.gradle.kts")) {
    manifest = Lang::Java;
  } else if (has("composer.json")) {
    manifest = Lang::Php;
  } else if (has("Directory.Build.props")) {
    manifest = Lang::Csharp;
  } else if (has("Gemfile")) {
    manifest = Lang::Ruby;
  } else if (has("package.json")) {
    manifest = has("tsconfig.json") ? Lang::Ts : Lang::Js;
  }
  if (manifest.has_value()) {
    // A manifest decides unless the repository is mostly Python, where a
    // package.json is usually the frontend or a dev tool.
    const std::size_t py = count_files(repo, {"py"});
    const std::size_t own = count_files(repo, extensions(*manifest));
    return py > own ? std::nullopt : manifest;
  }

  std::map<Lang, std::size_t> counts;
  std::vector<std::pair<std::filesystem::path, std::size_t>> stack = {{repo, 0}};
  while (!stack.empty()) {
    auto [directory, depth] = stack.back();
    stack.pop_back();
    if (depth > 3) continue;
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) continue;
    for (const auto& entry : iterator) {
      const std::string name = entry.path().filename().string();
      if (entry.is_directory(error)) {
        if (name != "build" && name != "node_modules" && name != "target" &&
            name != "vendor" && name != ".git" && name != "cpp" && name != "dist") {
          stack.emplace_back(entry.path(), depth + 1);
        }
        continue;
      }
      const std::string extension = entry.path().extension().string();
      const std::string_view bare = extension.empty() ? std::string_view() : std::string_view(extension).substr(1);
      for (const Lang lang : {Lang::Ts, Lang::Js, Lang::Go, Lang::Ruby, Lang::Java}) {
        for (const std::string_view candidate : extensions(lang)) {
          if (bare == candidate) ++counts[lang];
        }
      }
    }
  }
  std::optional<Lang> best;
  std::size_t best_count = 0;
  for (const auto& [lang, count] : counts) {
    // `max_by_key((count, Reverse(id)))`: highest count, then the smallest id.
    if (!best.has_value() || count > best_count ||
        (count == best_count && std::string_view(id(lang)) < std::string_view(id(*best)))) {
      best = lang;
      best_count = count;
    }
  }
  return best_count > 0 ? best : std::nullopt;
}

std::optional<Lang> infer(const std::filesystem::path& repo, std::string_view unit) {
  const std::filesystem::path path =
      std::filesystem::path(unit).is_absolute() ? std::filesystem::path(unit) : repo / unit;
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    std::filesystem::directory_iterator iterator(path, error);
    if (error) return std::nullopt;
    for (const auto& entry : iterator) {
      if (entry.path().extension() == ".go") return Lang::Go;
    }
    return std::nullopt;
  }
  const std::string extension = path.extension().string();
  const std::string_view bare = extension.empty() ? std::string_view() : std::string_view(extension).substr(1);
  for (const Lang lang : {Lang::Ts, Lang::Js, Lang::Go, Lang::Ruby, Lang::Java, Lang::Php,
                          Lang::Csharp}) {
    for (const std::string_view candidate : extensions(lang)) {
      if (bare == candidate) return lang;
    }
  }
  return std::nullopt;
}

Function Analysis::to_function(const Found& found) const {
  Function function;
  function.name = found.name;
  function.line = found.line;
  for (const FoundParam& param : found.params) {
    Param entry;
    entry.name = param.name;
    entry.annotation = param.ty;
    entry.ty = Ty::parse(param.ty);
    entry.default_value = param.default_value;
    entry.has_default = param.has_default;
    function.params.push_back(std::move(entry));
  }
  function.returns = Ty::parse(found.returns);
  function.returns_annotation = found.returns;
  function.doc = found.doc;
  return function;
}

Result<Json> analyze_repo(const std::filesystem::path& repo, Lang lang) {
  std::vector<Json> modules;
  std::vector<std::pair<std::filesystem::path, std::size_t>> stack = {{repo, 0}};
  std::vector<std::filesystem::path> seen_dirs;
  while (!stack.empty()) {
    auto [directory, depth] = stack.back();
    stack.pop_back();
    if (depth > 5) continue;
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) return std::unexpected(directory.string() + ": " + error.message());
    std::vector<std::filesystem::path> files;
    for (const auto& entry : iterator) {
      const std::string name = entry.path().filename().string();
      if (entry.is_directory(error)) {
        if (name == "build" || name == "node_modules" || name == "target" ||
            name == "vendor" || name == ".git" || name == "cpp" || name == "dist" ||
            name == "testdata") {
          continue;
        }
        stack.emplace_back(entry.path(), depth + 1);
        continue;
      }
      const std::string extension = entry.path().extension().string();
      const std::string_view bare = extension.empty() ? std::string_view() : std::string_view(extension).substr(1);
      for (const std::string_view candidate : extensions(lang)) {
        if (bare == candidate) files.push_back(entry.path());
      }
    }
    std::ranges::sort(files);
    if (unit_is_directory(lang)) {
      if (files.empty() ||
          std::ranges::find(seen_dirs, directory) != seen_dirs.end()) {
        continue;
      }
      seen_dirs.push_back(directory);
      auto analysis = go::analyze_package(directory);
      if (analysis.has_value() && !analysis->functions.empty()) {
        modules.push_back(module_json(lang, directory, repo, *analysis));
      }
      continue;
    }
    for (const auto& file : files) {
      auto analysis = analyze_file(file, lang);
      if (!analysis.has_value() || analysis->functions.empty()) continue;
      modules.push_back(module_json(lang, file, repo, *analysis));
    }
  }
  std::stable_sort(modules.begin(), modules.end(), [](const Json& a, const Json& b) {
    const Json* left = a.find("module");
    const Json* right = b.find("module");
    const std::string left_text = left != nullptr && left->is_string() ? left->as_string() : "";
    const std::string right_text = right != nullptr && right->is_string() ? right->as_string() : "";
    return left_text < right_text;
  });
  return Json::object({
      {"language", Json::string(std::string(id(lang)))},
      {"modules", Json::array(std::move(modules))},
  });
}

std::vector<std::string_view> language_risks(Lang lang) {
  switch (lang) {
    case Lang::Ts:
    case Lang::Js:
      return {
          "JavaScript numbers are doubles: integers beyond 2^53 are not int64, and the wire "
          "marks them",
          "Strings are UTF-16: an offset is not a byte offset, and emoji are two units",
          "undefined, null and a missing key are three different things in JavaScript, two in "
          "the wire",
          "Modules with side effects on import make recording observe more than the functions",
      };
    case Lang::Go:
      return {
          "Go ints are 64-bit on this platform but 32-bit elsewhere: the signature decides the "
          "width",
          "Strings are UTF-8 bytes: len() is bytes, runes are code points, and the port must "
          "choose",
          "nil slices and empty slices print the same, marshal differently, and compare against "
          "nil",
          "Named result parameters and deferred writes: the recorder observes the final value",
      };
    case Lang::Ruby:
      return {
          "Ruby integers are unbounded: values beyond int64 cannot cross the wire and are "
          "recorded as errors",
          "Strings are bytes with an encoding: a substring is not a character slice unless told "
          "so",
          "Symbols, ranges and custom objects do not cross the wire; the recorder reports them "
          "as errors",
          "Hash order is insertion order, and the wire preserves object key order",
      };
    case Lang::Java:
      return {
          "Java ints are 32-bit, longs 64-bit, and the wire carries int64: the signature "
          "decides the width",
          "char is UTF-16 code units, String is UTF-16: offsets and lengths are not bytes",
          "null and an empty string are different values, and both cross the wire",
          "Checked exceptions are part of the original's contract; the port mirrors the type "
          "names",
      };
    case Lang::Php:
      return {"PHP is whole-app only in this tool: unit-level ports arrive with a per-unit "
              "harness"};
    case Lang::Csharp:
      return {"C# is whole-app only in this tool: unit-level ports arrive with a per-unit "
              "harness"};
  }
  return {};
}

std::string module_name(const std::filesystem::path& relative) {
  std::filesystem::path stem = relative;
  stem.replace_extension();
  std::vector<std::string> parts;
  for (const auto& part : stem) {
    const std::string text = part.string();
    if (text.empty() || text == ".") continue;
    parts.push_back(text);
  }
  if (!parts.empty() && parts.back() == "index") parts.pop_back();
  if (parts.empty()) return "unit";
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out += '.';
    out += part;
  }
  return out;
}

Result<Analysis> analyze_file(const std::filesystem::path& file, Lang lang) {
  switch (lang) {
    case Lang::Ruby: return ruby::analyze(file);
    case Lang::Go: {
      if (!file.has_parent_path()) {
        return std::unexpected(file.string() + ": no parent directory");
      }
      return go::analyze_package(file.parent_path());
    }
    case Lang::Java: return java::analyze(file);
    case Lang::Ts:
    case Lang::Js: return ts::analyze(file, lang);
    case Lang::Php: return php::analyze(file);
    case Lang::Csharp: return csharp::analyze(file);
  }
  return std::unexpected("unknown language");
}

Result<Unit> inventory(const std::filesystem::path& repo, std::string_view unit_name,
                       Lang lang) {
  std::error_code error;
  const auto repo_abs = std::filesystem::canonical(repo, error);
  if (error) return std::unexpected(repo.string() + ": " + error.message());
  const std::filesystem::path raw =
      std::filesystem::path(unit_name).is_absolute() ? std::filesystem::path(unit_name)
                                                     : repo_abs / unit_name;
  const auto path = std::filesystem::canonical(raw, error);
  if (error) return std::unexpected(raw.string() + ": " + error.message());

  std::filesystem::path source_file;
  Analysis analysis;
  if (unit_is_directory(lang)) {
    if (!std::filesystem::is_directory(path)) {
      return std::unexpected(path.string() + ": a Go unit is a package directory, not a file");
    }
    source_file = path;
    auto result = go::analyze_package(path);
    if (!result) return std::unexpected(result.error());
    analysis = std::move(*result);
  } else {
    if (!std::filesystem::is_regular_file(path)) {
      return std::unexpected(path.string() + ": not a file");
    }
    source_file = path;
    auto result = analyze_file(path, lang);
    if (!result) return std::unexpected(result.error());
    analysis = std::move(*result);
  }
  if (analysis.functions.empty()) {
    return std::unexpected(std::format("{} has no functions with a supported signature ({} "
                                       "skipped)",
                                       path.string(), analysis.skipped.size()));
  }
  Unit out;
  out.unit = unit_is_directory(lang) ? path.filename().string() : path.stem().string();
  if (out.unit.empty()) out.unit = "unit";
  std::error_code rel_error;
  const auto relative = std::filesystem::relative(path, repo_abs, rel_error);
  out.module = module_name(rel_error ? path : relative);
  out.file = source_file;
  out.package_root = repo_abs;
  out.lines = analysis.lines;
  for (const Found& found : analysis.functions) out.functions.push_back(analysis.to_function(found));
  out.skipped = analysis.skipped;
  for (const std::string_view risk : language_risks(lang)) out.risks.emplace_back(risk);
  out.imports = analysis.imports;
  return out;
}

std::string function_of(std::string_view id) {
  const std::size_t colon = id.rfind(':');
  return colon == std::string_view::npos ? std::string(id) : std::string(id.substr(0, colon));
}

std::string cases_json(const std::vector<Json>& calls) {
  std::string out;
  for (std::size_t index = 0; index < calls.size(); ++index) {
    if (index > 0) out += '\n';
    out += dump(calls[index]);
  }
  return out;
}

Result<std::size_t> write_cases(const Project& project, const std::vector<Json>& calls,
                                std::string_view output) {
  std::map<std::string, Json> by_id;
  for (const Json& call : calls) {
    const Json* id = call.find("id");
    if (id != nullptr && id->is_string()) by_id[id->as_string()] = call;
  }
  std::string text;
  std::size_t count = 0;
  std::size_t number = 0;
  for (const std::string_view line : split_lines(output)) {
    ++number;
    bool empty = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
    }
    if (empty) continue;
    Json doc;
    try {
      doc = cppan::parse(line);
    } catch (const JsonError& error) {
      return std::unexpected("recording line " + std::to_string(number) + ": " + error.what() +
                             ": " + std::string(line));
    }
    const Json* id_field = doc.find("id");
    if (id_field == nullptr || !id_field->is_string()) {
      return std::unexpected("recording line " + std::to_string(number) + ": no id");
    }
    const std::string id = id_field->as_string();
    const auto found = by_id.find(id);
    const Json* call = found != by_id.end() ? &found->second : nullptr;
    std::string function;
    if (call != nullptr) {
      if (const Json* fn = call->find("fn"); fn != nullptr && fn->is_string()) {
        function = fn->as_string();
      }
    }
    if (function.empty() && call != nullptr) function = function_of(id);
    if (function.empty()) function = function_of(id);
    Json args = Json::array();
    if (call != nullptr) {
      if (const Json* value = call->find("args")) args = *value;
    }
    Json expect = Json::null();
    if (const Json* error = doc.find("err"); error != nullptr) {
      expect = Json::object({{"err", *error}});
    } else {
      const Json* ok = doc.find("ok");
      expect = Json::object({{"ok", ok != nullptr ? *ok : Json::null()}});
    }
    const Json entry = Json::object({
        {"id", Json::string(id)},
        {"fn", Json::string(function)},
        {"args", args},
        {"expect", expect},
    });
    text += dump(entry);
    text += '\n';
    ++count;
  }
  auto written = write_file(project.dir / "cases.jsonl", text);
  if (!written) return std::unexpected(written.error());
  return count;
}

Result<std::string> run_harness(const Project& project, std::string_view program,
                                const std::vector<std::string>& args,
                                const std::optional<std::filesystem::path>& cwd) {
  const Run run = run_command(std::string(program), args, cwd.value_or(project.dir));
  if (!run.ok) {
    return std::unexpected("the " + std::string(program) + " recorder failed: " +
                           run.failure() + "\n" + indent_text(run.err, 2));
  }
  return run.out;
}

Result<std::size_t> record(const Project& project, std::size_t count,
                           const RecordOptions& options) {
  const auto lang = parse(project.source_lang);
  if (!lang.has_value()) {
    return std::unexpected(project.source_lang + " is not a plugin language");
  }
  if (whole_app_only(*lang)) {
    return std::unexpected(std::string(label(*lang)) +
                           " is whole-app only: record the app (`app record -- ...`), not a unit");
  }
  const std::filesystem::path repo = project.package_root.value_or(project.dir);
  auto unit = inventory(repo, project.source_file.string(), *lang);
  if (!unit) return std::unexpected(unit.error());
  std::vector<Json> calls = corpora::calls(unit->functions, count);
  if (options.inputs.has_value()) {
    const auto text = read_file(*options.inputs);
    if (!text.has_value()) return std::unexpected(options.inputs->string() + ": cannot read");
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
        doc = cppan::parse(line);
      } catch (const JsonError& error) {
        return std::unexpected(options.inputs->string() + ":" + std::to_string(number) + ": " +
                               error.what());
      }
      Json args = Json::array();
      if (const Json* value = doc.find("args")) args = *value;
      const Json* fn = doc.find("fn");
      calls.push_back(Json::object({
          {"id", Json::string("input:" + std::to_string(number))},
          {"fn", fn != nullptr ? *fn : Json::null()},
          {"args", args},
      }));
    }
  }
  switch (*lang) {
    case Lang::Ruby: return ruby::record(project, *unit, calls);
    case Lang::Go: return go::record(project, *unit, calls);
    case Lang::Java: return java::record(project, *unit, calls);
    case Lang::Ts:
    case Lang::Js: return ts::record(project, *unit, calls, options, *lang);
    case Lang::Php:
    case Lang::Csharp: break;
  }
  return std::unexpected(std::string(label(*lang)) + " is whole-app only");
}

Result<double> bench_original(const Project& project, std::size_t repeat) {
  const auto lang = parse(project.source_lang);
  if (!lang.has_value()) return std::unexpected(project.source_lang + " is not a plugin language");
  switch (*lang) {
    case Lang::Ruby: return ruby::bench(project, repeat);
    case Lang::Go: return go::bench(project, repeat);
    case Lang::Java: return java::bench(project, repeat);
    case Lang::Ts:
    case Lang::Js: return ts::bench(project, repeat, *lang);
    case Lang::Php:
    case Lang::Csharp: break;
  }
  return std::unexpected(std::string(label(*lang)) +
                         " is whole-app only: bench the app, not a unit");
}

Result<std::vector<std::pair<std::string, std::string>>> shadow_files(const Project& project,
                                                                      std::string_view ns) {
  const auto lang = parse(project.source_lang);
  if (!lang.has_value()) return std::vector<std::pair<std::string, std::string>>{};
  if (whole_app_only(*lang)) return std::vector<std::pair<std::string, std::string>>{};
  switch (*lang) {
    case Lang::Ruby: return ruby::shadow_files(project, ns);
    case Lang::Go: return go::shadow_files(project, ns);
    case Lang::Java: return java::shadow_files(project, ns);
    case Lang::Ts:
    case Lang::Js: return ts::shadow_files(project, ns, *lang);
    case Lang::Php:
    case Lang::Csharp: break;
  }
  return std::vector<std::pair<std::string, std::string>>{};
}

}  // namespace cppan::langs
