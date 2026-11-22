// Go: a unit is a package. The analyzer reads exported functions and their Go
// types; the recorder builds a harness inside the module; the shadow binding uses
// cgo over the same C ABI.
module;
#include <cctype>

module cppan.langs;
import cppan.json;
import cppan.types;
import cppan.util;
import cppan.inventory;
import std;

namespace cppan::langs::go {
namespace {

struct GoParam {
  std::optional<std::string> name;
  std::string ty;
};

struct GoResult {
  std::string ty;
};

struct GoFunction {
  std::string name;
  std::size_t line = 0;
  std::vector<GoParam> params;
  std::vector<GoResult> results;
  bool generic = false;
  bool receiver = false;
  std::optional<std::string> malformed;
};

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

std::string_view trim_start(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  return text;
}

std::string_view strip_prefix(std::string_view text, std::string_view prefix) {
  return text.starts_with(prefix) ? text.substr(prefix.size()) : text;
}

bool is_space(char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; }

Result<std::vector<std::filesystem::path>> go_files(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) return std::unexpected(directory.string() + ": " + error.message());
  std::vector<std::filesystem::path> files;
  for (const auto& entry : iterator) {
    if (!entry.is_regular_file(error)) continue;
    const std::string name = entry.path().filename().string();
    if (name.starts_with('.') || name.starts_with('_')) continue;
    if (!name.ends_with(".go") || name.ends_with("_test.go")) continue;
    files.push_back(entry.path());
  }
  std::ranges::sort(files);
  return files;
}

bool is_exported(std::string_view name) {
  return !name.empty() && std::isupper(static_cast<unsigned char>(name.front())) != 0;
}

std::optional<std::string> quoted_path(std::string_view text) {
  const std::size_t start = text.find('"');
  if (start == std::string_view::npos) return std::nullopt;
  const std::size_t end = text.find('"', start + 1);
  if (end == std::string_view::npos) return std::nullopt;
  return std::string(text.substr(start + 1, end - start - 1));
}

void collect_imports(std::string_view source, std::vector<std::string>& imports) {
  bool block = false;
  for (const std::string_view line : split_lines(source)) {
    const std::string_view trimmed = trim(line);
    if (block) {
      if (trimmed.starts_with(')')) {
        block = false;
        continue;
      }
      if (const auto path = quoted_path(trimmed); path.has_value()) imports.push_back(*path);
      continue;
    }
    if (!trimmed.starts_with("import")) continue;
    const std::string_view rest = trimmed.substr(6);
    if (rest.empty() || !(is_space(rest.front()) || rest.front() == '(')) continue;
    const std::string_view after = trim_start(rest);
    if (after.starts_with('(')) {
      block = true;
    } else if (const auto path = quoted_path(after); path.has_value()) {
      imports.push_back(*path);
    }
  }
}

std::string doc_comment(const std::vector<std::string_view>& lines, std::size_t index) {
  std::vector<std::string> out;
  std::size_t at = index;
  while (at > 0) {
    --at;
    const std::string_view line = trim(lines[at]);
    if (!line.starts_with("//")) break;
    out.emplace_back(trim(line.substr(2)));
  }
  std::ranges::reverse(out);
  std::string joined;
  for (const std::string& line : out) {
    if (!joined.empty()) joined += ' ';
    joined += line;
  }
  return joined;
}

std::string_view strip_line_comment(std::string_view line) {
  const std::size_t position = line.find("//");
  return position == std::string_view::npos ? line : line.substr(0, position);
}

bool starts_func(std::string_view trimmed) {
  if (!trimmed.starts_with("func")) return false;
  const std::string_view rest = trimmed.substr(4);
  return rest.empty() || is_space(rest.front()) || rest.front() == '(';
}

std::optional<std::pair<std::string, std::string>> split_pair(std::string_view text, char open,
                                                              char close) {
  if (!text.starts_with(open)) return std::nullopt;
  int depth = 0;
  for (std::size_t position = 0; position < text.size(); ++position) {
    const char ch = text[position];
    if (ch == open) {
      ++depth;
    } else if (ch == close) {
      --depth;
      if (depth == 0) {
        return std::pair{std::string(text.substr(1, position - 1)),
                         std::string(text.substr(position + 1))};
      }
    }
  }
  return std::nullopt;
}

Result<std::vector<GoParam>> parse_params(std::string_view text) {
  std::vector<GoParam> out;
  std::vector<std::string> pending;
  const auto flush = [&](const std::string& ty) {
    for (const std::string& name : pending) out.push_back(GoParam{name, ty});
    pending.clear();
  };
  for (std::string_view part : split_top_level(text, ',')) {
    part = trim(part);
    if (part.empty()) continue;
    if (part.starts_with("...")) {
      return std::unexpected("variadic parameters have no wire form");
    }
    const std::size_t space = part.find_first_of(" \t\n\r\v\f");
    bool named = false;
    if (space != std::string_view::npos) {
      const std::string_view name = part.substr(0, space);
      const std::string_view ty = trim(part.substr(space));
      if (!ty.empty() && !name.empty() &&
          std::ranges::all_of(name, [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
          }) &&
          (std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_')) {
        if (ty.starts_with("...")) {
          return std::unexpected("variadic parameter `" + std::string(name) +
                                 "` has no wire form");
        }
        pending.emplace_back(name);
        flush(std::string(ty));
        named = true;
      }
    }
    if (!named) pending.emplace_back(part);
  }
  if (!pending.empty()) {
    if (out.empty()) {
      for (const std::string& ty : pending) out.push_back(GoParam{std::nullopt, ty});
    } else {
      std::string names;
      for (const std::string& name : pending) {
        if (!names.empty()) names += ", ";
        names += name;
      }
      return std::unexpected("parameter `" + names + "` has no type");
    }
  }
  return out;
}

Result<std::vector<GoResult>> parse_results(std::string_view text) {
  text = trim(text);
  if (text.empty()) return std::vector<GoResult>{};
  if (text.starts_with('(')) {
    const auto pair = split_pair(text, '(', ')');
    if (!pair) return std::unexpected("an unclosed result list");
    if (!trim(pair->second).empty()) {
      return std::unexpected("`" + std::string(trim(pair->second)) + "` after the result list");
    }
    std::vector<GoResult> out;
    std::vector<std::string> pending;
    for (std::string_view part : split_top_level(pair->first, ',')) {
      part = trim(part);
      if (part.empty()) continue;
      const std::size_t space = part.find_first_of(" \t\n\r\v\f");
      bool named = false;
      if (space != std::string_view::npos) {
        const std::string_view name = part.substr(0, space);
        const std::string_view ty = trim(part.substr(space));
        if (!ty.empty() && !name.empty() &&
            std::ranges::all_of(name, [](char ch) {
              return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
            }) &&
            (std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_')) {
          pending.emplace_back(name);
          for (std::size_t count = 0; count < pending.size(); ++count) {
            out.push_back(GoResult{std::string(ty)});
          }
          pending.clear();
          named = true;
        }
      }
      if (!named) pending.emplace_back(part);
    }
    if (!pending.empty()) {
      if (out.empty()) {
        for (const std::string& ty : pending) out.push_back(GoResult{ty});
      } else {
        std::string names;
        for (const std::string& name : pending) {
          if (!names.empty()) names += ", ";
          names += name;
        }
        return std::unexpected("result `" + names + "` has no type");
      }
    }
    return out;
  }
  return std::vector<GoResult>{GoResult{std::string(text)}};
}

Result<std::string> map_go_type(std::string_view text) {
  text = trim(text);
  if (text.empty()) return std::unexpected("an empty type");
  for (const std::string_view candidate :
       {"int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64",
        "uintptr", "byte", "rune"}) {
    if (text == candidate) return "int";
  }
  if (text == "float32" || text == "float64") return "float";
  if (text == "string") return "str";
  if (text == "bool") return "bool";
  if (text == "error") {
    return std::unexpected("the `error` type has no wire form (a nil error is not a value)");
  }
  if (text.starts_with("[]")) {
    auto inner = map_go_type(text.substr(2));
    if (!inner) return std::unexpected(inner.error());
    return "list[" + *inner + "]";
  }
  if (text.starts_with("map[")) {
    const std::string_view inner = text.substr(4);
    int depth = 1;
    std::optional<std::size_t> end;
    for (std::size_t position = 0; position < inner.size(); ++position) {
      if (inner[position] == '[') {
        ++depth;
      } else if (inner[position] == ']') {
        --depth;
        if (depth == 0) {
          end = position;
          break;
        }
      }
    }
    if (!end.has_value()) return std::unexpected("an unclosed map type");
    const std::string_view key = trim(inner.substr(0, *end));
    const std::string_view value = trim(inner.substr(*end + 1));
    if (key != "string") return std::unexpected("map key `" + std::string(key) + "` is not string");
    auto mapped = map_go_type(value);
    if (!mapped) return std::unexpected(mapped.error());
    return "dict[" + *mapped + "]";
  }
  if (text.starts_with("...")) return std::unexpected("variadic parameters have no wire form");
  if (text.starts_with('*')) return std::unexpected("pointer types do not cross the wire");
  if (text.starts_with('[')) return std::unexpected("fixed-size arrays do not cross the wire");
  if (text.starts_with("func")) return std::unexpected("function values do not cross the wire");
  if (text.starts_with("chan") || text.starts_with("<-chan")) {
    return std::unexpected("channels do not cross the wire");
  }
  if (text == "any" || text.starts_with("interface")) {
    return std::unexpected("interface types do not cross the wire");
  }
  if (text.starts_with("struct")) return std::unexpected("struct types do not cross the wire");
  if (text.find('|') != std::string_view::npos || text.find('~') != std::string_view::npos) {
    return std::unexpected("type constraints have no wire form");
  }
  if (text.starts_with("complex")) return std::unexpected("complex numbers do not cross the wire");
  return std::unexpected("`" + std::string(text) + "` is not a type the wire carries");
}

GoFunction parse_signature(std::string_view signature, std::size_t line) {
  GoFunction function;
  function.line = line;
  std::string_view rest = trim(signature);
  rest = trim_start(strip_prefix(rest, "func"));
  if (rest.starts_with('(')) {
    function.receiver = true;
    const auto pair = split_pair(rest, '(', ')');
    if (!pair) {
      function.malformed = "an unclosed method receiver";
      return function;
    }
    rest = trim_start(pair->second);
  }
  std::size_t name_end = rest.find_first_of(" \t\n\r\v\f[(");
  if (name_end == std::string_view::npos) name_end = rest.size();
  const std::string name(rest.substr(0, name_end));
  function.name = name;
  rest = trim_start(rest.substr(name_end));
  if (name.empty() || !std::ranges::all_of(name, [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
      })) {
    function.malformed = "the function name is not a plain identifier";
    return function;
  }
  if (rest.starts_with('[')) {
    function.generic = true;
    const auto pair = split_pair(rest, '[', ']');
    if (!pair) {
      function.malformed = "an unclosed type parameter list";
      return function;
    }
    rest = trim_start(pair->second);
  }
  if (!rest.starts_with('(')) {
    function.malformed = "a parameter list is missing";
    return function;
  }
  const auto pair = split_pair(rest, '(', ')');
  if (!pair) {
    function.malformed = "an unclosed parameter list";
    return function;
  }
  auto params = parse_params(pair->first);
  if (params) {
    function.params = std::move(*params);
  } else {
    function.malformed = params.error();
  }
  const std::string_view results = trim(pair->second);
  if (!results.empty()) {
    auto parsed = parse_results(results);
    if (parsed) {
      function.results = std::move(*parsed);
    } else {
      function.malformed = parsed.error();
    }
  }
  return function;
}

std::vector<GoFunction> scan_functions(std::string_view source) {
  const auto lines = split_lines(source);
  std::vector<GoFunction> out;
  std::size_t index = 0;
  while (index < lines.size()) {
    const std::size_t start = index;
    if (!starts_func(trim_start(lines[index]))) {
      ++index;
      continue;
    }
    std::string signature;
    std::size_t cursor = index;
    int parens = 0;
    int brackets = 0;
    bool seen_params = false;
    bool finished = false;
    while (cursor < lines.size()) {
      const std::string_view content = strip_line_comment(lines[cursor]);
      for (const char ch : content) {
        if (ch == '{' && parens == 0 && brackets == 0 && seen_params) {
          finished = true;
          break;
        }
        signature += ch;
        if (ch == '(') {
          ++parens;
        } else if (ch == ')') {
          --parens;
          if (parens == 0) seen_params = true;
        } else if (ch == '[') {
          ++brackets;
        } else if (ch == ']') {
          --brackets;
        }
      }
      if (finished) break;
      signature += '\n';
      ++cursor;
    }
    out.push_back(parse_signature(signature, start));
    index = std::max(cursor, start) + 1;
  }
  return out;
}

}  // namespace

Result<Analysis> analyze_package(const std::filesystem::path& directory) {
  auto files = go_files(directory);
  if (!files) return std::unexpected(files.error());
  Analysis analysis;
  std::uint32_t lines = 0;
  std::vector<std::string> imports;

  for (const auto& file : *files) {
    const auto source = read_file(file);
    if (!source.has_value()) return std::unexpected(file.string() + ": cannot read");
    lines += static_cast<std::uint32_t>(line_count(*source));
    collect_imports(*source, imports);
    const auto source_lines = split_lines(*source);
    for (const GoFunction& function : scan_functions(*source)) {
      if (function.receiver) continue;
      const std::string& name = function.name;
      if (name.empty()) continue;
      if (!is_exported(name)) continue;
      if (function.malformed.has_value()) {
        analysis.skipped.emplace_back(name, *function.malformed);
        continue;
      }
      if (function.generic) {
        analysis.skipped.emplace_back(name, "generic functions have no wire form");
        continue;
      }
      std::vector<FoundParam> params;
      std::optional<std::string> reason;
      for (std::size_t index = 0; index < function.params.size(); ++index) {
        const GoParam& param = function.params[index];
        auto mapped = map_go_type(param.ty);
        if (!mapped) {
          const std::string label =
              param.name.has_value() && !param.name->empty() ? *param.name : param.ty;
          reason = "parameter `" + label + "`: " + mapped.error();
          break;
        }
        std::string param_name;
        if (param.name.has_value() && !param.name->empty() && *param.name != "_") {
          param_name = *param.name;
        } else {
          param_name = "arg" + std::to_string(index);
        }
        params.push_back(
            FoundParam{param_name, *mapped, false, std::nullopt});
      }
      if (reason.has_value()) {
        analysis.skipped.emplace_back(name, *reason);
        continue;
      }
      if (function.results.size() > 1) {
        std::string types;
        for (const GoResult& result : function.results) {
          if (!types.empty()) types += ", ";
          types += result.ty;
        }
        analysis.skipped.emplace_back(
            name, "multiple results `(" + types + ")`: the wire carries one value");
        continue;
      }
      std::string returns;
      if (function.results.empty()) {
        returns = "None";
      } else {
        auto mapped = map_go_type(function.results.front().ty);
        if (!mapped) {
          analysis.skipped.emplace_back(name, "result: " + mapped.error());
          continue;
        }
        returns = *mapped;
      }
      Found found;
      found.name = name;
      found.line = static_cast<std::uint32_t>(function.line + 1);
      found.params = std::move(params);
      found.returns = returns;
      found.doc = doc_comment(source_lines, function.line);
      analysis.functions.push_back(std::move(found));
    }
  }

  for (Found& function : analysis.functions) function.call = function.name;
  std::ranges::sort(imports);
  imports.erase(std::unique(imports.begin(), imports.end()), imports.end());

  analysis.imports = std::move(imports);
  analysis.lines = lines;
  return analysis;
}

namespace {

std::string go_quote(std::string_view text) {
  std::string out = "\"";
  for (const char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20 ||
            static_cast<unsigned char>(ch) == 0x7f) {
          out += std::format("\\u{:04x}",
                             static_cast<unsigned>(static_cast<unsigned char>(ch)));
        } else {
          out += ch;
        }
    }
  }
  out += '"';
  return out;
}

std::string replace_all(std::string text, std::string_view needle, std::string_view with) {
  if (needle.empty()) return text;
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), with);
    at += with.size();
  }
  return text;
}

