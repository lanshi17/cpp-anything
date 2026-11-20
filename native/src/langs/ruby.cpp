// Ruby: top-level functions (and module functions) with arguments the wire can
// carry. The recorder runs the original with `ruby`, the shadow module calls the
// C++ port through Fiddle (no compiler needed in the Ruby process).
#include <algorithm>
#include <cctype>

#include "cppan/langs.hpp"

namespace cppan::langs::ruby {
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

std::string_view trim_start(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  return text;
}

std::string_view trim_end(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

bool is_constant(std::string_view name) {
  if (name.empty()) return false;
  const char ch = name.front();
  return std::isupper(static_cast<unsigned char>(ch)) || ch == '_';
}

bool starts_with(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }

std::optional<Json> ruby_literal(std::string_view text) {
  if (text == "nil") return Json::null();
  if (text == "true") return Json::boolean(true);
  if (text == "false") return Json::boolean(false);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    return Json::string(std::string(text.substr(1, text.size() - 2)));
  }
  if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'') {
    return Json::string(std::string(text.substr(1, text.size() - 2)));
  }
  try {
    std::size_t used = 0;
    const std::string value(text);
    const std::int64_t number = std::stoll(value, &used);
    if (used == value.size()) return Json::integer(number);
  } catch (const std::exception&) {
  }
  try {
    std::size_t used = 0;
    const std::string value(text);
    const double number = std::stod(value, &used);
    if (used == value.size()) return Json::real(number);
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

struct DefResult {
  bool is_self = false;
  std::string name;
  std::vector<std::string> params;
  std::vector<std::optional<Json>> defaults;
};

Result<DefResult> parse_def(std::string_view rest) {
  rest = trim_end(rest);
  std::string_view head = rest;
  std::optional<std::string_view> tail;
  if (const std::size_t open = rest.find('('); open != std::string_view::npos) {
    head = trim(rest.substr(0, open));
    tail = rest.substr(open + 1);
  } else {
    head = rest;
  }
  const bool is_self = starts_with(head, "self.");
  std::string name(trim(starts_with(head, "self.") ? head.substr(5) : head));
  if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) ||
                        name.front() == '_')) {
    return std::unexpected("\"" + name + "\" is not a plain method name");
  }
  if (name.ends_with('=')) {
    return std::unexpected("setters return the argument, not a value");
  }
  DefResult out;
  out.is_self = is_self;
  out.name = name;
  if (!tail.has_value()) return out;

  std::string_view params_text;
  const std::size_t close = tail->rfind(')');
  if (close == std::string_view::npos) {
    return std::unexpected("an unclosed parameter list");
  }
  params_text = tail->substr(0, close);
  const std::string_view after = trim(tail->substr(close + 1));
  if (!after.empty() && !after.starts_with('#')) {
    return std::unexpected("`" + std::string(after) + "` after the parameter list");
  }
  params_text = trim(params_text);
  if (params_text.empty()) return out;

  for (std::string_view part : split_top_level(params_text, ',')) {
    part = trim(part);
    if (part.empty()) continue;
    if (part.starts_with('*') || part.starts_with('&')) {
      return std::unexpected("splat, keyword and block parameters have no wire form");
    }
    if (part.find(':') != std::string_view::npos &&
        part.find("=>") == std::string_view::npos) {
      return std::unexpected("keyword parameters have no wire form");
    }
    std::string param_name;
    std::optional<Json> default_value;
    if (const std::size_t equals = part.find('='); equals != std::string_view::npos) {
      param_name = std::string(trim(part.substr(0, equals)));
      const std::string_view literal = trim(part.substr(equals + 1));
      default_value = ruby_literal(literal);
      if (!default_value.has_value()) {
        return std::unexpected("the default `" + std::string(literal) +
                               "` is not a literal the wire carries");
      }
    } else {
      param_name = std::string(part);
    }
    if (param_name.empty() || !(std::isalpha(static_cast<unsigned char>(param_name.front())) ||
                               param_name.front() == '_')) {
      return std::unexpected("\"" + param_name + "\" is not a plain parameter name");
    }
    out.params.push_back(param_name);
    out.defaults.push_back(std::move(default_value));
  }
  return out;
}

