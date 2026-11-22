// Rust: free functions from a library crate whose parameters and return value
// cross the wire. The recorder builds a small Cargo harness that depends on the
// original crate and calls the functions; the shadow binding is a Rust crate over
// the same C ABI, with a build script that links the C++ shadow library.
//
// Rust has no reflection, so the supported subset is declared in the signature:
// integers, floats, bool, String/&str, Vec<T>/&[T], Option<T> and
// HashMap<String, T>. Everything else (generics, traits, impl methods, async,
// Result, references to owned non-slice types) is skipped with a reason.
module;
#include <cctype>
#include <cmath>

module cppan.langs;
import cppan.json;
import cppan.types;
import cppan.util;
import cppan.inventory;
import cppan.project;
import std;

namespace cppan::langs::rust {
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

std::string compact(std::string_view text) {
  std::string out;
  for (const char ch : text) {
    if (!std::isspace(static_cast<unsigned char>(ch))) out += ch;
  }
  return out;
}

bool ident_start(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool ident_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

std::size_t line_of(std::string_view source, std::size_t at) {
  std::size_t line = 1;
  for (std::size_t index = 0; index < at && index < source.size(); ++index) {
    if (source[index] == '\n') ++line;
  }
  return line;
}

std::string replace_all(std::string text, std::string_view needle, std::string_view value) {
  std::size_t at = 0;
  while (!needle.empty() && (at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), value);
    at += value.size();
  }
  return text;
}

// --- a small Rust lexer -------------------------------------------------------

struct Token {
  std::string_view text;
  std::size_t at = 0;
};

std::vector<Token> lex(std::string_view source) {
  std::vector<Token> out;
  std::size_t index = 0;
  while (index < source.size()) {
    const char ch = source[index];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++index;
      continue;
    }
    if (source.substr(index).starts_with("//")) {
      while (index < source.size() && source[index] != '\n') ++index;
      continue;
    }
    if (source.substr(index).starts_with("/*")) {
      int depth = 1;
      index += 2;
      while (index < source.size() && depth > 0) {
        if (source.substr(index).starts_with("/*")) {
          ++depth;
          index += 2;
        } else if (source.substr(index).starts_with("*/")) {
          --depth;
          index += 2;
        } else {
          ++index;
        }
      }
      continue;
    }
    if (ch == '"') {
      ++index;
      while (index < source.size()) {
        if (source[index] == '\\') {
          index += 2;
          continue;
        }
        if (source[index] == '"') {
          ++index;
          break;
        }
        ++index;
      }
      continue;
    }
    if (ch == '\'') {
      // A char literal is `'x'` or `'\n'`; anything else is a lifetime.
      if (index + 1 < source.size() && source[index + 1] == '\\') {
        index += 2;
        while (index < source.size() && source[index] != '\'') ++index;
        if (index < source.size()) ++index;
      } else if (index + 2 < source.size() && source[index + 2] == '\'') {
        index += 3;
      } else {
        ++index;
      }
      continue;
    }
    if (ident_start(ch)) {
      const std::size_t start = index;
      ++index;
      while (index < source.size() && ident_char(source[index])) ++index;
      out.push_back(Token{source.substr(start, index - start), start});
      continue;
    }
    out.push_back(Token{source.substr(index, 1), index});
    ++index;
  }
  return out;
}

std::optional<std::size_t> matching(const std::vector<Token>& tokens, std::size_t open,
                                    std::string_view open_text, std::string_view close_text) {
  if (open >= tokens.size() || tokens[open].text != open_text) return std::nullopt;
  int depth = 0;
  for (std::size_t index = open; index < tokens.size(); ++index) {
    if (tokens[index].text == open_text) {
      ++depth;
    } else if (tokens[index].text == close_text) {
      --depth;
      if (depth == 0) return index;
    }
  }
  return std::nullopt;
}

// --- types -------------------------------------------------------------------

struct RType {
  enum class Kind : std::uint8_t { Str, Int, Float, Bool, Unit, List, Dict, Optional };
  Kind kind = Kind::Str;
  std::string wire;
  std::string inner_raw;
  bool slice = false;
};

bool is_int_name(std::string_view name) {
  return name == "i8" || name == "i16" || name == "i32" || name == "i64" ||
         name == "i128" || name == "isize" || name == "u8" || name == "u16" ||
         name == "u32" || name == "u64" || name == "u128" || name == "usize";
}

std::optional<RType> type_of(std::string_view raw);

std::optional<std::pair<std::string_view, std::string_view>> generic_parts(std::string_view raw) {
  const std::size_t open = raw.find('<');
  if (open == std::string_view::npos) return std::nullopt;
  int depth = 0;
  for (std::size_t index = open; index < raw.size(); ++index) {
    if (raw[index] == '<') {
      ++depth;
    } else if (raw[index] == '>') {
      --depth;
      if (depth == 0) {
        if (!trim(raw.substr(index + 1)).empty()) return std::nullopt;
        return std::pair{trim(raw.substr(0, open)), trim(raw.substr(open + 1, index - open - 1))};
      }
    }
  }
  return std::nullopt;
}

std::vector<std::string_view> split_top_level(std::string_view text, char separator) {
  std::vector<std::string_view> out;
  int angle = 0;
  int bracket = 0;
  int paren = 0;
  std::size_t start = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == '<') {
      ++angle;
    } else if (ch == '>') {
      --angle;
    } else if (ch == '[') {
      ++bracket;
    } else if (ch == ']') {
      --bracket;
    } else if (ch == '(') {
      ++paren;
    } else if (ch == ')') {
      --paren;
    } else if (ch == separator && angle == 0 && bracket == 0 && paren == 0) {
      out.push_back(text.substr(start, index - start));
      start = index + 1;
    }
  }
  out.push_back(text.substr(start));
  return out;
}

std::optional<RType> type_of(std::string_view raw) {
  raw = trim(raw);
  if (raw.empty()) return std::nullopt;

  if (raw.front() == '&') {
    raw.remove_prefix(1);
    raw = trim(raw);
    if (raw.starts_with("'")) {
      raw.remove_prefix(1);
      while (!raw.empty() && ident_char(raw.front())) raw.remove_prefix(1);
      raw = trim(raw);
    }
    if (raw.starts_with("mut")) {
      const std::string_view rest = trim(raw.substr(3));
      if (rest.empty() || rest.front() == ' ') return std::nullopt;
      return std::nullopt;
    }
    const std::string referenced = compact(raw);
    if (referenced != "str" && !referenced.starts_with("[")) return std::nullopt;
    auto inner = type_of(raw);
    if (!inner.has_value() || inner->kind == RType::Kind::Unit) return std::nullopt;
    if (raw.starts_with("[")) inner->slice = true;
    return inner;
  }

  const std::string name = compact(raw);
  if (name == "str" || name == "String") return RType{RType::Kind::Str, "str", "", false};
  if (is_int_name(name)) return RType{RType::Kind::Int, "int", "", false};
  if (name == "f32" || name == "f64") return RType{RType::Kind::Float, "float", "", false};
  if (name == "bool") return RType{RType::Kind::Bool, "bool", "", false};
  if (name == "()") return RType{RType::Kind::Unit, "None", "", false};

  if (const auto parts = generic_parts(raw); parts.has_value()) {
    const std::string head = compact(parts->first);
    if (head == "Vec") {
      auto inner = type_of(parts->second);
      if (!inner.has_value() || inner->kind == RType::Kind::Unit) return std::nullopt;
      return RType{RType::Kind::List, "list[" + inner->wire + "]", std::string(parts->second),
                   false};
    }
    if (head == "Option") {
      auto inner = type_of(parts->second);
      if (!inner.has_value() || inner->kind == RType::Kind::Unit) return std::nullopt;
      return RType{RType::Kind::Optional, inner->wire + " | None",
                   std::string(parts->second), false};
    }
    if (head == "HashMap" || head == "BTreeMap") {
      const auto entries = split_top_level(parts->second, ',');
      if (entries.size() != 2 || compact(entries[0]) != "String") return std::nullopt;
      auto value = type_of(entries[1]);
      if (!value.has_value() || value->kind == RType::Kind::Unit) return std::nullopt;
      return RType{RType::Kind::Dict, "dict[" + value->wire + "]",
                   std::string(trim(entries[1])), false};
    }
    return std::nullopt;
  }

  if (raw.front() == '[' && raw.back() == ']') {
    const std::string_view inner_raw = trim(raw.substr(1, raw.size() - 2));
    auto inner = type_of(inner_raw);
    if (!inner.has_value() || inner->kind == RType::Kind::Unit) return std::nullopt;
    return RType{RType::Kind::List, "list[" + inner->wire + "]", std::string(inner_raw), true};
  }
  return std::nullopt;
}