std::vector<std::string> path_parts(const std::filesystem::path& path) {
  std::vector<std::string> parts;
  for (const auto& part : path) {
    std::string text = part.string();
    if (text.empty()) continue;
    if (text == "." && !parts.empty()) continue;
    parts.push_back(std::move(text));
  }
  return parts;
}

bool parts_start_with(const std::vector<std::string>& parts,
                      const std::vector<std::string>& prefix) {
  if (prefix.size() > parts.size()) return false;
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (parts[index] != prefix[index]) return false;
  }
  return true;
}

std::string join_parts(const std::vector<std::string>& parts, std::size_t from) {
  std::string joined;
  for (std::size_t index = from; index < parts.size(); ++index) {
    if (parts[index] == "." || parts[index] == "..") continue;
    if (!joined.empty()) joined += '/';
    joined += parts[index];
  }
  return joined;
}

bool is_file(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error);
}

std::optional<std::filesystem::path> find_module_root(
    const std::filesystem::path& package_dir) {
  std::filesystem::path current = package_dir;
  while (true) {
    if (is_file(current / "go.mod")) return current;
    if (current.empty()) return std::nullopt;
    const std::filesystem::path parent = current.parent_path();
    if (parent == current) return std::nullopt;
    current = parent;
  }
}

Result<std::string> module_path(const std::filesystem::path& root) {
  const std::filesystem::path file = root / "go.mod";
  const auto text = read_file(file);
  if (!text.has_value()) return std::unexpected(file.string() + ": cannot read");
  for (const std::string_view line : split_lines(*text)) {
    const std::string_view trimmed = trim(line);
    if (!trimmed.starts_with("module ")) continue;
    const std::string_view rest = trimmed.substr(7);
    std::size_t start = 0;
    while (start < rest.size() && is_space(rest[start])) ++start;
    std::size_t end = start;
    while (end < rest.size() && !is_space(rest[end])) ++end;
    std::string_view module = rest.substr(start, end - start);
    while (!module.empty() && module.front() == '"') module.remove_prefix(1);
    while (!module.empty() && module.back() == '"') module.remove_suffix(1);
    if (!module.empty()) return std::string(module);
  }
  return std::unexpected(file.string() + ": no module line");
}

