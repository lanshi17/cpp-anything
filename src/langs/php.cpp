// PHP is whole-app only: the analyzer lists top-level functions so `analyze
// --whole-app` can plan an app, and `bridge`/`record` refuse per-unit ports.
#include <algorithm>

#include "cppan/langs.hpp"

namespace cppan::langs::php {
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

std::string php_type(std::string_view text) {
  std::string lowered(text);
  std::ranges::transform(lowered, lowered.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (lowered == "string") return "str";
  if (lowered == "int" || lowered == "integer") return "int";
  if (lowered == "float" || lowered == "double") return "float";
  if (lowered == "bool" || lowered == "boolean") return "bool";
  if (lowered == "void") return "None";
  if (lowered.starts_with("array")) return "list[str]";
  return "Any";
}

/// Splits the text after `(` into parameter texts and the tail after `)`.
std::expected<std::pair<std::vector<std::string>, std::string_view>,
              std::pair<std::string, std::string>>
split_params(std::string_view after) {
  std::vector<std::string> params;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t index = 0; index < after.size(); ++index) {
    const char ch = after[index];
    if (ch == '(' || ch == '[') {
      ++depth;
    } else if (ch == ')' || ch == ']') {
      if (depth == 0 && ch == ')') {
        params.emplace_back(after.substr(start, index - start));
        return std::pair{std::move(params), after.substr(index + 1)};
      }
      --depth;
    } else if (ch == ',' && depth == 0) {
      params.emplace_back(after.substr(start, index - start));
      start = index + 1;
    }
  }
  return std::unexpected(std::pair{std::string("function"),
                                   std::string("an unclosed parameter list")});
}

std::expected<std::tuple<std::string, std::vector<FoundParam>, std::string>,
              std::pair<std::string, std::string>>
parse_function(std::string_view rest) {
  const std::size_t open = rest.find('(');
  if (open == std::string_view::npos) {
    return std::unexpected(
        std::pair{std::string("function"), std::string("a function without a parameter list")});
  }
  std::string name(trim(rest.substr(0, open)));
  auto split = split_params(rest.substr(open + 1));
  if (!split) {
    const auto& failure = split.error();
    return std::unexpected(std::pair{name, failure.second});
  }
  auto [params, tail] = std::move(*split);

  std::string returns = "Any";
  std::string_view annotation_text = trim(tail);
  if (annotation_text.starts_with(':')) {
    annotation_text.remove_prefix(1);
    const std::size_t stop = annotation_text.find_first_of("{;");
    annotation_text = trim(annotation_text.substr(0, stop));
    while (annotation_text.starts_with('?')) annotation_text.remove_prefix(1);
    returns = php_type(annotation_text);
  }

  std::vector<FoundParam> out;
  for (const std::string& param : params) {
    std::string_view text = trim(param);
    if (text.starts_with("...") || text.starts_with('&')) {
      return std::unexpected(std::pair{name, std::string(
                                                   "by-reference and variadic parameters have no "
                                                   "wire form")});
    }
    while (text.starts_with('&')) text.remove_prefix(1);
    std::string type = "Any";
    std::string_view rest_text = text;
    const std::size_t space = text.find_first_of(" \t\n\r");
    if (space != std::string_view::npos) {
      const std::string_view head = text.substr(0, space);
      if (head.find('$') == std::string_view::npos && head.find('=') == std::string_view::npos) {
        std::string_view bare = head;
        while (bare.starts_with('?')) bare.remove_prefix(1);
        type = php_type(bare);
        rest_text = trim(text.substr(space + 1));
      }
    }
    std::string_view var = rest_text;
    std::optional<std::string> default_value;
    if (const std::size_t equals = rest_text.find('='); equals != std::string_view::npos) {
      var = trim(rest_text.substr(0, equals));
      default_value = std::string(trim(rest_text.substr(equals + 1)));
    }
    while (var.starts_with('$')) var.remove_prefix(1);
    var = trim(var);
    if (var.empty() || !(std::isalpha(static_cast<unsigned char>(var.front())) ||
                         var.front() == '_')) {
      return std::unexpected(
          std::pair{std::string(var), std::string("a parameter without a plain name")});
    }
    out.push_back(FoundParam{std::string(var), type, default_value.has_value(), std::nullopt});
  }
  return std::tuple{name, std::move(out), returns};
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
    if (trimmed.starts_with("require") || trimmed.starts_with("include")) {
      analysis.imports.emplace_back(trimmed);
    }
    if (trimmed.starts_with("//") || trimmed.starts_with('#')) continue;
    if (trimmed.starts_with("function ")) {
      const std::string_view rest = trimmed.substr(9);
      if (depth != 0) {
        const std::size_t open = rest.find('(');
        analysis.skipped.emplace_back(std::string(rest.substr(0, open)),
                                      "a nested or method function: PHP classes are whole-app "
                                      "only");
        continue;
      }
      auto parsed = parse_function(rest);
      if (parsed) {
        auto& [name, params, returns] = *parsed;
        analysis.functions.push_back(Found{name, static_cast<std::uint32_t>(index + 1),
                                           "php_" + std::to_string(index), std::move(params),
                                           returns, ""});
      } else {
        analysis.skipped.push_back(parsed.error());
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

}  // namespace cppan::langs::php
