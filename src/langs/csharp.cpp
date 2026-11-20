// C# is whole-app only: the analyzer lists public static methods so
// `analyze --whole-app` can plan an app; `bridge`/`record` refuse per-unit ports.
#include <algorithm>
#include <cctype>

#include "cppan/langs.hpp"

namespace cppan::langs::csharp {
namespace {

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

bool starts_with(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }

std::string_view strip_prefix(std::string_view text, std::string_view prefix) {
  return text.starts_with(prefix) ? text.substr(prefix.size()) : text;
}

std::string csharp_type(std::string_view text) {
  text = trim(text);
  if (text == "string" || text == "String") return "str";
  if (text == "int" || text == "Int32" || text == "long" || text == "Int64" ||
      text == "short" || text == "byte") {
    return "int";
  }
  if (text == "double" || text == "Double" || text == "float" || text == "Single") return "float";
  if (text == "bool" || text == "Boolean") return "bool";
  if (text == "void") return "None";
  if (text == "List<string>" || text == "IList<string>" || text == "IReadOnlyList<string>" ||
      text == "string[]") {
    return "list[str]";
  }
  if (text == "List<int>" || text == "int[]") return "list[int]";
  if (text == "Dictionary<string,string>" || text == "Dictionary<string, string>") {
    return "dict[str]";
  }
  return "Any";
}

std::vector<std::string_view> split_words(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
    if (index >= text.size()) break;
    const std::size_t start = index;
    while (index < text.size() && !std::isspace(static_cast<unsigned char>(text[index]))) ++index;
    words.push_back(text.substr(start, index - start));
  }
  return words;
}

std::expected<std::pair<std::vector<std::string>, std::string_view>,
              std::pair<std::string, std::string>>
split_params(std::string_view after) {
  std::vector<std::string> params;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t index = 0; index < after.size(); ++index) {
    const char ch = after[index];
    if (ch == '(' || ch == '<' || ch == '[') {
      ++depth;
    } else if (ch == ')' || ch == ']') {
      if (ch == ')' && depth == 0) {
        params.emplace_back(after.substr(start, index - start));
        return std::pair{std::move(params), after.substr(index + 1)};
      }
      --depth;
    } else if (ch == ',' && depth == 0) {
      params.emplace_back(after.substr(start, index - start));
      start = index + 1;
    }
  }
  return std::unexpected(std::pair{std::string("method"),
                                   std::string("an unclosed parameter list")});
}

std::optional<std::string> class_method(std::string_view line) {
  if (line.find('(') == std::string_view::npos || line.find(" class ") != std::string_view::npos ||
      starts_with(line, "class ")) {
    return std::nullopt;
  }
  const std::string_view head = trim(line.substr(0, line.find('(')));
  const auto words = split_words(head);
  if (words.empty()) return std::nullopt;
  const std::string name(words.back());
  if (!(line.ends_with('{') || line.find('{') != std::string_view::npos)) return std::nullopt;
  return name;
}

std::expected<std::optional<std::tuple<std::string, std::vector<FoundParam>, std::string>>,
              std::pair<std::string, std::string>>
parse_method(std::string_view line) {
  std::string_view text = strip_prefix(line, "public static ");
  text = strip_prefix(text, "static ");
  const std::size_t open = text.find('(');
  if (open == std::string_view::npos) return std::optional<std::tuple<std::string, std::vector<FoundParam>, std::string>>{};
  const std::string_view head = text.substr(0, open);
  auto words = split_words(head);
  if (words.size() < 2) return std::optional<std::tuple<std::string, std::vector<FoundParam>, std::string>>{};
  const std::string name(words.back());
  words.pop_back();
  if (name.find('<') != std::string::npos) {
    return std::unexpected(std::pair{name, std::string("generic methods have no wire form")});
  }
  std::string joined;
  for (const std::string_view word : words) {
    if (!joined.empty()) joined += ' ';
    joined += word;
  }
  const std::string returns = csharp_type(joined);
  if (returns == "None") return std::optional<std::tuple<std::string, std::vector<FoundParam>, std::string>>{};
  auto split = split_params(text.substr(open + 1));
  if (!split) {
    const auto& failure = split.error();
    return std::unexpected(std::pair{name, failure.second});
  }
  std::vector<FoundParam> params;
  for (const std::string& param : split->first) {
    const std::string_view text_param = trim(param);
    if (text_param.empty()) continue;
    if (starts_with(text_param, "params ") || starts_with(text_param, "ref ") ||
        starts_with(text_param, "out ")) {
      return std::unexpected(
          std::pair{name, std::string("ref/out/params parameters have no wire form")});
    }
    std::string_view decl = text_param;
    bool has_default = false;
    if (const std::size_t equals = text_param.find('='); equals != std::string_view::npos) {
      decl = trim(text_param.substr(0, equals));
      has_default = true;
    }
    auto parts = split_words(decl);
    if (parts.size() < 2) {
      return std::unexpected(std::pair{name, std::string("a parameter without a type")});
    }
    const std::string var(parts.back());
    parts.pop_back();
    std::string type_text;
    for (const std::string_view word : parts) {
      if (!type_text.empty()) type_text += ' ';
      type_text += word;
    }
    params.push_back(FoundParam{var, csharp_type(type_text), has_default, std::nullopt});
  }
  return std::optional<std::tuple<std::string, std::vector<FoundParam>, std::string>>{
      std::tuple{name, std::move(params), returns}};
}

}  // namespace

Result<Analysis> analyze(const std::filesystem::path& path) {
  const auto source = read_file(path);
  if (!source.has_value()) return std::unexpected(path.string() + ": cannot read");
  Analysis analysis;
  analysis.lines = static_cast<std::uint32_t>(line_count(*source));
  int depth = 0;
  const auto lines = split_lines(*source);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view trimmed = trim(lines[index]);
    if (starts_with(trimmed, "using ") && trimmed.ends_with(';')) {
      std::string import(trimmed);
      import = import.substr(6);
      import.pop_back();
      analysis.imports.push_back(std::move(import));
      continue;
    }
    if (starts_with(trimmed, "//") || starts_with(trimmed, "[")) continue;
    if (depth == 1 && (starts_with(trimmed, "public static ") || starts_with(trimmed, "static "))) {
      auto parsed = parse_method(trimmed);
      if (!parsed) {
        const auto& failure = parsed.error();
        analysis.skipped.push_back(failure);
      } else if (parsed->has_value()) {
        auto& [name, params, returns] = **parsed;
        analysis.functions.push_back(Found{name, static_cast<std::uint32_t>(index + 1),
                                           "csharp_" + std::to_string(index), std::move(params),
                                           returns, ""});
      }
    } else if (depth == 1) {
      if (const auto name = class_method(trimmed); name.has_value()) {
        analysis.skipped.emplace_back(
            *name, "an instance method: the recorder calls public static methods only");
      }
    }
    for (const char ch : trimmed) {
      if (ch == '{') ++depth;
      if (ch == '}') --depth;
    }
    if (depth < 0) depth = 0;
  }
  return analysis;
}

}  // namespace cppan::langs::csharp