std::string elide_lifetimes(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    if (text[index] == '\'' && index + 1 < text.size() && ident_start(text[index + 1])) {
      index += 2;
      while (index < text.size() && ident_char(text[index])) ++index;
      continue;
    }
    out += text[index];
    ++index;
  }
  return out;
}

// --- analysis ----------------------------------------------------------------

struct RustParam {
  std::string name;
  std::string raw;
  RType type;
};

struct RustFunction {
  std::string name;
  std::uint32_t line = 0;
  std::vector<RustParam> params;
  std::string returns_raw;
  RType returns;
  std::string call;
  std::string doc;
};

struct RustAnalysis {
  std::vector<RustFunction> functions;
  std::vector<std::pair<std::string, std::string>> skipped;
  std::vector<std::string> imports;
  std::uint32_t lines = 0;
};

std::string doc_comment(std::string_view source, std::size_t line) {
  const auto lines = split_lines(source);
  if (line == 0 || line > lines.size()) return "";
  std::vector<std::string> out;
  std::size_t at = line - 1;
  while (at > 0) {
    --at;
    const std::string_view text = trim(lines[at]);
    if (text.starts_with("///")) {
      out.emplace_back(trim(text.substr(3)));
      continue;
    }
    if (text.starts_with("#[")) continue;
    break;
  }
  std::ranges::reverse(out);
  while (!out.empty() && out.front().empty()) out.erase(out.begin());
  std::string joined;
  for (const std::string& part : out) {
    if (!joined.empty()) joined += ' ';
    joined += part;
  }
  return joined;
}

std::optional<RustAnalysis> analyze_source(std::string_view source) {
  RustAnalysis analysis;
  analysis.lines = static_cast<std::uint32_t>(line_count(source));
  for (const std::string_view line : split_lines(source)) {
    const std::string_view text = trim(line);
    if (text.starts_with("use ") && text.ends_with(';')) {
      analysis.imports.emplace_back(trim(text.substr(4, text.size() - 5)));
    }
  }

  const auto tokens = lex(source);
  int depth = 0;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const Token& token = tokens[index];
    if (token.text == "}") {
      if (depth > 0) --depth;
      continue;
    }
    if (depth == 0 && token.text == "pub") {
      std::size_t cursor = index + 1;
      if (cursor < tokens.size() && tokens[cursor].text == "(") {
        // `pub(crate)`, `pub(super)`: not reachable from the harness crate.
        continue;
      }
      bool modifier = false;
      std::string modifier_name;
      while (cursor < tokens.size() && tokens[cursor].text != "fn" &&
             tokens[cursor].text != ";" && tokens[cursor].text != "{") {
        if (tokens[cursor].text == "async" || tokens[cursor].text == "unsafe" ||
            tokens[cursor].text == "const" || tokens[cursor].text == "extern") {
          modifier = true;
          modifier_name = std::string(tokens[cursor].text);
        }
        ++cursor;
      }
      if (cursor >= tokens.size() || tokens[cursor].text != "fn") continue;
      const std::size_t fn_index = cursor;
      if (fn_index + 1 >= tokens.size() || !ident_start(tokens[fn_index + 1].text.front())) {
        continue;
      }
      const std::string name(tokens[fn_index + 1].text);
      const std::uint32_t line = static_cast<std::uint32_t>(line_of(source, token.at));
      if (modifier) {
        analysis.skipped.emplace_back(name,
                                      "`" + modifier_name + "` functions have no wire form");
        continue;
      }
      const std::size_t after_name = fn_index + 2;
      if (after_name < tokens.size() && tokens[after_name].text == "<") {
        analysis.skipped.emplace_back(name, "generic functions have no wire form");
        continue;
      }
      if (after_name >= tokens.size() || tokens[after_name].text != "(") {
        analysis.skipped.emplace_back(name, "a function without a parameter list");
        continue;
      }
      const auto close = matching(tokens, after_name, "(", ")");
      if (!close.has_value()) {
        analysis.skipped.emplace_back(name, "an unclosed parameter list");
        continue;
      }
      const std::string_view params_text =
          source.substr(tokens[after_name].at + 1, tokens[*close].at - tokens[after_name].at - 1);

      std::size_t next = *close + 1;
      bool where_clause = false;
      if (next < tokens.size() && tokens[next].text == "where") where_clause = true;
      std::string_view returns_text = "()";
      if (next + 1 < tokens.size() && tokens[next].text == "-" &&
          tokens[next + 1].text == ">") {
        const std::size_t start = tokens[next + 1].at + 1;
        next += 2;
        std::size_t end = start;
        while (next < tokens.size() && tokens[next].text != "{" && tokens[next].text != ";" &&
               tokens[next].text != "where") {
          end = tokens[next].at + tokens[next].text.size();
          ++next;
        }
        returns_text = trim(source.substr(start, end - start));
        if (next < tokens.size() && tokens[next].text == "where") where_clause = true;
      }
      if (where_clause) {
        analysis.skipped.emplace_back(name, "where clauses make the function generic");
        continue;
      }
      bool has_body = false;
      while (next < tokens.size() && tokens[next].text != "{" && tokens[next].text != ";") {
        ++next;
      }
      if (next < tokens.size() && tokens[next].text == "{") has_body = true;
      if (!has_body) {
        analysis.skipped.emplace_back(name, "a declaration without a body");
        continue;
      }

      RustFunction function;
      function.name = name;
      function.line = line;
      function.doc = doc_comment(source, line);
      function.returns_raw = std::string(returns_text);
      auto returns = type_of(returns_text);
      if (!returns.has_value()) {
        analysis.skipped.emplace_back(
            name, "the return type `" + std::string(returns_text) + "` does not cross the wire");
        continue;
      }
      function.returns = *returns;

      bool bad = false;
      for (const std::string_view part_raw : split_top_level(params_text, ',')) {
        std::string_view part = trim(part_raw);
        if (part.empty()) continue;
        if (part.starts_with("self") || part.starts_with("&self") ||
            part.starts_with("&mut self")) {
          analysis.skipped.emplace_back(name, "methods take self: the recorder calls free functions");
          bad = true;
          break;
        }
        const std::size_t colon = part.find(':');
        if (colon == std::string_view::npos) {
          analysis.skipped.emplace_back(name, "a parameter without a type");
          bad = true;
          break;
        }
        std::string_view param_name = trim(part.substr(0, colon));
        if (param_name.starts_with("mut ")) param_name = trim(param_name.substr(4));
        const std::string_view type_text = trim(part.substr(colon + 1));
        bool valid_name = !param_name.empty() && ident_start(param_name.front());
        for (const char ch : param_name) {
          if (!ident_char(ch)) valid_name = false;
        }
        if (!valid_name) {
          analysis.skipped.emplace_back(name, "patterns have no wire form");
          bad = true;
          break;
        }
        auto param_type = type_of(type_text);
        if (!param_type.has_value() || param_type->kind == RType::Kind::Unit) {
          analysis.skipped.emplace_back(
              name, "the parameter `" + std::string(param_name) + ": " +
                        std::string(type_text) + "` does not cross the wire");
          bad = true;
          break;
        }
        function.params.push_back(
            RustParam{std::string(param_name), std::string(type_text), *param_type});
      }
      if (bad) continue;
      analysis.functions.push_back(std::move(function));
    }
    if (token.text == "{") ++depth;
  }
  return analysis;
}