std::string relative_import(std::string_view module, const std::filesystem::path& root,
                            const std::filesystem::path& package_dir) {
  const std::vector<std::string> parts = path_parts(package_dir);
  const std::vector<std::string> prefix = path_parts(root);
  std::string joined;
  if (parts_start_with(parts, prefix)) joined = join_parts(parts, prefix.size());
  return joined.empty() ? std::string(module) : std::string(module) + "/" + joined;
}

Result<std::string> import_path(const std::filesystem::path& package_dir) {
  const auto root = find_module_root(package_dir);
  if (!root.has_value()) {
    return std::unexpected(package_dir.string() +
                           ": no go.mod above this package; the Go binding needs the module path");
  }
  auto module = module_path(*root);
  if (!module) return std::unexpected(module.error());
  return relative_import(*module, *root, package_dir);
}

struct Harness {
  std::string import;
  std::filesystem::path directory;
  std::string run;
  std::filesystem::path cwd;
  std::vector<std::filesystem::path> temporary;
};

/// Picks where the harness lives: inside the package's module when it has one,
/// else a throwaway module whose `replace` points at the repository.
Result<Harness> locate_harness(const Project& project,
                               const std::filesystem::path& package_dir,
                               std::string_view ns) {
  if (const auto root = find_module_root(package_dir); root.has_value()) {
    auto module = module_path(*root);
    if (!module) return std::unexpected(module.error());
    const std::filesystem::path directory =
        *root / ("cppan-record-" + std::string(ns));
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
      error.clear();
      std::filesystem::remove_all(directory, error);
      if (error) return std::unexpected(directory.string() + ": " + error.message());
    }
    return Harness{relative_import(*module, *root, package_dir), directory,
                   "./cppan-record-" + std::string(ns), *root, {}};
  }

  std::filesystem::path repo = package_dir;
  if (project.package_root.has_value() &&
      parts_start_with(path_parts(package_dir), path_parts(*project.package_root))) {
    repo = *project.package_root;
  }
  const std::filesystem::path module_file = repo / "go.mod";
  std::error_code error;
  const bool created_module = !std::filesystem::exists(module_file, error);
  if (created_module) {
    auto written = write_file(module_file, "module cppanoriginal\n\ngo 1.21\n");
    if (!written) return std::unexpected(written.error());
  }
  const std::filesystem::path directory =
      project.dir / "build" / ("gorecord-" + std::string(ns));
  if (std::filesystem::exists(directory, error)) {
    error.clear();
    std::filesystem::remove_all(directory, error);
    if (error) return std::unexpected(directory.string() + ": " + error.message());
  }
  const std::vector<std::string> package_parts = path_parts(package_dir);
  const std::vector<std::string> repo_parts = path_parts(repo);
  const std::string relative =
      parts_start_with(package_parts, repo_parts) ? join_parts(package_parts, repo_parts.size())
                                                  : std::string();
  const std::string import = relative_import(
      "cppanoriginal", std::filesystem::path{},
      std::filesystem::path(relative));
  auto written = write_file(
      directory / "go.mod",
      "module cppanrecord\n\ngo 1.21\n\nrequire cppanoriginal v0.0.0\n\nreplace cppanoriginal => " +
          repo.string() + "\n");
  if (!written) return std::unexpected(written.error());
  std::vector<std::filesystem::path> temporary;
  if (created_module) temporary.push_back(module_file);
  return Harness{import, directory, ".", directory, std::move(temporary)};
}