std::string doc_comment(const std::string& source, std::size_t index) {
  const auto lines = split_lines(source);
  std::vector<std::string> out;
  std::size_t at = index;
  while (at > 0) {
    --at;
    const std::string_view line = trim(lines[at]);
    if (!line.starts_with('#')) break;
    const std::string_view text = trim(line.substr(1));
    if (starts_with(text, " frozen_string_literal")) break;
    out.emplace_back(text);
  }
  std::ranges::reverse(out);
  while (!out.empty() && out.front().empty()) out.erase(out.begin());
  std::string joined;
  for (const std::string& line : out) {
    if (!joined.empty()) joined += ' ';
    joined += line;
  }
  return joined;
}

}  // namespace

Result<Analysis> analyze(const std::filesystem::path& path) {
  const auto source = read_file(path);
  if (!source.has_value()) return std::unexpected(path.string() + ": cannot read");
  Analysis analysis;
  analysis.lines = static_cast<std::uint32_t>(line_count(*source));

  struct Container {
    std::size_t indent;
    std::string name;
    bool module_function;
  };
  std::vector<Container> containers;
  const auto lines = split_lines(*source);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    const std::string_view trimmed = trim_start(line);
    const std::size_t indent = line.size() - trimmed.size();
    if (trimmed.starts_with('#') || trimmed.empty()) continue;
    if (trimmed.starts_with("require_relative ") || trimmed.starts_with("require ")) {
      std::string_view rest = trimmed.starts_with("require_relative ")
                                  ? trimmed.substr(17)
                                  : trimmed.substr(8);
      rest = trim(rest);
      while (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
        rest.remove_prefix(1);
      }
      while (!rest.empty() && (rest.back() == '"' || rest.back() == '\'')) {
        rest.remove_suffix(1);
      }
      analysis.imports.emplace_back(rest);
      continue;
    }
    // Leaving blocks: anything at this indent closes containers deeper than it.
    while (!containers.empty() && containers.back().indent >= indent) containers.pop_back();
    if (trimmed.starts_with("module ")) {
      std::string_view rest = trim(trimmed.substr(7));
      if (rest.ends_with(';')) rest.remove_suffix(1);
      rest = trim(rest);
      if (is_constant(rest)) containers.push_back({indent, std::string(rest), false});
      continue;
    }
    if (trimmed.starts_with("class ")) {
      std::string_view rest = trim(trimmed.substr(6));
      if (rest.ends_with(';')) rest.remove_suffix(1);
      rest = trim(rest);
      containers.push_back({indent, "class:" + std::string(rest), false});
      continue;
    }
    if (trimmed == "module_function") {
      if (!containers.empty()) containers.back().module_function = true;
      continue;
    }
    if (!trimmed.starts_with("def ")) continue;

    const std::string_view rest = trimmed.substr(4);
    auto parsed = parse_def(rest);
    if (!parsed) {
      std::string skipped_name(rest);
      const std::size_t stop = skipped_name.find_first_of("( ");
      if (stop != std::string::npos) skipped_name.resize(stop);
      analysis.skipped.emplace_back(skipped_name, parsed.error());
      continue;
    }
    const DefResult& def = *parsed;
    std::string call;
    if (containers.empty()) {
      if (def.is_self) {
        analysis.skipped.emplace_back(def.name, "a singleton method outside a module");
        continue;
      }
      call = "TOPLEVEL";
    } else {
      const Container& container = containers.back();
      if (container.name.starts_with("class:")) {
        analysis.skipped.emplace_back(
            def.name, "a class method: the recorder does not construct an instance");
        continue;
      }
      if (!def.is_self && !container.module_function) {
        analysis.skipped.emplace_back(
            def.name,
            "an instance method of a module: define it as `def self.` or use `module_function`");
        continue;
      }
      call = container.name;
    }

    Found found;
    found.name = def.name;
    found.line = static_cast<std::uint32_t>(index + 1);
    found.call = call;
    for (std::size_t param_index = 0; param_index < def.params.size(); ++param_index) {
      FoundParam param;
      param.name = def.params[param_index];
      param.ty = "Any";
      if (param_index < def.defaults.size()) {
        param.has_default = def.defaults[param_index].has_value();
        param.default_value = def.defaults[param_index];
      }
      found.params.push_back(std::move(param));
    }
    found.returns = "Any";
    found.doc = doc_comment(*source, index);
    analysis.functions.push_back(std::move(found));
  }
  return analysis;
}

}  // namespace cppan::langs::ruby