// --- crate information -------------------------------------------------------

struct CrateInfo {
  std::string package;
  std::string lib_name;
  std::filesystem::path root;
  bool library = false;
  std::string module;
};

std::string toml_string(std::string_view text, std::string_view key) {
  const std::string needle = std::string(key) + " = \"";
  const std::size_t at = text.find(needle);
  if (at == std::string_view::npos) return "";
  const std::size_t start = at + needle.size();
  const std::size_t end = text.find('"', start);
  return end == std::string_view::npos ? "" : std::string(text.substr(start, end - start));
}

std::optional<CrateInfo> crate_info(const std::filesystem::path& repo,
                                    const std::filesystem::path& source) {
  std::filesystem::path directory = source.parent_path();
  const std::filesystem::path repo_abs = std::filesystem::absolute(repo);
  std::filesystem::path manifest;
  while (!directory.empty()) {
    if (std::filesystem::is_regular_file(directory / "Cargo.toml")) {
      const auto text = read_file(directory / "Cargo.toml");
      if (text.has_value() && text->find("[package]") != std::string::npos) {
        manifest = directory / "Cargo.toml";
        break;
      }
    }
    std::error_code equal_error;
    if (std::filesystem::equivalent(directory, repo_abs, equal_error) && !equal_error) break;
    if (std::filesystem::absolute(directory).lexically_normal() ==
        repo_abs.lexically_normal()) {
      break;
    }
    const std::filesystem::path parent = directory.parent_path();
    if (parent == directory) break;
    directory = parent;
  }
  if (manifest.empty()) return std::nullopt;
  const auto text = read_file(manifest);
  if (!text.has_value()) return std::nullopt;
  CrateInfo info;
  info.root = manifest.parent_path();
  info.package = toml_string(*text, "name");
  if (info.package.empty()) info.package = info.root.filename().string();
  info.lib_name = info.package;

  // `[lib] name = "..."` changes the extern crate name.
  const std::size_t lib_at = text->find("[lib]");
  if (lib_at != std::string::npos) {
    const std::size_t next = text->find("\n[", lib_at + 5);
    const std::string_view section(text->data() + lib_at,
                                   next == std::string::npos ? text->size() - lib_at
                                                             : next - lib_at);
    const std::string name = toml_string(section, "name");
    if (!name.empty()) info.lib_name = name;
    info.library = true;
  }
  if (std::filesystem::is_regular_file(info.root / "src" / "lib.rs")) info.library = true;

  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(source, info.root / "src", error);
  if (error) return std::nullopt;
  const std::string text_relative = relative.generic_string();
  if (text_relative.empty() || text_relative.starts_with("..")) return std::nullopt;
  if (text_relative.starts_with("bin/") || text_relative == "main.rs") return std::nullopt;
  std::string module;
  if (text_relative != "lib.rs") {
    std::string stem = text_relative;
    if (stem.ends_with("/mod.rs")) {
      stem.resize(stem.size() - std::string("/mod.rs").size());
    } else if (stem.ends_with(".rs")) {
      stem.resize(stem.size() - 3);
    } else {
      return std::nullopt;
    }
    for (const char ch : stem) {
      if (ch == '/') {
        module += "::";
      } else {
        module += ch;
      }
    }
  }
  info.module = module;
  return info;
}

std::string rust_literal(std::string_view type_raw, const Json& value);

std::string quote(std::string_view text) { return quote_debug(text); }

std::string rust_real(double value) {
  if (!std::isfinite(value)) {
    if (std::isnan(value)) return "f64::NAN";
    return value < 0 ? "f64::NEG_INFINITY" : "f64::INFINITY";
  }
  std::string text = format_real(value);
  if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
      text.find('E') == std::string::npos && text.find("inf") == std::string::npos) {
    text += ".0";
  }
  return text;
}

std::string base_name(std::string_view raw) {
  std::string text = compact(raw);
  while (!text.empty() && text.front() == '&') {
    text.erase(text.begin());
    if (!text.empty() && text.front() == '\'') {
      text.erase(0, 1);
      while (!text.empty() && ident_char(text.front())) text.erase(0, 1);
    }
    if (text.starts_with("mut")) text.erase(0, 3);
  }
  return text;
}

std::string rust_literal(std::string_view type_raw, const Json& value) {
  auto type = type_of(type_raw);
  if (!type.has_value()) return "";
  const std::string base = base_name(type_raw);
  switch (type->kind) {
    case RType::Kind::Str:
      if (!value.is_string()) return "";
      if (base == "String") return quote(value.as_string()) + ".to_string()";
      return quote(value.as_string());
    case RType::Kind::Int: {
      if (!value.is_integer()) return "";
      const std::int64_t number = value.as_int();
      std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
      std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
      if (base == "i8") { minimum = -128; maximum = 127; }
      else if (base == "i16") { minimum = -32768; maximum = 32767; }
      else if (base == "i32") { minimum = -2147483648LL; maximum = 2147483647LL; }
      else if (base == "u8") { minimum = 0; maximum = 255; }
      else if (base == "u16") { minimum = 0; maximum = 65535; }
      else if (base == "u32") { minimum = 0; maximum = 4294967295LL; }
      else if (base == "u64" || base == "u128" || base == "usize") { minimum = 0; }
      if (number < minimum || number > maximum) return "";
      std::string suffix;
      for (const char ch : base) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') suffix += ch;
      }
      if (suffix.empty()) suffix = "i64";
      return std::to_string(number) + suffix;
    }
    case RType::Kind::Float: {
      if (!value.is_number()) return "";
      const std::string suffix = base == "f32" ? "f32" : "f64";
      if (value.is_real() && !std::isfinite(value.as_real())) {
        if (std::isnan(value.as_real())) return suffix + "::NAN";
        return value.as_real() < 0 ? suffix + "::NEG_INFINITY" : suffix + "::INFINITY";
      }
      return rust_real(value.as_number()) + suffix;
    }
    case RType::Kind::Bool:
      if (!value.is_bool()) return "";
      return value.as_bool() ? "true" : "false";
    case RType::Kind::List: {
      if (!value.is_array()) return "";
      const auto inner = type_of(type->inner_raw);
      if (!inner.has_value()) return "";
      std::string items;
      for (const Json& item : value.as_array()) {
        const std::string literal = rust_literal(type->inner_raw, item);
        if (literal.empty()) return "";
        if (!items.empty()) items += ", ";
        items += literal;
      }
      if (type->slice) {
        if (value.as_array().empty()) {
          return "&[] as &[" + elide_lifetimes(type->inner_raw) + "]";
        }
        return "&[" + items + "][..]";
      }
      return "vec![" + items + "]";
    }
    case RType::Kind::Dict: {
      if (!value.is_object()) return "";
      const auto inner = type_of(type->inner_raw);
      if (!inner.has_value()) return "";
      std::string entries;
      for (const auto& [key, item] : value.as_object()) {
        const std::string literal = rust_literal(type->inner_raw, item);
        if (literal.empty()) return "";
        if (!entries.empty()) entries += ", ";
        entries += "(" + quote(key) + ".to_string(), " + literal + ")";
      }
      if (value.as_object().empty()) return "::std::collections::HashMap::new()";
      return "::std::collections::HashMap::from([" + entries + "])";
    }
    case RType::Kind::Optional: {
      if (value.is_null()) return "None";
      const std::string literal = rust_literal(type->inner_raw, value);
      if (literal.empty()) return "";
      return "Some(" + literal + ")";
    }
    case RType::Kind::Unit: return "()";
  }
  return "";
}