/// Writes the harness, runs it, and removes the temporary module either way.
Result<std::string> execute(const Project& project, Harness harness, std::string_view body,
                            std::uint64_t* rss_kb = nullptr) {
  const auto written = write_file(harness.directory / "main.go", body);
  Result<std::string> result =
      written ? run_harness(project, "go", {"run", harness.run}, harness.cwd, rss_kb)
              : Result<std::string>(std::unexpected(written.error()));
  std::error_code error;
  std::filesystem::remove_all(harness.directory, error);
  for (const auto& path : harness.temporary) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  return result;
}

constexpr std::string_view kGoHarnessHelpers = R"CPPAN(
var cppanErrorInterface = reflect.TypeOf((*error)(nil)).Elem()

type cppanError struct {
	Type string `json:"type"`
	Msg  string `json:"msg"`
}

func cppanDecode(text string, target interface{}) error {
	decoder := json.NewDecoder(strings.NewReader(text))
	decoder.UseNumber()
	return decoder.Decode(target)
}

func cppanInvoke(function interface{}, args []interface{}) (result interface{}, failure *cppanError) {
	defer func() {
		if recovered := recover(); recovered != nil {
			result = nil
			failure = &cppanError{Type: "panic", Msg: cppanTrim(fmt.Sprintf("%T: %v", recovered, recovered))}
		}
	}()
	value := reflect.ValueOf(function)
	signature := value.Type()
	if signature.IsVariadic() {
		return nil, &cppanError{Type: "ArgumentError", Msg: "variadic functions have no wire form"}
	}
	if len(args) != signature.NumIn() {
		return nil, &cppanError{Type: "ArgumentError", Msg: fmt.Sprintf("expected %d arguments, got %d", signature.NumIn(), len(args))}
	}
	values := make([]reflect.Value, len(args))
	for index, arg := range args {
		converted, err := cppanConvert(arg, signature.In(index))
		if err != nil {
			return nil, &cppanError{Type: "ArgumentError", Msg: err.Error()}
		}
		values[index] = converted
	}
	out := value.Call(values)
	if len(out) > 0 {
		last := out[len(out)-1]
		if last.Type().Implements(cppanErrorInterface) {
			if cppanNilable(last.Kind()) && last.IsNil() {
				out = out[:len(out)-1]
			} else {
				original := last.Interface().(error)
				return nil, &cppanError{Type: cppanErrorName(original), Msg: cppanTrim(original.Error())}
			}
		}
	}
	switch len(out) {
	case 0:
		return nil, nil
	case 1:
		return out[0].Interface(), nil
	default:
		values := make([]interface{}, len(out))
		for index := range out {
			values[index] = out[index].Interface()
		}
		return values, nil
	}
}

func cppanNilable(kind reflect.Kind) bool {
	switch kind {
	case reflect.Chan, reflect.Func, reflect.Interface, reflect.Map, reflect.Ptr, reflect.Slice:
		return true
	default:
		return false
	}
}

func cppanConvert(value interface{}, target reflect.Type) (reflect.Value, error) {
	if value == nil {
		switch target.Kind() {
		case reflect.Interface, reflect.Ptr, reflect.Slice, reflect.Map:
			return reflect.Zero(target), nil
		default:
			return reflect.Value{}, fmt.Errorf("null is not a %s", target)
		}
	}
	switch target.Kind() {
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		number, err := cppanInt(value)
		if err != nil {
			return reflect.Value{}, err
		}
		if target.OverflowInt(number) {
			return reflect.Value{}, fmt.Errorf("%d overflows %s", number, target)
		}
		out := reflect.New(target).Elem()
		out.SetInt(number)
		return out, nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		number, err := cppanUint(value)
		if err != nil {
			return reflect.Value{}, err
		}
		if target.OverflowUint(number) {
			return reflect.Value{}, fmt.Errorf("%d overflows %s", number, target)
		}
		out := reflect.New(target).Elem()
		out.SetUint(number)
		return out, nil
	case reflect.Float32, reflect.Float64:
		number, err := cppanFloat(value)
		if err != nil {
			return reflect.Value{}, err
		}
		out := reflect.New(target).Elem()
		out.SetFloat(number)
		return out, nil
	case reflect.String:
		text, ok := value.(string)
		if !ok {
			return reflect.Value{}, fmt.Errorf("%v is not a string", value)
		}
		out := reflect.New(target).Elem()
		out.SetString(text)
		return out, nil
	case reflect.Bool:
		flag, ok := value.(bool)
		if !ok {
			return reflect.Value{}, fmt.Errorf("%v is not a bool", value)
		}
		out := reflect.New(target).Elem()
		out.SetBool(flag)
		return out, nil
	case reflect.Slice:
		items, ok := value.([]interface{})
		if !ok {
			return reflect.Value{}, fmt.Errorf("%v is not an array", value)
		}
		out := reflect.MakeSlice(target, len(items), len(items))
		for index, item := range items {
			converted, err := cppanConvert(item, target.Elem())
			if err != nil {
				return reflect.Value{}, err
			}
			out.Index(index).Set(converted)
		}
		return out, nil
	case reflect.Map:
		if target.Key().Kind() != reflect.String {
			return reflect.Value{}, fmt.Errorf("map keys must be strings, not %s", target.Key())
		}
		object, ok := value.(map[string]interface{})
		if !ok {
			return reflect.Value{}, fmt.Errorf("%v is not an object", value)
		}
		out := reflect.MakeMapWithSize(target, len(object))
		for key, item := range object {
			converted, err := cppanConvert(item, target.Elem())
			if err != nil {
				return reflect.Value{}, err
			}
			out.SetMapIndex(reflect.ValueOf(key).Convert(target.Key()), converted)
		}
		return out, nil
	case reflect.Interface:
		dynamic, err := cppanDynamic(value)
		if err != nil {
			return reflect.Value{}, err
		}
		if !dynamic.Type().AssignableTo(target) {
			return reflect.Value{}, fmt.Errorf("%s is not assignable to %s", dynamic.Type(), target)
		}
		return dynamic, nil
	default:
		return reflect.Value{}, fmt.Errorf("%s has no JSON form", target)
	}
}