std::string json_value_for(std::string_view type_raw, std::string_view expr) {
  const std::string base = base_name(type_raw);
  if (base.starts_with('[') || base.starts_with('&')) return "crate::cppan_json(" + std::string(expr) + ")";
  if (!base.empty() && base.front() == '&') return "crate::cppan_json(" + std::string(expr) + ")";
  if (type_raw.find('&') != std::string_view::npos) {
    // A reference argument can be passed straight through.
    const std::string_view trimmed = trim(type_raw);
    if (!trimmed.empty() && trimmed.front() == '&') return "crate::cppan_json(" + std::string(expr) + ")";
  }
  return "crate::cppan_json(&" + std::string(expr) + ")";
}

std::string json_return_expr(std::string_view raw) {
  const std::string_view trimmed = trim(raw);
  if (!trimmed.empty() && trimmed.front() == '&') return "crate::cppan_json(value)";
  return "crate::cppan_json(&value)";
}

// --- harness generation ------------------------------------------------------

constexpr std::string_view kHarnessPrelude = R"RUST(
use std::collections::HashMap;

#[allow(dead_code)]
fn cppan_escape(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

#[allow(dead_code)]
trait CppanJson {
    fn cppan_json(&self) -> String;
}

impl CppanJson for str {
    fn cppan_json(&self) -> String {
        format!("\"{}\"", cppan_escape(self))
    }
}

impl CppanJson for String {
    fn cppan_json(&self) -> String {
        self.as_str().cppan_json()
    }
}

impl<T: CppanJson + ?Sized> CppanJson for &T {
    fn cppan_json(&self) -> String {
        (**self).cppan_json()
    }
}

macro_rules! cppan_int {
    ($($ty:ty),* $(,)?) => {
        $(impl CppanJson for $ty {
            fn cppan_json(&self) -> String { self.to_string() }
        })*
    };
}
cppan_int!(i8, i16, i32, i64, i128, isize, u8, u16, u32, u64, u128, usize);

impl CppanJson for f64 {
    fn cppan_json(&self) -> String {
        if !self.is_finite() {
            return "{\"$opaque\":\"f64\"}".to_string();
        }
        format!("{:?}", self)
    }
}

impl CppanJson for f32 {
    fn cppan_json(&self) -> String {
        (*self as f64).cppan_json()
    }
}

impl CppanJson for bool {
    fn cppan_json(&self) -> String { self.to_string() }
}

impl CppanJson for () {
    fn cppan_json(&self) -> String { "null".to_string() }
}

impl<T: CppanJson> CppanJson for Option<T> {
    fn cppan_json(&self) -> String {
        match self {
            Some(value) => value.cppan_json(),
            None => "null".to_string(),
        }
    }
}

impl<T: CppanJson> CppanJson for Vec<T> {
    fn cppan_json(&self) -> String {
        let items: Vec<String> = self.iter().map(|item| item.cppan_json()).collect();
        format!("[{}]", items.join(","))
    }
}

impl<T: CppanJson> CppanJson for [T] {
    fn cppan_json(&self) -> String {
        let items: Vec<String> = self.iter().map(|item| item.cppan_json()).collect();
        format!("[{}]", items.join(","))
    }
}

impl<T: CppanJson> CppanJson for HashMap<String, T> {
    fn cppan_json(&self) -> String {
        let mut keys: Vec<&String> = self.keys().collect();
        keys.sort();
        let items: Vec<String> = keys
            .into_iter()
            .map(|key| format!("{}:{}", key.cppan_json(), self[key].cppan_json()))
            .collect();
        format!("{{{}}}", items.join(","))
    }
}

#[allow(dead_code)]
fn cppan_json<T: CppanJson + ?Sized>(value: &T) -> String {
    value.cppan_json()
}

#[allow(dead_code)]
fn cppan_panic_text(payload: Box<dyn std::any::Any + Send>) -> String {
    if let Some(text) = payload.downcast_ref::<&str>() {
        return (*text).to_string();
    }
    if let Some(text) = payload.downcast_ref::<String>() {
        return text.clone();
    }
    "panic".to_string()
}

#[allow(dead_code)]
fn cppan_emit_ok(id: &str, value: String) {
    println!("{{\"id\":\"{}\",\"ok\":{}}}", cppan_escape(id), value);
}

#[allow(dead_code)]
fn cppan_emit_err(id: &str, kind: &str, message: &str) {
    let short: String = message.chars().take(200).collect();
    println!(
        "{{\"id\":\"{}\",\"err\":{{\"type\":\"{}\",\"msg\":\"{}\"}}}}",
        cppan_escape(id),
        cppan_escape(kind),
        cppan_escape(&short)
    );
}

#[allow(dead_code)]
fn cppan_case(id: &str) {
    match id {
@RECORD@
        _ => {
            cppan_emit_err(id, "CaseError", "no compiled case for this id");
        }
    }
}

#[allow(dead_code)]
fn cppan_record() {
    let ids: &[&str] = @IDS@;
    for id in ids {
        cppan_case(id);
    }
}

#[allow(dead_code)]
fn cppan_bench() {
    let repeat: usize = @REPEAT@;
    #[allow(unused_mut)]
    let mut calls: usize = 0;
    let started = std::time::Instant::now();
    for _ in 0..repeat {
@BENCH@
    }
    let nanos = started.elapsed().as_nanos();
    println!("{{\"ns\":{},\"calls\":{}}}", nanos, calls);
}

fn main() {
    std::panic::set_hook(Box::new(|_| {}));
    let mode = std::env::args().nth(1).unwrap_or_else(|| "record".to_string());
    match mode.as_str() {
        "bench" => cppan_bench(),
        _ => cppan_record(),
    }
}
)RUST";

std::string case_branch(const std::string& id, const RustFunction& function,
                        const std::string& prefix, const Json& args) {
  std::string call = prefix + function.name + "(";
  std::vector<std::string> literals;
  if (args.is_array()) {
    for (std::size_t index = 0; index < args.as_array().size(); ++index) {
      if (index >= function.params.size()) {
        return "        " + quote(id) +
               " => { cppan_emit_err(" + quote(id) +
               ", \"EncodingError\", \"too many arguments\"); }\n";
      }
      const std::string literal =
          rust_literal(function.params[index].raw, args.as_array()[index]);
      if (literal.empty()) return "";
      literals.push_back(literal);
    }
  }
  if (function.params.size() != literals.size()) {
    return "        " + quote(id) + " => { cppan_emit_err(" + quote(id) +
           ", \"EncodingError\", \"wrong argument count\"); }\n";
  }
  for (std::size_t index = 0; index < literals.size(); ++index) {
    if (index > 0) call += ", ";
    call += "cppan_arg" + std::to_string(index);
  }
  call += ")";
  std::string out = "        " + quote(id) + " => {\n";
  out += "            let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {\n";
  for (std::size_t index = 0; index < literals.size(); ++index) {
    out += "                let cppan_arg" + std::to_string(index) + " = " + literals[index] +
           ";\n";
  }
  out += "                let value = " + call + ";\n";
  out += "                " + json_return_expr(function.returns_raw) + "\n";
  out += "            }));\n";
  out += "            match outcome {\n";
  out += "                Ok(value) => cppan_emit_ok(" + quote(id) + ", value),\n";
  out +=
      "                Err(payload) => cppan_emit_err(" + quote(id) +
      ", \"panic\", &cppan_panic_text(payload)),\n";
  out += "            }\n";
  out += "        }\n";
  return out;
}

std::string bench_branch(const RustFunction& function, const std::string& prefix,
                         const Json& args) {
  std::string call = prefix + function.name + "(";
  std::vector<std::string> literals;
  if (args.is_array()) {
    for (std::size_t index = 0; index < args.as_array().size(); ++index) {
      if (index >= function.params.size()) return "";
      const std::string literal = rust_literal(function.params[index].raw, args.as_array()[index]);
      if (literal.empty()) return "";
      literals.push_back(literal);
    }
  }
  if (function.params.size() != literals.size()) return "";
  for (std::size_t index = 0; index < literals.size(); ++index) {
    if (index > 0) call += ", ";
    call += "cppan_arg" + std::to_string(index);
  }
  call += ")";
  std::string out = "        {\n";
  for (std::size_t index = 0; index < literals.size(); ++index) {
    out += "            let cppan_arg" + std::to_string(index) + " = " + literals[index] + ";\n";
  }
  out += "            let value = " + call +
         ";\n            std::hint::black_box(&value);\n            calls += 1;\n        }\n";
  return out;
}

std::string render_harness(const RustAnalysis& analysis, const std::string& prefix,
                           const std::vector<Json>& record_calls,
                           const std::vector<Json>& bench_calls, std::size_t repeat) {
  std::map<std::string, const RustFunction*> by_name;
  for (const RustFunction& function : analysis.functions) by_name[function.name] = &function;

  std::string branches;
  std::string ids;
  for (const Json& call : record_calls) {
    const Json* id_field = call.find("id");
    const Json* fn = call.find("fn");
    const std::string id = id_field != nullptr && id_field->is_string() ? id_field->as_string() : "";
    const std::string name = fn != nullptr && fn->is_string() ? fn->as_string() : "";
    if (id.empty()) continue;
    const auto found = by_name.find(name.empty() ? function_of(id) : name);
    std::string branch;
    if (found == by_name.end()) {
      branch = "        " + quote(id) + " => { cppan_emit_err(" + quote(id) +
               ", \"CaseError\", \"no such function\"); }\n";
    } else {
      Json args = Json::array();
      if (const Json* value = call.find("args"); value != nullptr) args = *value;
      branch = case_branch(id, *found->second, prefix, args);
    }
    // A case whose argument does not fit the Rust width cannot be compiled;
    // leave it out rather than recording an error the original never raised.
    if (branch.empty()) continue;
    if (!ids.empty()) ids += ", ";
    ids += quote(id);
    branches += branch;
  }

  std::string bench;
  for (const Json& call : bench_calls) {
    const Json* fn = call.find("fn");
    const std::string name = fn != nullptr && fn->is_string() ? fn->as_string() : "";
    const auto found = by_name.find(name);
    if (found == by_name.end()) continue;
    Json args = Json::array();
    if (const Json* value = call.find("args"); value != nullptr) args = *value;
    bench += bench_branch(*found->second, prefix, args);
  }

  std::string text(kHarnessPrelude);
  text = replace_all(std::move(text), "@RECORD@", branches);
  text = replace_all(std::move(text), "@IDS@", "&[" + ids + "]");
  text = replace_all(std::move(text), "@BENCH@", bench);
  text = replace_all(std::move(text), "@REPEAT@", std::to_string(repeat));
  return text;
}

Result<std::filesystem::path> write_harness(const Project& project, const CrateInfo& info,
                                            const std::string& source) {
  const std::filesystem::path directory = project.dir / "build" / "rust-harness";
  std::error_code error;
  std::filesystem::create_directories(directory / "src", error);
  if (error) return std::unexpected(directory.string() + ": " + error.message());
  std::string manifest = "# Generated by cpp-anything record: a harness that calls the original.\n";
  manifest += "[package]\nname = \"cppan-rust-harness\"\nversion = \"0.0.0\"\nedition = \"2021\"\n";
  manifest += "publish = false\n\n[workspace]\n\n[dependencies]\n";
  manifest += "cppan_original = { package = " + quote(info.package) + ", path = " +
              quote(std::filesystem::absolute(info.root).string()) + " }\n";
  if (auto written = write_file(directory / "Cargo.toml", manifest); !written) {
    return std::unexpected(written.error());
  }
  if (auto written = write_file(directory / "src" / "main.rs", source); !written) {
    return std::unexpected(written.error());
  }
  return directory;
}

Result<Run> run_cargo(const Project& project, const std::filesystem::path& directory,
                      bool release) {
  std::vector<std::string> args = {"run", "--quiet"};
  if (release) args.emplace_back("--release");
  args.emplace_back("--manifest-path");
  args.emplace_back((directory / "Cargo.toml").string());
  args.emplace_back("--");
  args.emplace_back(release ? "bench" : "record");
  const Run run = run_command("cargo", args, project.dir);
  if (!run.ok) {
    return std::unexpected("the cargo recorder failed: " + run.failure() + "\n" +
                           indent_text(run.err, 2));
  }
  return run;
}

std::string prefix_for(const CrateInfo& info) {
  return info.module.empty() ? "cppan_original::" : "cppan_original::" + info.module + "::";
}

}  // namespace