func cppanDynamic(value interface{}) (reflect.Value, error) {
	if value == nil {
		return reflect.Zero(reflect.TypeOf((*interface{})(nil)).Elem()), nil
	}
	if number, ok := value.(json.Number); ok {
		if integer, err := number.Int64(); err == nil {
			return reflect.ValueOf(integer), nil
		}
		if real, err := number.Float64(); err == nil {
			return reflect.ValueOf(real), nil
		}
		return reflect.Value{}, fmt.Errorf("%s is not a number", number)
	}
	if items, ok := value.([]interface{}); ok {
		out := make([]interface{}, len(items))
		for index, item := range items {
			converted, err := cppanDynamic(item)
			if err != nil {
				return reflect.Value{}, err
			}
			out[index] = converted.Interface()
		}
		return reflect.ValueOf(out), nil
	}
	if object, ok := value.(map[string]interface{}); ok {
		out := make(map[string]interface{}, len(object))
		for key, item := range object {
			converted, err := cppanDynamic(item)
			if err != nil {
				return reflect.Value{}, err
			}
			out[key] = converted.Interface()
		}
		return reflect.ValueOf(out), nil
	}
	return reflect.ValueOf(value), nil
}

func cppanInt(value interface{}) (int64, error) {
	switch typed := value.(type) {
	case json.Number:
		if integer, err := typed.Int64(); err == nil {
			return integer, nil
		}
		real, err := typed.Float64()
		if err != nil {
			return 0, fmt.Errorf("%s is not an integer", typed)
		}
		return cppanFloatToInt(real)
	case float64:
		return cppanFloatToInt(typed)
	case int64:
		return typed, nil
	case int:
		return int64(typed), nil
	}
	return 0, fmt.Errorf("%v is not an integer", value)
}

func cppanFloatToInt(number float64) (int64, error) {
	if math.IsNaN(number) || math.IsInf(number, 0) || number != math.Trunc(number) || number < math.MinInt64 || number > math.MaxInt64 {
		return 0, fmt.Errorf("%v is not an int64", number)
	}
	return int64(number), nil
}

func cppanUint(value interface{}) (uint64, error) {
	switch typed := value.(type) {
	case json.Number:
		if integer, err := typed.Int64(); err == nil {
			if integer < 0 {
				return 0, fmt.Errorf("%s is negative", typed)
			}
			return uint64(integer), nil
		}
		if unsigned, err := strconv.ParseUint(string(typed), 10, 64); err == nil {
			return unsigned, nil
		}
		real, err := typed.Float64()
		if err != nil {
			return 0, fmt.Errorf("%s is not an integer", typed)
		}
		if real < 0 || math.IsNaN(real) || math.IsInf(real, 0) {
			return 0, fmt.Errorf("%s is not a uint64", typed)
		}
		return uint64(real), nil
	case float64:
		if typed < 0 || math.IsNaN(typed) || math.IsInf(typed, 0) {
			return 0, fmt.Errorf("%v is not a uint64", typed)
		}
		return uint64(typed), nil
	case int:
		if typed < 0 {
			return 0, fmt.Errorf("%d is negative", typed)
		}
		return uint64(typed), nil
	case int64:
		if typed < 0 {
			return 0, fmt.Errorf("%d is negative", typed)
		}
		return uint64(typed), nil
	}
	return 0, fmt.Errorf("%v is not an integer", value)
}

func cppanFloat(value interface{}) (float64, error) {
	switch typed := value.(type) {
	case json.Number:
		return typed.Float64()
	case float64:
		return typed, nil
	case int64:
		return float64(typed), nil
	case int:
		return float64(typed), nil
	}
	return 0, fmt.Errorf("%v is not a number", value)
}

func cppanEncode(value interface{}) (interface{}, *cppanError) {
	return cppanEncodeValue(reflect.ValueOf(value))
}

func cppanEncodeValue(value reflect.Value) (interface{}, *cppanError) {
	if !value.IsValid() {
		return nil, nil
	}
	for value.Kind() == reflect.Interface || value.Kind() == reflect.Ptr {
		if value.IsNil() {
			return nil, nil
		}
		value = value.Elem()
	}
	switch value.Kind() {
	case reflect.Bool:
		return value.Bool(), nil
	case reflect.String:
		return value.String(), nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return value.Int(), nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		number := value.Uint()
		if number > math.MaxInt64 {
			return nil, &cppanError{Type: "RangeError", Msg: fmt.Sprintf("%d does not fit int64", number)}
		}
		return int64(number), nil
	case reflect.Float32, reflect.Float64:
		number := value.Float()
		if math.IsNaN(number) || math.IsInf(number, 0) {
			return nil, &cppanError{Type: "RangeError", Msg: "non-finite float"}
		}
		return number, nil
	case reflect.Slice, reflect.Array:
		items := make([]interface{}, value.Len())
		for index := 0; index < value.Len(); index++ {
			encoded, failure := cppanEncodeValue(value.Index(index))
			if failure != nil {
				return nil, failure
			}
			items[index] = encoded
		}
		return items, nil
	case reflect.Map:
		if value.Type().Key().Kind() != reflect.String {
			return nil, &cppanError{Type: "TypeError", Msg: "map keys must be strings"}
		}
		object := make(map[string]interface{}, value.Len())
		iterator := value.MapRange()
		for iterator.Next() {
			encoded, failure := cppanEncodeValue(iterator.Value())
			if failure != nil {
				return nil, failure
			}
			object[iterator.Key().String()] = encoded
		}
		return object, nil
	default:
		return map[string]interface{}{"$opaque": value.Type().String()}, nil
	}
}

func cppanErrorName(err error) string {
	name := reflect.TypeOf(err).String()
	if position := strings.LastIndex(name, "."); position >= 0 {
		name = name[position+1:]
	}
	name = strings.TrimPrefix(name, "*")
	if name == "" {
		return "error"
	}
	return name
}

func cppanTrim(text string) string {
	if len(text) > 200 {
		return text[:200]
	}
	return text
}

func cppanEmit(id string, value interface{}) {
	encoded, failure := cppanEncode(value)
	if failure != nil {
		cppanEmitError(id, failure)
		return
	}
	cppanPrint(map[string]interface{}{"id": id, "ok": encoded})
}

func cppanEmitError(id string, failure *cppanError) {
	if id == "" {
		fmt.Fprintln(os.Stderr, "cppan record:", failure.Type, failure.Msg)
		return
	}
	cppanPrint(map[string]interface{}{"id": id, "err": failure})
}

func cppanPrint(value interface{}) {
	text, err := json.Marshal(value)
	if err != nil {
		fmt.Fprintln(os.Stderr, "cppan record:", err)
		return
	}
	fmt.Println(string(text))
}
)CPPAN";

constexpr std::string_view kRecordMain = R"CPPAN(