Result<Analysis> analyze(const std::filesystem::path& path) {
  const auto source = read_file(path);
  if (!source.has_value()) return std::unexpected(path.string() + ": cannot read");
  auto parsed = analyze_source(*source);
  if (!parsed.has_value()) return std::unexpected(path.string() + ": cannot read");
  Analysis analysis;
  analysis.lines = parsed->lines;
  analysis.imports = parsed->imports;
  analysis.skipped = parsed->skipped;
  for (const RustFunction& function : parsed->functions) {
    Found found;
    found.name = function.name;
    found.line = function.line;
    found.call = function.call;
    found.returns = function.returns.wire;
    found.doc = function.doc;
    for (const RustParam& param : function.params) {
      found.params.push_back(FoundParam{param.name, param.type.wire, false, std::nullopt});
    }
    analysis.functions.push_back(std::move(found));
  }
  return analysis;
}

Result<std::size_t> record(const Project& project, const Unit& unit,
                           const std::vector<Json>& calls) {
  const auto source = read_file(project.source_file);
  if (!source.has_value()) return std::unexpected(project.source_file.string() + ": cannot read");
  auto parsed = analyze_source(*source);
  if (!parsed.has_value()) return std::unexpected(project.source_file.string() + ": cannot read");
  const auto info = crate_info(project.package_root.value_or(project.dir), project.source_file);
  if (!info.has_value()) {
    return std::unexpected(project.source_file.string() +
                           ": not inside a Cargo package (no Cargo.toml)");
  }
  if (!info->library) {
    return std::unexpected(info->root.string() +
                           ": the crate has no library target (src/lib.rs or [lib]); a harness "
                           "can only call a library");
  }
  (void)unit;
  const std::string prefix = prefix_for(*info);
  const std::string harness = render_harness(*parsed, prefix, calls, {}, 1);
  auto directory = write_harness(project, *info, harness);
  if (!directory) return std::unexpected(directory.error());
  auto run = run_cargo(project, *directory, false);
  if (!run) return std::unexpected(run.error());
  return write_cases(project, calls, run->out);
}

Result<BenchMeasurement> bench(const Project& project, std::size_t repeat) {
  const auto source = read_file(project.source_file);
  if (!source.has_value()) return std::unexpected(project.source_file.string() + ": cannot read");
  auto parsed = analyze_source(*source);
  if (!parsed.has_value()) return std::unexpected(project.source_file.string() + ": cannot read");
  const auto info = crate_info(project.package_root.value_or(project.dir), project.source_file);
  if (!info.has_value() || !info->library) {
    return std::unexpected(project.source_file.string() + ": not a library crate");
  }
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
    const Json* fn = doc.find("fn");
    if (fn != nullptr && fn->is_string()) first.try_emplace(fn->as_string(), doc);
  }
  std::vector<Json> calls;
  for (const auto& [function, doc] : first) {
    Json args = Json::array();
    if (const Json* value = doc.find("args")) args = *value;
    calls.push_back(Json::object({
        {"id", Json::string(function)},
        {"fn", Json::string(function)},
        {"args", args},
    }));
  }
  const std::string prefix = prefix_for(*info);
  const std::string harness = render_harness(*parsed, prefix, {}, calls, repeat);
  auto directory = write_harness(project, *info, harness);
  if (!directory) return std::unexpected(directory.error());
  auto run = run_cargo(project, *directory, true);
  if (!run) return std::unexpected(run.error());
  Json doc;
  try {
    doc = cppan::parse(trim(run->out));
  } catch (const JsonError& error) {
    return std::unexpected(error.what());
  }
  const Json* nanos = doc.find("ns");
  const Json* calls_field = doc.find("calls");
  const double ns = nanos != nullptr && nanos->is_number() ? nanos->as_number() : 0.0;
  const double total = calls_field != nullptr && calls_field->is_number()
                           ? calls_field->as_number()
                           : 1.0;
  if (total <= 0.0) return std::unexpected("the Rust benchmark ran no calls");
  return BenchMeasurement{ns / total, run->rss_kb};
}

std::vector<std::pair<std::string, std::string>> shadow_files(const Project& project,
                                                              std::string_view ns) {
  std::vector<std::pair<std::string, std::string>> files;
  const auto source = read_file(project.source_file);
  if (!source.has_value()) return files;
  auto parsed = analyze_source(*source);
  if (!parsed.has_value()) return files;
  const auto info = crate_info(project.package_root.value_or(project.dir), project.source_file);
  if (!info.has_value() || !info->library) return files;

  std::map<std::string, const RustFunction*> by_name;
  for (const RustFunction& function : parsed->functions) by_name[function.name] = &function;

  std::string wrappers;
  for (const Function& function : project.functions) {
    const auto found = by_name.find(function.name);
    if (found == by_name.end()) continue;
    const RustFunction& rust_function = *found->second;
    wrappers += "    pub fn " + rust_function.name + "(";
    for (std::size_t index = 0; index < rust_function.params.size(); ++index) {
      if (index > 0) wrappers += ", ";
      wrappers += rust_function.params[index].name + ": " +
                  elide_lifetimes(rust_function.params[index].raw);
    }
    wrappers += ") -> " + elide_lifetimes(rust_function.returns_raw) + " {\n";
    wrappers += "        let cppan_args = vec![";
    for (std::size_t index = 0; index < rust_function.params.size(); ++index) {
      if (index > 0) wrappers += ", ";
      wrappers += json_value_for(rust_function.params[index].raw,
                                 rust_function.params[index].name);
    }
    wrappers += "];\n";
    wrappers += "        let value = cppan_original::" +
                (info->module.empty() ? "" : info->module + "::") + rust_function.name + "(";
    for (std::size_t index = 0; index < rust_function.params.size(); ++index) {
      if (index > 0) wrappers += ", ";
      wrappers += rust_function.params[index].name;
    }
    wrappers += ");\n";
    wrappers += "        crate::cppan_shadow(" + quote(rust_function.name) +
                ", &cppan_args, " + json_return_expr(rust_function.returns_raw) + ");\n";
    wrappers += "        value\n    }\n\n";
  }

  std::string nesting;
  if (!info->module.empty()) {
    std::size_t start = 0;
    while (true) {
      const std::size_t separator = info->module.find("::", start);
      const std::string part = info->module.substr(
          start, separator == std::string::npos ? std::string::npos : separator - start);
      nesting += "pub mod " + part + " {\n    use std::collections::HashMap;\n";
      if (separator == std::string::npos) break;
      start = separator + 2;
    }
  }
  std::string closing;
  if (!info->module.empty()) {
    std::size_t count = 1;
    for (std::size_t index = 0; index + 1 < info->module.size(); ++index) {
      if (info->module[index] == ':' && info->module[index + 1] == ':') ++count;
    }
    for (std::size_t index = 0; index < count; ++index) closing += "}\n";
  }

  const std::filesystem::path build_dir = std::filesystem::absolute(project.dir / "build");
  const std::string library = "cppan_shadow";
  std::string build_script = "fn main() {\n";
  build_script += "    println!(\"cargo:rustc-link-search=native=" +
                  build_dir.string() + "\");\n";
  build_script += "    println!(\"cargo:rustc-link-lib=dylib=" + library + "\");\n";
  build_script += "#[cfg(target_os = \"linux\")]\n";
  build_script += "    println!(\"cargo:rustc-link-arg=-Wl,-rpath," + build_dir.string() +
                  "\");\n";
  build_script += "#[cfg(target_os = \"macos\")]\n";
  build_script += "    println!(\"cargo:rustc-link-arg=-Wl,-rpath," + build_dir.string() +
                  "\");\n";
  build_script += "}\n";

  const std::string package = std::string(ns) + "_shadow";
  std::string manifest = "# Generated by cpp-anything: the Rust shadow over the C++ port.\n";
  manifest += "[package]\nname = " + quote(package) + "\nversion = \"0.1.0\"\nedition = \"2021\"\n";
  manifest += "publish = false\n\n[workspace]\n\n[dependencies]\n";
  manifest += "cppan_original = { package = " + quote(info->package) + ", path = " +
              quote(std::filesystem::absolute(info->root).string()) + " }\n";

  const std::string log = shadow_log_path(project.dir, project.unit).string();
  std::string lib = R"RUST(// Generated by cpp-anything: the original Rust runs first, the C++ port second,
// and the caller always sees the original's result. Disagreements go to the log.
#![allow(clippy::all, dead_code)]

use std::collections::HashMap;
use std::ffi::{c_char, CStr, CString};
use std::io::Write;

extern "C" {
    fn cppan_shadow_call(name: *const c_char, args: *const c_char) -> *mut c_char;
    fn cppan_shadow_free(value: *mut c_char);
}

fn cppan_escape(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

#[allow(dead_code)]
fn cppan_json_str(value: &str) -> String {
    format!("\"{}\"", cppan_escape(value))
}

// Integer and float arguments are passed through the same JSON helpers; one
// generic trait keeps every supported type on one path.
trait CppanJson { fn cppan_json(&self) -> String; }

impl CppanJson for str { fn cppan_json(&self) -> String { cppan_json_str(self) } }
impl CppanJson for String { fn cppan_json(&self) -> String { self.as_str().cppan_json() } }
impl<T: CppanJson + ?Sized> CppanJson for &T {
    fn cppan_json(&self) -> String { (**self).cppan_json() }
}
macro_rules! cppan_int_impl {
    ($($ty:ty),* $(,)?) => {
        $(impl CppanJson for $ty {
            fn cppan_json(&self) -> String { self.to_string() }
        })*
    };
}
cppan_int_impl!(i8, i16, i32, i64, i128, isize, u8, u16, u32, u64, u128, usize);
impl CppanJson for f64 {
    fn cppan_json(&self) -> String {
        if self.is_finite() { format!("{:?}", self) } else { "{\"$opaque\":\"f64\"}".to_string() }
    }
}
impl CppanJson for f32 { fn cppan_json(&self) -> String { (*self as f64).cppan_json() } }
impl CppanJson for bool { fn cppan_json(&self) -> String { self.to_string() } }
impl CppanJson for () { fn cppan_json(&self) -> String { "null".to_string() } }
impl<T: CppanJson> CppanJson for Option<T> {
    fn cppan_json(&self) -> String {
        match self { Some(value) => value.cppan_json(), None => "null".to_string() }
    }
}
impl<T: CppanJson> CppanJson for Vec<T> {
    fn cppan_json(&self) -> String {
        let items: Vec<String> = self.iter().map(|item| item.cppan_json()).collect();
        format!("[{}]", items.join(","))
    }
}
impl<T: CppanJson> CppanJson for [T] {
    fn cppan_json(&self) -> String {
        let items: Vec<String> = self.iter().map(|item| item.cppan_json()).collect();
        format!("[{}]", items.join(","))
    }
}
impl<T: CppanJson> CppanJson for HashMap<String, T> {
    fn cppan_json(&self) -> String {
        let mut keys: Vec<&String> = self.keys().collect();
        keys.sort();
        let items: Vec<String> = keys
            .into_iter()
            .map(|key| format!("{}:{}", key.cppan_json(), self[key].cppan_json()))
            .collect();
        format!("{{{}}}", items.join(","))
    }
}

#[allow(dead_code)]
fn cppan_json<T: CppanJson + ?Sized>(value: &T) -> String { value.cppan_json() }

#[derive(PartialEq)]
enum JsonValue {
    Null,
    Bool(bool),
    Int(i64),
    Real(f64),
    Str(String),
    Arr(Vec<JsonValue>),
    Obj(Vec<(String, JsonValue)>),
}

fn parse_json(text: &str) -> Option<JsonValue> {
    let bytes = text.as_bytes();
    let mut index = 0usize;
    fn ws(bytes: &[u8], index: &mut usize) {
        while *index < bytes.len() && (bytes[*index] as char).is_whitespace() { *index += 1; }
    }
    fn string(bytes: &[u8], index: &mut usize) -> Option<String> {
        if *index >= bytes.len() || bytes[*index] != b'"' { return None; }
        *index += 1;
        let mut out = String::new();
        while *index < bytes.len() {
            let ch = bytes[*index];
            *index += 1;
            match ch {
                b'"' => return Some(out),
                b'\\' => {
                    if *index >= bytes.len() { return None; }
                    let esc = bytes[*index];
                    *index += 1;
                    match esc {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'u' => {
                            if *index + 4 > bytes.len() { return None; }
                            let hex = std::str::from_utf8(&bytes[*index..*index + 4]).ok()?;
                            let code = u32::from_str_radix(hex, 16).ok()?;
                            out.push(char::from_u32(code)?);
                            *index += 4;
                        }
                        _ => return None,
                    }
                }
                byte if byte < 0x20 => return None,
                byte => out.push(byte as char),
            }
        }
        None
    }
    fn value(bytes: &[u8], index: &mut usize) -> Option<JsonValue> {
        ws(bytes, index);
        if *index >= bytes.len() { return None; }
        match bytes[*index] {
            b'n' => { if bytes[*index..].starts_with(b"null") { *index += 4; Some(JsonValue::Null) } else { None } }
            b't' => { if bytes[*index..].starts_with(b"true") { *index += 4; Some(JsonValue::Bool(true)) } else { None } }
            b'f' => { if bytes[*index..].starts_with(b"false") { *index += 5; Some(JsonValue::Bool(false)) } else { None } }
            b'"' => string(bytes, index).map(JsonValue::Str),
            b'[' => {
                *index += 1;
                let mut items = Vec::new();
                loop {
                    ws(bytes, index);
                    if *index < bytes.len() && bytes[*index] == b']' { *index += 1; break; }
                    items.push(value(bytes, index)?);
                    ws(bytes, index);
                    if *index < bytes.len() && bytes[*index] == b',' { *index += 1; continue; }
                    if *index < bytes.len() && bytes[*index] == b']' { *index += 1; break; }
                    return None;
                }
                Some(JsonValue::Arr(items))
            }
            b'{' => {
                *index += 1;
                let mut entries = Vec::new();
                loop {
                    ws(bytes, index);
                    if *index < bytes.len() && bytes[*index] == b'}' { *index += 1; break; }
                    let key = string(bytes, index)?;
                    ws(bytes, index);
                    if *index >= bytes.len() || bytes[*index] != b':' { return None; }
                    *index += 1;
                    let item = value(bytes, index)?;
                    entries.push((key, item));
                    ws(bytes, index);
                    if *index < bytes.len() && bytes[*index] == b',' { *index += 1; continue; }
                    if *index < bytes.len() && bytes[*index] == b'}' { *index += 1; break; }
                    return None;
                }
                Some(JsonValue::Obj(entries))
            }
            _ => {
                let start = *index;
                if bytes[*index] == b'-' { *index += 1; }
                while *index < bytes.len() && (bytes[*index] as char).is_ascii_digit() { *index += 1; }
                let mut real = false;
                if *index < bytes.len() && bytes[*index] == b'.' {
                    real = true;
                    *index += 1;
                    while *index < bytes.len() && (bytes[*index] as char).is_ascii_digit() { *index += 1; }
                }
                if *index < bytes.len() && (bytes[*index] == b'e' || bytes[*index] == b'E') {
                    real = true;
                    *index += 1;
                    if *index < bytes.len() && (bytes[*index] == b'+' || bytes[*index] == b'-') { *index += 1; }
                    while *index < bytes.len() && (bytes[*index] as char).is_ascii_digit() { *index += 1; }
                }
                let text = std::str::from_utf8(&bytes[start..*index]).ok()?;
                if real {
                    text.parse::<f64>().ok().map(JsonValue::Real)
                } else {
                    text.parse::<i64>().ok().map(JsonValue::Int)
                }
            }
        }
    }
    let parsed = value(bytes, &mut index)?;
    ws(bytes, &mut index);
    if index == bytes.len() { Some(parsed) } else { None }
}

fn json_equal(left: &JsonValue, right: &JsonValue) -> bool {
    match (left, right) {
        (JsonValue::Null, JsonValue::Null) => true,
        (JsonValue::Bool(a), JsonValue::Bool(b)) => a == b,
        (JsonValue::Int(a), JsonValue::Int(b)) => a == b,
        (JsonValue::Real(a), JsonValue::Real(b)) => a == b,
        (JsonValue::Str(a), JsonValue::Str(b)) => a == b,
        (JsonValue::Arr(a), JsonValue::Arr(b)) => {
            a.len() == b.len() && a.iter().zip(b.iter()).all(|(x, y)| json_equal(x, y))
        }
        (JsonValue::Obj(a), JsonValue::Obj(b)) => {
            let mut left: Vec<(&String, &JsonValue)> = a.iter().map(|(k, v)| (k, v)).collect();
            let mut right: Vec<(&String, &JsonValue)> = b.iter().map(|(k, v)| (k, v)).collect();
            left.sort_by(|x, y| x.0.cmp(y.0));
            right.sort_by(|x, y| x.0.cmp(y.0));
            left.len() == right.len()
                && left.iter().zip(right.iter()).all(|((ka, va), (kb, vb))| ka == kb && json_equal(va, vb))
        }
        _ => false,
    }
}

fn cppan_log(function: &str, args: &[String], expected: &str, got: &str, reason: &str) {
    let path = std::env::var("CPPAN_SHADOW_LOG").unwrap_or_else(|_| @LOG@.to_string());
    if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(
            file,
            "{{\"at\":\"rust\",\"fn\":{},\"args\":[{}],\"expected\":{},\"got\":{},\"reason\":{}}}",
            cppan_json_str(function),
            args.join(","),
            expected,
            got,
            cppan_json_str(reason)
        );
    }
}

#[allow(dead_code)]
fn cppan_shadow(function: &str, args: &[String], expected: String) {
    if std::env::var("CPPAN_SHADOW").as_deref() == Ok("off") { return; }
    let args_json = format!("[{}]", args.join(","));
    let name = match CString::new(function) { Ok(value) => value, Err(_) => return };
    let args_c = match CString::new(args_json) { Ok(value) => value, Err(_) => return };
    // SAFETY: both pointers are NUL-terminated and live for the call; the result
    // is the C ABI's allocation and is freed exactly once.
    let got = unsafe {
        let out = cppan_shadow_call(name.as_ptr(), args_c.as_ptr());
        if out.is_null() {
            String::new()
        } else {
            let text = CStr::from_ptr(out).to_string_lossy().into_owned();
            cppan_shadow_free(out);
            text
        }
    };
    let Some(parsed) = parse_json(&got) else {
        cppan_log(function, args, &format!("{{\"ok\":{}}}", expected), "\"<invalid json>\"",
                  "the port returned invalid JSON");
        return;
    };
    match parsed {
        JsonValue::Obj(entries) => {
            let error = entries.iter().find(|(key, _)| key == "err").map(|(_, value)| value);
            if let Some(value) = error {
                let kind = match value {
                    JsonValue::Obj(fields) => fields
                        .iter()
                        .find(|(key, _)| key == "type")
                        .map(|(_, value)| match value {
                            JsonValue::Str(text) => text.clone(),
                            _ => "?".to_string(),
                        })
                        .unwrap_or_else(|| "?".to_string()),
                    _ => "?".to_string(),
                };
                cppan_log(function, args, &format!("{{\"ok\":{}}}", expected),
                          &format!("{{\"err\":{}}}", cppan_json_str(&kind)),
                          "the original returns a value, the port raises");
                return;
            }
            if let Some((_, value)) = entries.iter().find(|(key, _)| key == "ok") {
                if let Some(expected_value) = parse_json(&expected) {
                    if !json_equal(&expected_value, value) {
                        cppan_log(function, args, &format!("{{\"ok\":{}}}", expected),
                                  &format!("{{\"ok\":{}}}", got_value(value)),
                                  "values differ");
                    }
                }
            }
        }
        _ => {
            cppan_log(function, args, &format!("{{\"ok\":{}}}", expected), &got,
                      "the port returned an unexpected shape");
        }
    }
}

fn got_value(value: &JsonValue) -> String {
    match value {
        JsonValue::Null => "null".to_string(),
        JsonValue::Bool(inner) => inner.to_string(),
        JsonValue::Int(inner) => inner.to_string(),
        JsonValue::Real(inner) => format!("{:?}", inner),
        JsonValue::Str(inner) => format!("\"{}\"", cppan_escape(inner)),
        JsonValue::Arr(items) => {
            let parts: Vec<String> = items.iter().map(got_value).collect();
            format!("[{}]", parts.join(","))
        }
        JsonValue::Obj(entries) => {
            let mut sorted: Vec<(&String, &JsonValue)> = entries.iter().map(|(k, v)| (k, v)).collect();
            sorted.sort_by(|a, b| a.0.cmp(b.0));
            let parts: Vec<String> = sorted
                .into_iter()
                .map(|(key, item)| format!("{}:{}", cppan_json_str(key), got_value(item)))
                .collect();
            format!("{{{}}}", parts.join(","))
        }
    }
}

)RUST";
  lib = replace_all(std::move(lib), "@LOG@", quote(log));
  lib += nesting + wrappers + closing;

  std::string readme = "# Rust shadow for `" + std::string(ns) + "`\n\n";
  readme += "The original crate runs first; the C++ port runs second and disagreements go to\n";
  readme += "`shadow/" + std::string(ns) + ".shadow.jsonl`. Add the crate to the app:\n\n";
  readme += "```toml\n[dependencies]\n" + package + " = { path = \"" +
            std::filesystem::absolute(project.dir / "shadow" / "rust").string() + "\" }\n```\n\n";
  readme += "Build the port first (`cpp-anything shadow .` builds `build/libcppan_shadow.so`), and\n";
  readme += "set `LD_LIBRARY_PATH` to that build directory if the linker did not record the rpath.\n";
  readme += "Set `CPPAN_SHADOW=off` to bypass the comparison.\n";

  files.emplace_back("shadow/rust/Cargo.toml", manifest);
  files.emplace_back("shadow/rust/build.rs", build_script);
  files.emplace_back("shadow/rust/src/lib.rs", lib);
  files.emplace_back("shadow/rust/README.md", readme);
  return files;
}

}  // namespace cppan::langs::rust