func main() {
	for _, line := range cppanCalls {
		var call struct {
			ID   string            `json:"id"`
			Fn   string            `json:"fn"`
			Args []json.RawMessage `json:"args"`
		}
		if err := cppanDecode(line, &call); err != nil {
			fmt.Fprintln(os.Stderr, "cppan record: bad call line:", err)
			continue
		}
		args := make([]interface{}, 0, len(call.Args))
		failed := false
		for _, raw := range call.Args {
			var value interface{}
			if err := cppanDecode(string(raw), &value); err != nil {
				cppanEmitError(call.ID, &cppanError{Type: "DecodeError", Msg: err.Error()})
				failed = true
				break
			}
			args = append(args, value)
		}
		if failed {
			continue
		}
		function, ok := cppanFunctions[call.Fn]
		if !ok {
			cppanEmitError(call.ID, &cppanError{Type: "NameError", Msg: "no function named " + call.Fn})
			continue
		}
		value, failure := cppanInvoke(function, args)
		if failure != nil {
			cppanEmitError(call.ID, failure)
		} else {
			cppanEmit(call.ID, value)
		}
	}
}
)CPPAN";

constexpr std::string_view kBenchMain = R"CPPAN(

func main() {
	for _, line := range cppanBench {
		cppanBenchCall(line)
	}
	started := time.Now()
	for round := 0; round < cppanRepeat; round++ {
		for _, line := range cppanBench {
			cppanBenchCall(line)
		}
	}
	elapsed := time.Since(started).Nanoseconds()
	fmt.Printf("{\"ns\":%d,\"calls\":%d}\n", elapsed, len(cppanBench)*cppanRepeat)
}

func cppanBenchCall(line string) {
	var call struct {
		Fn   string            `json:"fn"`
		Args []json.RawMessage `json:"args"`
	}
	if err := cppanDecode(line, &call); err != nil {
		return
	}
	args := make([]interface{}, 0, len(call.Args))
	for _, raw := range call.Args {
		var value interface{}
		if err := cppanDecode(string(raw), &value); err != nil {
			return
		}
		args = append(args, value)
	}
	function, ok := cppanFunctions[call.Fn]
	if !ok {
		return
	}
	_, _ = cppanInvoke(function, args)
}
)CPPAN";

std::string render_record_harness(std::string_view import,
                                  const std::vector<std::string>& functions,
                                  const std::vector<Json>& calls) {
  std::string source;
  source +=
      "// Generated by cpp-anything record. Runs the original once and prints one JSON line "
      "per call.\n";
  source += "package main\n\n";
  source +=
      "import (\n\t\"encoding/json\"\n\t\"fmt\"\n\t\"math\"\n\t\"os\"\n\t\"reflect\"\n\t\"strconv\""
      "\n\t\"strings\"\n\n\tcppanoriginal ";
  source += go_quote(import);
  source += "\n)\n\n";
  source += "var cppanCalls = []string{\n";
  for (const Json& call : calls) {
    source += '\t';
    source += go_quote(dump(call));
    source += ",\n";
  }
  source += "}\n\n";
  source += "var cppanFunctions = map[string]interface{}{\n";
  for (const std::string& name : functions) {
    source += '\t';
    source += go_quote(name);
    source += ": cppanoriginal.";
    source += name;
    source += ",\n";
  }
  source += "}\n\n";
  source += kGoHarnessHelpers;
  source += kRecordMain;
  return source;
}

}  // namespace

Result<std::size_t> record(const Project& project, const Unit& unit,
                           const std::vector<Json>& calls) {
  const std::filesystem::path calls_path = project.dir / "build" / "record-calls.jsonl";
  auto written = write_file(calls_path, cases_json(calls));
  if (!written) return std::unexpected(written.error());
  if (unit.functions.empty()) {
    return std::unexpected("the package has no functions with a supported signature");
  }
  const std::string ns = sanitize_identifier(project.unit);
  auto harness = locate_harness(project, unit.file, ns);
  if (!harness) return std::unexpected(harness.error());
  std::vector<std::string> functions;
  functions.reserve(unit.functions.size());
  for (const Function& function : unit.functions) functions.push_back(function.name);
  const std::string body = render_record_harness(harness->import, functions, calls);
  auto output = execute(project, std::move(*harness), body);
  if (!output) return std::unexpected(output.error());
  return write_cases(project, calls, *output);
}

Result<BenchMeasurement> bench(const Project& project, std::size_t repeat) {
  const std::filesystem::path cases_path = project.dir / "cases.jsonl";
  const auto text = read_file(cases_path);
  if (!text.has_value()) return std::unexpected(cases_path.string() + ": cannot read");
  std::map<std::string, Json> first;
  for (const std::string_view line : split_lines(*text)) {
    bool empty = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
    }
    if (empty) continue;
    Json doc;
    try {
      doc = cppan::parse(line);
    } catch (const JsonError& error) {
      return std::unexpected(error.what());
    }
    std::string function;
    if (const Json* fn = doc.find("fn"); fn != nullptr && fn->is_string()) {
      function = fn->as_string();
    }
    first.try_emplace(function, doc);
  }
  std::vector<std::string> calls;
  for (const auto& [key, doc] : first) {
    (void)key;
    const Json* fn = doc.find("fn");
    if (fn == nullptr || !fn->is_string()) return std::unexpected("a case without fn");
    Json args = Json::array();
    if (const Json* value = doc.find("args")) args = *value;
    calls.push_back(dump(Json::object({
        {"fn", Json::string(fn->as_string())},
        {"args", args},
    })));
  }
  if (calls.empty()) return std::unexpected("no recorded cases to benchmark");
  const std::string ns = sanitize_identifier(project.unit);
  auto harness = locate_harness(project, project.source_file, ns);
  if (!harness) return std::unexpected(harness.error());

  std::string source;
  source +=
      "// Generated by cpp-anything bench. Times the original on its first recorded case per "
      "function.\n";
  source += "package main\n\n";
  source +=
      "import (\n\t\"encoding/json\"\n\t\"fmt\"\n\t\"math\"\n\t\"os\"\n\t\"reflect\"\n\t\"strconv\""
      "\n\t\"strings\"\n\t\"time\"\n\n\tcppanoriginal ";
  source += go_quote(harness->import);
  source += "\n)\n\n";
  source += "var cppanFunctions = map[string]interface{}{\n";
  for (const Function& function : project.functions) {
    source += '\t';
    source += go_quote(function.name);
    source += ": cppanoriginal.";
    source += function.name;
    source += ",\n";
  }
  source += "}\n\n";
  source += "var cppanBench = []string{\n";
  for (const std::string& call : calls) {
    source += '\t';
    source += go_quote(call);
    source += ",\n";
  }
  source += "}\n\n";
  source += "const cppanRepeat = " + std::to_string(repeat) + "\n";
  source += kGoHarnessHelpers;
  source += kBenchMain;

  std::uint64_t rss_kb = 0;
  auto output = execute(project, std::move(*harness), source, &rss_kb);
  if (!output) return std::unexpected(output.error());
  Json doc;
  try {
    doc = cppan::parse(trim(*output));
  } catch (const JsonError& error) {
    return std::unexpected(error.what());
  }
  const Json* nanos = doc.find("ns");
  const Json* calls_field = doc.find("calls");
  const double total = nanos != nullptr && nanos->is_number() ? nanos->as_number() : 0.0;
  const double count = calls_field != nullptr && calls_field->is_number()
                           ? calls_field->as_number()
                           : 1.0;
  if (count <= 0.0) return std::unexpected("the Go benchmark ran no calls");
  return BenchMeasurement{total / count, rss_kb};
}

namespace {

std::string param_name(const GoParam& param, std::size_t index) {
  if (param.name.has_value() && !param.name->empty() && *param.name != "_") {
    return *param.name;
  }
  return "arg" + std::to_string(index);
}

std::string render_wrapper(const Function& function, const GoFunction& signature) {
  std::string args;
  std::vector<std::string> names;
  for (std::size_t index = 0; index < signature.params.size(); ++index) {
    std::string name = param_name(signature.params[index], index);
    if (!args.empty()) args += ", ";
    args += name + " " + signature.params[index].ty;
    names.push_back(std::move(name));
  }
  std::string values;
  for (const std::string& name : names) {
    if (!values.empty()) values += ", ";
    values += name;
  }
  const std::string call = "cppanoriginal." + function.name + "(" + values + ")";
  std::string out;
  out += "\n// " + function.name +
         " runs the original package's function first; the port observes it.\n";
  if (signature.results.empty()) {
    out += "func " + function.name + "(" + args + ") {\n";
    out += "\t_cppanArgs := []interface{}{" + values + "}\n";
    out += "\tdefer cppanPanic(" + go_quote(function.name) + ", _cppanArgs)\n";
    out += "\t" + call + "\n";
    out += "\tcppanObserve(" + go_quote(function.name) + ", _cppanArgs, nil)\n";
    out += "}\n";
  } else {
    const std::string& result = signature.results.front().ty;
    out += "func " + function.name + "(" + args + ") " + result + " {\n";
    out += "\t_cppanArgs := []interface{}{" + values + "}\n";
    out += "\tdefer cppanPanic(" + go_quote(function.name) + ", _cppanArgs)\n";
    out += "\t_cppanResult := " + call + "\n";
    out += "\tcppanObserve(" + go_quote(function.name) +
           ", _cppanArgs, _cppanResult)\n";
    out += "\treturn _cppanResult\n";
    out += "}\n";
  }
  return out;
}

constexpr std::string_view kShadowTemplate = R"CPPAN(// Generated by cpp-anything. DO NOT EDIT.
//
// The original Go function runs first; the C++ port runs second through the C
// ABI in build/libcppan_shadow.so. Callers see exactly the original's value or
// panic; disagreements are appended to shadow/@@NS@@.shadow.jsonl.
package cppanshadow_@@NS@@

/*
#cgo LDFLAGS: -L${SRCDIR}/../build -lcppan_shadow -Wl,-rpath,${SRCDIR}/../build
#include <stdlib.h>

const char* cppan_shadow_call(const char* fn, const char* args_json);
void cppan_shadow_free(const char* ptr);
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"time"
	"unsafe"

	@@IMPORT_LINE@@
)

const cppanLogPath = "@@LOG@@"

// Error is what Call returns when the port fails.
type Error struct {
	Type string `json:"type"`
	Msg  string `json:"msg"`
}

func (e *Error) Error() string {
	if e == nil {
		return "shadow error"
	}
	return e.Type + ": " + e.Msg
}

// Call runs the C++ port through the C ABI. It returns the port's value, or an
// *Error carrying the port's error type (messages are not compared).
func Call(fn string, args ...interface{}) (interface{}, error) {
	payload, err := json.Marshal(args)
	if err != nil {
		return nil, &Error{Type: "EncodingError", Msg: err.Error()}
	}
	cName := C.CString(fn)
	defer C.free(unsafe.Pointer(cName))
	cArgs := C.CString(string(payload))
	defer C.free(unsafe.Pointer(cArgs))
	pointer := C.cppan_shadow_call(cName, cArgs)
	if pointer == nil {
		return nil, &Error{Type: "ShadowError", Msg: "the port returned nothing"}
	}
	text := C.GoString(pointer)
	C.cppan_shadow_free(pointer)
	var envelope struct {
		Ok  json.RawMessage `json:"ok"`
		Err *Error          `json:"err"`
	}
	if err := json.Unmarshal([]byte(text), &envelope); err != nil {
		return nil, &Error{Type: "ShadowError", Msg: "the port returned invalid JSON: " + err.Error()}
	}
	if envelope.Err != nil {
		return nil, envelope.Err
	}
	if envelope.Ok == nil {
		return nil, nil
	}
	var value interface{}
	decoder := json.NewDecoder(strings.NewReader(string(envelope.Ok)))
	decoder.UseNumber()
	if err := decoder.Decode(&value); err != nil {
		return nil, &Error{Type: "ShadowError", Msg: err.Error()}
	}
	return value, nil
}

func cppanPanic(fn string, args []interface{}) {
	if recovered := recover(); recovered != nil {
		cppanObservePanic(fn, args, recovered)
		panic(recovered)
	}
}

func cppanObserve(fn string, args []interface{}, value interface{}) {
	if os.Getenv("CPPAN_SHADOW") == "off" {
		return
	}
	expected := cppanExpected(value, nil, false)
	got := cppanCall(fn, args)
	if reason := cppanCompare(expected, got); reason != "" {
		cppanLog(fn, args, expected, got, reason)
	}
}

func cppanObservePanic(fn string, args []interface{}, recovered interface{}) {
	if os.Getenv("CPPAN_SHADOW") == "off" {
		return
	}
	expected := cppanExpected(nil, recovered, true)
	got := cppanCall(fn, args)
	if reason := cppanCompare(expected, got); reason != "" {
		cppanLog(fn, args, expected, got, reason)
	}
}

func cppanExpected(value interface{}, panicValue interface{}, panicked bool) map[string]interface{} {
	if panicked {
		return map[string]interface{}{
			"err": map[string]interface{}{
				"type": "panic",
				"msg":  cppanTrim(fmt.Sprintf("%T: %v", panicValue, panicValue)),
			},
		}
	}
	encoded, failure := cppanEncode(value)
	if failure != nil {
		return map[string]interface{}{"err": map[string]interface{}{"type": failure.Type, "msg": failure.Msg}}
	}
	text, err := json.Marshal(encoded)
	if err != nil {
		return map[string]interface{}{"err": map[string]interface{}{"type": "EncodingError", "msg": err.Error()}}
	}
	var normalized interface{}
	decoder := json.NewDecoder(strings.NewReader(string(text)))
	decoder.UseNumber()
	if err := decoder.Decode(&normalized); err != nil {
		return map[string]interface{}{"err": map[string]interface{}{"type": "EncodingError", "msg": err.Error()}}
	}
	return map[string]interface{}{"ok": normalized}
}

func cppanCall(fn string, args []interface{}) map[string]interface{} {
	value, err := Call(fn, args...)
	if err == nil {
		return map[string]interface{}{"ok": value}
	}
	if failure, ok := err.(*Error); ok {
		return map[string]interface{}{"err": map[string]interface{}{"type": failure.Type, "msg": failure.Msg}}
	}
	return map[string]interface{}{"err": map[string]interface{}{"type": "ShadowError", "msg": err.Error()}}
}

func cppanCompare(expected, got map[string]interface{}) string {
	expectedError, expectedFailed := expected["err"]
	gotError, gotFailed := got["err"]
	if expectedFailed || gotFailed {
		if !expectedFailed {
			return "the original returns a value, the port raises " + cppanErrorType(gotError)
		}
		if !gotFailed {
			return "the original raises, the port returns a value"
		}
		left := cppanErrorType(expectedError)
		right := cppanErrorType(gotError)
		if left != right {
			return "error type: expected " + left + ", got " + right
		}
		return ""
	}
	if !reflect.DeepEqual(expected["ok"], got["ok"]) {
		return fmt.Sprintf("values differ: expected %v, got %v", expected["ok"], got["ok"])
	}
	return ""
}

func cppanErrorType(value interface{}) string {
	object, ok := value.(map[string]interface{})
	if !ok {
		return "?"
	}
	text, _ := object["type"].(string)
	if text == "" {
		return "?"
	}
	return text
}

func cppanLog(fn string, args []interface{}, expected, got map[string]interface{}, reason string) {
	encoded := make([]interface{}, 0, len(args))
	for _, arg := range args {
		value, failure := cppanEncode(arg)
		if failure != nil {
			value = map[string]interface{}{"$opaque": fmt.Sprintf("%T", arg)}
		}
		encoded = append(encoded, value)
	}
	path := os.Getenv("CPPAN_SHADOW_LOG")
	if path == "" {
		path = cppanLogPath
	}
	if directory := filepath.Dir(path); directory != "" {
		_ = os.MkdirAll(directory, 0o755)
	}
	entry := map[string]interface{}{
		"at":       time.Now().UTC().Format("2006-01-02T15:04:05Z"),
		"fn":       fn,
		"args":     encoded,
		"expected": expected,
		"got":      got,
		"reason":   reason,
	}
	text, err := json.Marshal(entry)
	if err != nil {
		return
	}
	file, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	defer file.Close()
	_, _ = file.Write(append(text, '\n'))
}

func cppanEncode(value interface{}) (interface{}, *Error) {
	return cppanEncodeValue(reflect.ValueOf(value))
}

func cppanEncodeValue(value reflect.Value) (interface{}, *Error) {
	if !value.IsValid() {
		return nil, nil
	}
	for value.Kind() == reflect.Interface || value.Kind() == reflect.Ptr {
		if value.IsNil() {
			return nil, nil
		}
		value = value.Elem()
	}
	switch value.Kind() {
	case reflect.Bool:
		return value.Bool(), nil
	case reflect.String:
		return value.String(), nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return value.Int(), nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		number := value.Uint()
		if number > math.MaxInt64 {
			return nil, &Error{Type: "RangeError", Msg: fmt.Sprintf("%d does not fit int64", number)}
		}
		return int64(number), nil
	case reflect.Float32, reflect.Float64:
		number := value.Float()
		if math.IsNaN(number) || math.IsInf(number, 0) {
			return nil, &Error{Type: "RangeError", Msg: "non-finite float"}
		}
		return number, nil
	case reflect.Slice, reflect.Array:
		items := make([]interface{}, value.Len())
		for index := 0; index < value.Len(); index++ {
			encoded, failure := cppanEncodeValue(value.Index(index))
			if failure != nil {
				return nil, failure
			}
			items[index] = encoded
		}
		return items, nil
	case reflect.Map:
		if value.Type().Key().Kind() != reflect.String {
			return nil, &Error{Type: "TypeError", Msg: "map keys must be strings"}
		}
		object := make(map[string]interface{}, value.Len())
		iterator := value.MapRange()
		for iterator.Next() {
			encoded, failure := cppanEncodeValue(iterator.Value())
			if failure != nil {
				return nil, failure
			}
			object[iterator.Key().String()] = encoded
		}
		return object, nil
	default:
		return map[string]interface{}{"$opaque": value.Type().String()}, nil
	}
}

func cppanTrim(text string) string {
	if len(text) > 200 {
		return text[:200]
	}
	return text
}

@@WRAPPERS@@

@@ANCHOR_LINE@@
)CPPAN";

constexpr std::string_view kShadowBuild = R"CPPAN(#!/bin/sh
# cpp-anything shadow: cgo links the port at `go build` time.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
lib="$here/../build/libcppan_shadow.so"
if [ ! -f "$lib" ]; then
  echo "shadow: $lib is missing; run: cpp-anything build <project>" >&2
  exit 1
fi
echo "shadow: ready ($lib)"
)CPPAN";

std::map<std::string, GoFunction> read_signatures(
    const std::filesystem::path& package_dir) {
  std::map<std::string, GoFunction> signatures;
  const auto files = go_files(package_dir);
  if (!files) return signatures;
  for (const auto& file : *files) {
    const auto source = read_file(file);
    if (!source.has_value()) continue;
    for (GoFunction& function : scan_functions(*source)) {
      if (function.receiver || !is_exported(function.name)) continue;
      signatures.insert_or_assign(function.name, std::move(function));
    }
  }
  return signatures;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> shadow_files(const Project& project,
                                                              std::string_view ns) {
  std::string import = "cppanoriginal";
  if (auto resolved = import_path(project.source_file); resolved.has_value()) {
    import = *resolved;
  }
  std::error_code error;
  std::map<std::string, GoFunction> signatures;
  if (std::filesystem::is_directory(project.source_file, error)) {
    signatures = read_signatures(project.source_file);
  }
  const std::string log = shadow_log_path(project.dir, project.unit).string();
  std::string wrappers;
  for (const Function& function : project.functions) {
    if (function.name == "Call" || function.name == "Error") {
      wrappers += "// " + function.name +
                  ": the generated binding already uses that name; no wrapper was "
                  "generated.\n";
      continue;
    }
    const auto found = signatures.find(function.name);
    if (found == signatures.end()) {
      wrappers += "// " + function.name +
                  ": not found in the package source; no wrapper was generated.\n";
      continue;
    }
    if (found->second.params.size() != function.params.size() ||
        found->second.results.size() > 1) {
      wrappers += "// " + function.name +
                  ": the source signature changed shape; run `cpp-anything refresh`.\n";
      continue;
    }
    wrappers += render_wrapper(function, found->second);
  }
  std::string anchor;
  if (!project.functions.empty()) {
    anchor = "var _ = cppanoriginal." + project.functions.front().name;
  }
  const std::string import_line =
      anchor.empty() ? "_ " + go_quote(import) : "cppanoriginal " + go_quote(import);
  std::string text(kShadowTemplate);
  text = replace_all(std::move(text), "@@NS@@", ns);
  text = replace_all(std::move(text), "@@IMPORT_LINE@@", import_line);
  text = replace_all(std::move(text), "@@LOG@@", log);
  text = replace_all(std::move(text), "@@WRAPPERS@@", wrappers);
  text = replace_all(std::move(text), "@@ANCHOR_LINE@@", anchor);
  return {
      {"shadow/" + std::string(ns) + "_shadow.go", text},
      {"shadow/build.sh", std::string(kShadowBuild)},
  };
}

}  // namespace cppan::langs::go
