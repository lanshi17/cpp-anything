// TypeScript/JavaScript: functions exported from a module. The analyzer reads
// TypeScript annotations; the recorder runs node; the shadow binding is a
// Node-API addon over the C ABI.
//
// The analyzer is a small, honest parser of the module surface, not a TypeScript
// compiler: it tokenizes, skips comments and strings, and reads the top-level
// `export function`, `export const ... = (...) =>`, `export const ... = function`
// and `module.exports.name = ...` forms. Everything else is reported in `skipped`
// with a reason, never guessed.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>

#include "cppan/codegen.hpp"
#include "cppan/langs.hpp"
#include "cppan/shadow.hpp"

namespace cppan::langs::ts {
namespace {

enum class Kind : std::uint8_t { Ident, Number, Str, Template, Regex, Punct };

struct Tok {
  Kind kind = Kind::Punct;
  std::string text;
  std::uint32_t line = 0;
  std::size_t start = 0;
};

struct Doc {
  std::size_t end = 0;
  std::string text;
};

bool is_ident_start(unsigned char byte) {
  return std::isalpha(byte) != 0 || byte == '_' || byte == '$' || byte >= 0x80;
}

bool is_ident_byte(unsigned char byte) {
  return std::isalnum(byte) != 0 || byte == '_' || byte == '$' || byte >= 0x80;
}

bool regex_can_start(const Tok* previous) {
  if (previous == nullptr) return true;
  switch (previous->kind) {
    case Kind::Ident:
      for (const std::string_view keyword :
           {"return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "case",
            "yield", "await"}) {
        if (previous->text == keyword) return true;
      }
      return false;
    case Kind::Number:
    case Kind::Str:
    case Kind::Template:
    case Kind::Regex: return false;
    case Kind::Punct:
      return !(previous->text == ")" || previous->text == "]" || previous->text == "}");
  }
  return false;
}

std::string clean_doc(std::string_view body) {
  std::vector<std::string> lines;
  for (std::string_view line : split_lines(body)) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.remove_prefix(1);
    }
    if (line.starts_with('*')) line.remove_prefix(1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
  }
  while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  std::string joined;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    std::string line = lines[index];
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.erase(line.begin());
    }
    if (index > 0) joined += '\n';
    joined += line;
  }
  return joined;
}

std::pair<std::vector<Tok>, std::vector<Doc>> tokenize(std::string_view source) {
  std::vector<Tok> tokens;
  std::vector<Doc> docs;
  std::size_t index = 0;
  std::uint32_t line = 1;
  const auto byte_at = [&](std::size_t at) { return static_cast<unsigned char>(source[at]); };

  while (index < source.size()) {
    const unsigned char byte = byte_at(index);
    if (byte == '\n') {
      ++line;
      ++index;
      continue;
    }
    if (std::isspace(byte) != 0) {
      ++index;
      continue;
    }
    if (byte == '/' && index + 1 < source.size() && source[index + 1] == '/') {
      while (index < source.size() && source[index] != '\n') ++index;
      continue;
    }
    if (byte == '/' && index + 1 < source.size() && source[index + 1] == '*') {
      const bool is_doc = index + 2 < source.size() && source[index + 2] == '*';
      index += 2;
      const std::size_t body_start = index;
      while (index + 1 < source.size() &&
             !(source[index] == '*' && source[index + 1] == '/')) {
        if (source[index] == '\n') ++line;
        ++index;
      }
      const std::size_t body_end = std::min(index, source.size());
      index = std::min(index + 2, source.size());
      if (is_doc) docs.push_back(Doc{index, clean_doc(source.substr(body_start, body_end - body_start))});
      continue;
    }
    const std::uint32_t token_line = line;
    const std::size_t start = index;
    if (byte == '\'' || byte == '"' || byte == '`') {
      const unsigned char quote = byte;
      ++index;
      while (index < source.size()) {
        if (source[index] == '\\') {
          index = std::min(index + 2, source.size());
          continue;
        }
        if (static_cast<unsigned char>(source[index]) == quote) {
          ++index;
          break;
        }
        if (source[index] == '\n') ++line;
        ++index;
      }
      tokens.push_back(Tok{quote == '`' ? Kind::Template : Kind::Str,
                           std::string(source.substr(start, index - start)), token_line, start});
      continue;
    }
    if (byte == '/' && regex_can_start(tokens.empty() ? nullptr : &tokens.back())) {
      ++index;
      bool in_class = false;
      while (index < source.size()) {
        switch (source[index]) {
          case '\\': index = std::min(index + 2, source.size()); continue;
          case '[': in_class = true; break;
          case ']': in_class = false; break;
          case '/':
            if (!in_class) {
              ++index;
              goto regex_done;
            }
            break;
          case '\n': goto regex_done;
          default: break;
        }
        ++index;
      }
    regex_done:
      while (index < source.size() && std::isalpha(byte_at(index)) != 0) ++index;
      tokens.push_back(Tok{Kind::Regex, std::string(source.substr(start, index - start)),
                           token_line, start});
      continue;
    }
    if (std::isdigit(byte) != 0 ||
        (byte == '.' && index + 1 < source.size() && std::isdigit(byte_at(index + 1)) != 0)) {
      ++index;
      while (index < source.size()) {
        const unsigned char current = byte_at(index);
        if (std::isalnum(current) != 0 || current == '.' || current == '_') {
          ++index;
        } else if ((current == '+' || current == '-') && index > start &&
                   (source[index - 1] == 'e' || source[index - 1] == 'E')) {
          ++index;
        } else {
          break;
        }
      }
      tokens.push_back(Tok{Kind::Number, std::string(source.substr(start, index - start)),
                           token_line, start});
      continue;
    }
    if (is_ident_start(byte)) {
      ++index;
      while (index < source.size() && is_ident_byte(byte_at(index))) ++index;
      tokens.push_back(Tok{Kind::Ident, std::string(source.substr(start, index - start)),
                           token_line, start});
      continue;
    }
    std::size_t width = 1;
    for (const std::string_view candidate :
         {"...", "=>", "?.", "??", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=",
          "**"}) {
      if (source.substr(start).starts_with(candidate)) {
        width = candidate.size();
        break;
      }
    }
    index += width;
    tokens.push_back(Tok{Kind::Punct, std::string(source.substr(start, index - start)),
                         token_line, start});
  }
  return {std::move(tokens), std::move(docs)};
}

std::string_view text_at(const std::vector<Tok>& tokens, std::size_t index) {
  return index < tokens.size() ? std::string_view(tokens[index].text) : std::string_view();
}

std::optional<std::size_t> matching(const std::vector<Tok>& tokens, std::size_t open) {
  const std::string_view opener = text_at(tokens, open);
  std::string_view closer;
  if (opener == "(") {
    closer = ")";
  } else if (opener == "[") {
    closer = "]";
  } else if (opener == "{") {
    closer = "}";
  } else if (opener == "<") {
    closer = ">";
  } else {
    return std::nullopt;
  }
  int depth = 0;
  for (std::size_t index = open; index < tokens.size(); ++index) {
    if (tokens[index].text == opener) {
      ++depth;
    } else if (tokens[index].text == closer) {
      --depth;
      if (depth == 0) return index;
    }
  }
  return std::nullopt;
}

std::string join_tokens(const std::vector<Tok>& tokens, std::size_t start, std::size_t end) {
  std::string out;
  for (std::size_t index = start; index < end && index < tokens.size(); ++index) {
    out += tokens[index].text;
  }
  return out;
}

std::vector<std::pair<std::size_t, std::size_t>> split_ranges(const std::vector<Tok>& tokens,
                                                              std::size_t start, std::size_t end,
                                                              std::string_view separator) {
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  int depth = 0;
  std::size_t piece = start;
  for (std::size_t index = start; index < end; ++index) {
    const std::string_view text = text_at(tokens, index);
    if (text == "(" || text == "[" || text == "{" || text == "<") {
      ++depth;
    } else if (text == ")" || text == "]" || text == "}" || text == ">") {
      --depth;
    } else if (text == separator && depth == 0) {
      ranges.emplace_back(piece, index);
      piece = index + 1;
    }
  }
  ranges.emplace_back(piece, end);
  return ranges;
}

std::size_t skip_block(const std::vector<Tok>& tokens, std::size_t open) {
  const auto close = matching(tokens, open);
  return close.has_value() ? *close + 1 : tokens.size();
}

struct ParseFailure {
  std::string name;
  std::string reason;
  std::size_t next = 0;
};

bool is_declaration_start(std::string_view text) {
  for (const std::string_view candidate :
       {"export", "import", "const", "let", "var", "function", "class", "async", "module",
        "exports", "type", "interface", "declare", "@"}) {
    if (text == candidate) return true;
  }
  return false;
}

std::size_t skip_statement(const std::vector<Tok>& tokens, std::size_t start) {
  const std::uint32_t start_line =
      start < tokens.size() ? tokens[start].line : 0;
  int depth = 0;
  std::size_t index = start;
  while (index < tokens.size()) {
    const std::string_view text = text_at(tokens, index);
    if (text == "(" || text == "[" || text == "{") {
      ++depth;
    } else if (text == ")" || text == "]" || text == "}") {
      --depth;
    } else if (text == ";" && depth == 0) {
      return index + 1;
    }
    ++index;
    if (depth == 0 && index < tokens.size() && tokens[index].line > start_line &&
        is_declaration_start(text_at(tokens, index))) {
      return index;
    }
  }
  return index;
}

std::size_t skip_expression(const std::vector<Tok>& tokens, std::size_t start) {
  return skip_statement(tokens, start);
}

std::size_t skip_block_or_statement(const std::vector<Tok>& tokens, std::size_t start) {
  std::size_t index = start;
  while (index < tokens.size()) {
    const std::string_view text = text_at(tokens, index);
    if (text == "{") return skip_block(tokens, index);
    if (text == ";") return index + 1;
    ++index;
  }
  return index;
}

std::size_t after_signature(const std::vector<Tok>& tokens, std::size_t close) {
  std::size_t index = close + 1;
  int depth = 0;
  while (index < tokens.size()) {
    const std::string_view text = text_at(tokens, index);
    if (text == "(" || text == "[" || text == "<") {
      ++depth;
    } else if (text == ")" || text == "]" || text == ">") {
      --depth;
    } else if (text == "{" && depth == 0) {
      return skip_block(tokens, index);
    } else if (text == ";" && depth == 0) {
      return index + 1;
    } else if (text == "=>" && depth == 0) {
      return skip_expression(tokens, index + 1);
    }
    ++index;
  }
  return index;
}

std::size_t skip_class(const std::vector<Tok>& tokens, std::size_t start) {
  std::size_t index = start;
  while (index < tokens.size()) {
    const std::string_view text = text_at(tokens, index);
    if (text == "{") return skip_block(tokens, index);
    if (text == ";") return index + 1;
    ++index;
  }
  return index;
}

std::string strip_outer_parens(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
    const std::string_view inner = text.substr(1, text.size() - 2);
    int depth = 0;
    bool balanced = true;
    for (const char ch : inner) {
      if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
        ++depth;
      } else if (ch == ')' || ch == ']' || ch == '}' || ch == '>') {
        --depth;
      }
      if (depth < 0) {
        balanced = false;
        break;
      }
    }
    if (balanced && depth == 0) return strip_outer_parens(inner);
  }
  return std::string(text);
}

std::optional<std::pair<std::string_view, std::string_view>> generic_parts(
    std::string_view text) {
  const std::size_t open = text.find('<');
  if (open == std::string_view::npos || !text.ends_with('>')) return std::nullopt;
  std::string_view name = text.substr(0, open);
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
    name.remove_suffix(1);
  }
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
    name.remove_prefix(1);
  }
  const std::string_view inner = text.substr(open + 1, text.size() - open - 2);
  if (!name.empty() && (std::isalpha(static_cast<unsigned char>(name.front())) != 0 ||
                        name.front() == '_' || name.front() == '$')) {
    return std::pair{name, inner};
  }
  return std::nullopt;
}

std::string ts_type(std::string_view raw);

std::string optional_annotation(std::string annotation) {
  if (annotation.starts_with("Optional[") || annotation == "Any") return annotation;
  return "Optional[" + annotation + "]";
}

std::string ts_type(std::string_view raw) {
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.remove_prefix(1);
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
  if (raw.empty()) return "Any";
  const auto union_parts = split_top_level(raw, '|');
  if (union_parts.size() > 1) {
    bool nullable = false;
    std::vector<std::string_view> members;
    for (std::string_view member : union_parts) {
      while (!member.empty() && std::isspace(static_cast<unsigned char>(member.front()))) {
        member.remove_prefix(1);
      }
      while (!member.empty() && std::isspace(static_cast<unsigned char>(member.back()))) {
        member.remove_suffix(1);
      }
      if (member == "null" || member == "undefined") {
        nullable = true;
      } else if (!member.empty()) {
        members.push_back(member);
      }
    }
    if (members.size() == 1) {
      const std::string inner = ts_type(members[0]);
      return nullable ? optional_annotation(inner) : inner;
    }
    return "Any";
  }
  const std::string text = strip_outer_parens(raw);
  if (text.size() > 2 && text.ends_with("[]")) {
    return "list[" + ts_type(std::string_view(text).substr(0, text.size() - 2)) + "]";
  }
  if (const auto generic = generic_parts(text)) {
    if (generic->first == "Array" || generic->first == "ReadonlyArray") {
      return "list[" + ts_type(generic->second) + "]";
    }
    if (generic->first == "Record") {
      const auto args = split_top_level(generic->second, ',');
      if (args.size() == 2) {
        std::string_view key = args[0];
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.remove_prefix(1);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.remove_suffix(1);
        if (key == "string") return "dict[" + ts_type(args[1]) + "]";
      }
      return "Any";
    }
    return "Any";
  }
  if (text == "string") return "str";
  if (text == "number") return "float";
  if (text == "boolean") return "bool";
  if (text == "null") return "None";
  return "Any";
}

bool is_binding_name(std::string_view name) {
  if (name.empty()) return false;
  const unsigned char first = static_cast<unsigned char>(name.front());
  if (!(std::isalpha(first) != 0 || first == '_' || first == '$')) return false;
  for (const char ch : name) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$')) {
      return false;
    }
  }
  for (const std::string_view reserved :
       {"await", "break", "case", "catch", "class", "const", "continue", "debugger", "default",
        "delete", "do", "else", "enum", "export", "extends", "false", "finally", "for",
        "function", "if", "import", "in", "instanceof", "let", "new", "null", "return", "static",
        "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while",
        "with", "yield"}) {
    if (name == reserved) return false;
  }
  return true;
}

std::optional<std::string> unquote(std::string_view text) {
  if (text.size() < 2) return std::nullopt;
  const char quote = text.front();
  if (quote != '"' && quote != '\'') return std::nullopt;
  if (text.back() != quote) return std::nullopt;
  std::string out;
  std::size_t index = 1;
  const std::size_t end = text.size() - 1;
  while (index < end) {
    const char ch = text[index++];
    if (ch != '\\') {
      out += ch;
      continue;
    }
    if (index >= end) return std::nullopt;
    const char escape = text[index++];
    switch (escape) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '0': out += '\0'; break;
      case '\\': out += '\\'; break;
      case '\'': out += '\''; break;
      case '"': out += '"'; break;
      case '`': out += '`'; break;
      case 'u': {
        if (index + 4 > end) return std::nullopt;
        const std::string hex(text.substr(index, 4));
        index += 4;
        std::size_t used = 0;
        unsigned long value = 0;
        try {
          value = std::stoul(hex, &used, 16);
        } catch (const std::exception&) {
          return std::nullopt;
        }
        if (used != hex.size() || value > 0x10FFFF) return std::nullopt;
        // Encode the code point as UTF-8.
        if (value < 0x80) {
          out += static_cast<char>(value);
        } else if (value < 0x800) {
          out += static_cast<char>(0xC0 | (value >> 6));
          out += static_cast<char>(0x80 | (value & 0x3F));
        } else {
          out += static_cast<char>(0xE0 | (value >> 12));
          out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (value & 0x3F));
        }
        break;
      }
      default: out += escape; break;
    }
  }
  return out;
}

std::optional<Json> js_literal(std::string_view text);

std::optional<Json> js_literal(std::string_view raw) {
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.remove_prefix(1);
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
  if (raw.empty()) return std::nullopt;
  if (raw == "null") return Json::null();
  if (raw == "true") return Json::boolean(true);
  if (raw == "false") return Json::boolean(false);
  if (raw == "undefined" || raw == "NaN" || raw == "Infinity") return std::nullopt;
  if (raw.size() >= 2 && ((raw.front() == '"' && raw.back() == '"') ||
                          (raw.front() == '\'' && raw.back() == '\''))) {
    if (const auto value = unquote(raw); value.has_value()) return Json::string(*value);
    return std::nullopt;
  }
  if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']') {
    const std::string_view inner = raw.substr(1, raw.size() - 2);
    Json::Array items;
    for (std::string_view part : split_top_level(inner, ',')) {
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.remove_suffix(1);
      if (part.empty()) continue;
      auto item = js_literal(part);
      if (!item.has_value()) return std::nullopt;
      items.push_back(std::move(*item));
    }
    return Json::array(std::move(items));
  }
  if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}') {
    const std::string_view inner = raw.substr(1, raw.size() - 2);
    Json::Object pairs;
    for (std::string_view part : split_top_level(inner, ',')) {
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.remove_suffix(1);
      if (part.empty()) continue;
      const std::size_t colon = part.find(':');
      if (colon == std::string_view::npos) return std::nullopt;
      std::string_view key = part.substr(0, colon);
      while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.remove_prefix(1);
      while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.remove_suffix(1);
      std::string key_text = unquote(key).value_or(std::string(key));
      auto value = js_literal(part.substr(colon + 1));
      if (!value.has_value()) return std::nullopt;
      pairs.emplace_back(std::move(key_text), std::move(*value));
    }
    return Json::object(std::move(pairs));
  }
  std::string cleaned(raw);
  cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '_'), cleaned.end());
  if (cleaned.find('.') == std::string::npos && cleaned.find('e') == std::string::npos &&
      cleaned.find('E') == std::string::npos) {
    try {
      std::size_t used = 0;
      const std::int64_t value = std::stoll(cleaned, &used);
      if (used == cleaned.size()) return Json::integer(value);
    } catch (const std::exception&) {
    }
  }
  try {
    std::size_t used = 0;
    const double value = std::stod(cleaned, &used);
    if (used == cleaned.size() && std::isfinite(value)) return Json::real(value);
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

Found clone_found(const Found& found) {
  Found out;
  out.name = found.name;
  out.line = found.line;
  out.call = found.call;
  out.params = found.params;
  out.returns = found.returns;
  out.doc = found.doc;
  return out;
}

std::size_t skip_decorator(const std::vector<Tok>& tokens, std::size_t start) {
  std::size_t index = start + 1;
  while (index < tokens.size() &&
         (tokens[index].kind == Kind::Ident || tokens[index].text == ".")) {
    ++index;
  }
  if (text_at(tokens, index) == "(") return skip_block(tokens, index);
  return index;
}

enum class OutcomeKind : std::uint8_t { Found, Skipped, None };

struct Outcome {
  OutcomeKind kind = OutcomeKind::None;
  Found found;
  std::string name;
  std::string reason;
  std::size_t next = 0;
};

class Parser {
 public:
  explicit Parser(std::string_view source) : source_(source) {
    auto [tokens, docs] = tokenize(source);
    tokens_ = std::move(tokens);
    docs_ = std::move(docs);
    lines = static_cast<std::uint32_t>(line_count(source));
  }

  void scan();
  void resolve_promotions();

  std::vector<Found> functions;
  std::vector<std::pair<std::string, std::string>> skipped;
  std::vector<std::string> imports;
  std::uint32_t lines = 0;

 private:
  std::string doc(std::size_t byte) const {
    for (auto it = docs_.rbegin(); it != docs_.rend(); ++it) {
      if (it->end <= byte) {
        std::string_view between = source_.substr(it->end, byte - it->end);
        bool empty = true;
        for (const char ch : between) {
          if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
        }
        return empty ? it->text : std::string();
      }
    }
    return {};
  }

  Result<std::vector<FoundParam>> parse_params(std::size_t open, std::size_t close) const;
  std::expected<std::pair<Found, std::size_t>, ParseFailure> parse_function(
      std::size_t start, const std::optional<std::string>& forced);
  std::expected<std::pair<Found, std::size_t>, ParseFailure> parse_arrow(std::size_t start,
                                                                         std::string_view name);
  Outcome parse_value(std::size_t start, std::string_view name);
  Outcome parse_var(std::size_t start);
  Outcome parse_module_exports(std::size_t start);
  std::size_t parse_import(std::size_t start);
  std::size_t record(Outcome outcome, bool exported, std::size_t declaration);
  std::size_t scan_export(std::size_t index);

  std::string_view source_;
  std::vector<Tok> tokens_;
  std::vector<Doc> docs_;
  std::vector<std::pair<std::string, Found>> locals_;
  std::vector<std::pair<std::string, std::string>> promotions_;
};

Result<std::vector<FoundParam>> Parser::parse_params(std::size_t open,
                                                     std::size_t close) const {
  std::vector<FoundParam> params;
  for (const auto& [start, end] : split_ranges(tokens_, open + 1, close, ",")) {
    if (start >= end) continue;
    const Tok& name_token = tokens_[start];
    if (name_token.text == "...") {
      return std::unexpected("a rest parameter (`...args`) has no fixed signature");
    }
    if (name_token.text == "{" || name_token.text == "[") {
      return std::unexpected("a destructured parameter has no name the harness can pass");
    }
    if (name_token.kind != Kind::Ident) {
      return std::unexpected("`" + name_token.text + "` is not a plain parameter name");
    }
    const std::string name = name_token.text;
    if (name == "this") return std::unexpected("a `this` parameter exists only at compile time");
    std::size_t index = start + 1;
    bool is_optional = false;
    std::string annotation = "Any";
    if (text_at(tokens_, index) == "?") {
      is_optional = true;
      ++index;
    }
    if (text_at(tokens_, index) == ":") {
      ++index;
      const std::size_t type_start = index;
      int depth = 0;
      while (index < end) {
        const std::string_view text = text_at(tokens_, index);
        if (text == "(" || text == "[" || text == "{" || text == "<") {
          ++depth;
        } else if (text == ")" || text == "]" || text == "}" || text == ">") {
          --depth;
        } else if (text == "=" && depth == 0) {
          break;
        }
        ++index;
      }
      annotation = ts_type(join_tokens(tokens_, type_start, index));
    }
    std::optional<Json> default_value;
    bool has_default = false;
    if (text_at(tokens_, index) == "=") {
      has_default = true;
      const std::string value_text = join_tokens(tokens_, index + 1, end);
      default_value = js_literal(value_text);
      index = end;
    }
    if (index < end) {
      return std::unexpected("the parameter `" + name +
                             "` has trailing syntax the analyzer does not read");
    }
    if (is_optional) annotation = optional_annotation(annotation);
    params.push_back(FoundParam{name, annotation, has_default, std::move(default_value)});
  }
  return params;
}

std::expected<std::pair<Found, std::size_t>, ParseFailure> Parser::parse_function(
    std::size_t start, const std::optional<std::string>& forced_name) {
  std::size_t index = start;
  bool is_async = false;
  if (text_at(tokens_, index) == "async") {
    is_async = true;
    ++index;
  }
  if (text_at(tokens_, index) != "function") {
    const std::size_t next = skip_statement(tokens_, start);
    return std::unexpected(ParseFailure{"anonymous", "not a function declaration", next});
  }
  ++index;
  bool is_generator = false;
  if (text_at(tokens_, index) == "*") {
    is_generator = true;
    ++index;
  }
  std::optional<std::string> name = forced_name;
  if (!name.has_value()) {
    if (index < tokens_.size() && tokens_[index].kind == Kind::Ident) {
      name = tokens_[index].text;
      ++index;
    }
  }
  if (!name.has_value()) {
    const std::size_t next = after_signature(tokens_, index);
    return std::unexpected(ParseFailure{"anonymous", "an anonymous function has no export name", next});
  }
  if (text_at(tokens_, index) == "<") {
    const auto close = matching(tokens_, index);
    if (close.has_value()) index = *close + 1;
  }
  const std::size_t body_next =
      text_at(tokens_, index) == "("
          ? after_signature(tokens_, matching(tokens_, index).value_or(index))
          : skip_statement(tokens_, start);
  if (is_async) {
    return std::unexpected(ParseFailure{
        *name, "an async function returns a Promise, which the wire does not carry", body_next});
  }
  if (is_generator) {
    return std::unexpected(ParseFailure{
        *name, "a generator function returns an iterator, which the wire does not carry",
        body_next});
  }
  if (text_at(tokens_, index) != "(") {
    return std::unexpected(
        ParseFailure{*name, "a function without a parameter list", body_next});
  }
  const auto close = matching(tokens_, index);
  if (!close.has_value()) {
    return std::unexpected(ParseFailure{*name, "an unclosed parameter list", body_next});
  }
  auto params = parse_params(index, *close);
  if (!params) {
    return std::unexpected(
        ParseFailure{*name, params.error(), after_signature(tokens_, *close)});
  }
  std::size_t cursor = *close + 1;
  std::string returns = "Any";
  if (text_at(tokens_, cursor) == ":") {
    ++cursor;
    if (text_at(tokens_, cursor) == "{") {
      const auto end = matching(tokens_, cursor);
      if (end.has_value()) cursor = *end + 1;
    } else {
      const std::size_t type_start = cursor;
      int depth = 0;
      while (cursor < tokens_.size()) {
        const std::string_view text = text_at(tokens_, cursor);
        if ((text == "{" || text == ";" || text == "=>") && depth == 0) break;
        if (text == "(" || text == "[" || text == "{" || text == "<") {
          ++depth;
        } else if (text == ")" || text == "]" || text == "}" || text == ">") {
          --depth;
        }
        ++cursor;
      }
      returns = ts_type(join_tokens(tokens_, type_start, cursor));
    }
  }
  if (text_at(tokens_, cursor) == ";") {
    return std::unexpected(ParseFailure{
        *name,
        "an overload without a body: the implementation is the same name later", cursor + 1});
  }
  if (text_at(tokens_, cursor) != "{") {
    const std::size_t next = after_signature(tokens_, cursor);
    return std::unexpected(
        ParseFailure{*name, "a function declaration without a body", next});
  }
  const std::size_t next = skip_block(tokens_, cursor);
  const std::uint32_t line = tokens_[start].line;
  const std::string documentation = doc(tokens_[start].start);
  Found found;
  found.name = *name;
  found.line = line;
  found.call = *name;
  found.params = std::move(*params);
  found.returns = returns;
  found.doc = documentation;
  return std::pair{std::move(found), next};
}

std::expected<std::pair<Found, std::size_t>, ParseFailure> Parser::parse_arrow(
    std::size_t start, std::string_view name) {
  std::size_t index = start;
  bool is_async = false;
  if (text_at(tokens_, index) == "async") {
    is_async = true;
    ++index;
  }
  if (text_at(tokens_, index) == "<") {
    const auto close = matching(tokens_, index);
    if (close.has_value()) index = *close + 1;
  }
  const bool single_ident =
      index < tokens_.size() && tokens_[index].kind == Kind::Ident && text_at(tokens_, index + 1) == "=>";
  std::optional<std::size_t> open;
  std::optional<std::size_t> close;
  if (text_at(tokens_, index) == "(") {
    open = index;
    close = matching(tokens_, index).value_or(index);
  } else if (!single_ident) {
    const std::size_t next = skip_statement(tokens_, start);
    return std::unexpected(ParseFailure{
        std::string(name), "an arrow whose parameters are not a parenthesized list", next});
  }
  std::vector<FoundParam> params;
  if (open.has_value() && close.has_value()) {
    auto parsed = parse_params(*open, *close);
    if (!parsed) {
      return std::unexpected(ParseFailure{std::string(name), parsed.error(),
                                          after_signature(tokens_, *close)});
    }
    params = std::move(*parsed);
  } else {
    params.push_back(FoundParam{tokens_[index].text, "Any", false, std::nullopt});
  }
  std::size_t cursor = close.has_value() ? *close + 1 : index + 1;
  std::string returns = "Any";
  if (text_at(tokens_, cursor) == ":") {
    ++cursor;
    if (text_at(tokens_, cursor) == "{") {
      const auto end = matching(tokens_, cursor);
      if (end.has_value()) cursor = *end + 1;
    } else {
      const std::size_t type_start = cursor;
      int depth = 0;
      while (cursor < tokens_.size()) {
        const std::string_view text = text_at(tokens_, cursor);
        if (text == "(" || text == "[" || text == "{" || text == "<") {
          ++depth;
        } else if (text == ")" || text == "]" || text == "}" || text == ">") {
          --depth;
        } else if (text == "=>" && depth == 0) {
          break;
        }
        ++cursor;
      }
      returns = ts_type(join_tokens(tokens_, type_start, cursor));
    }
  }
  if (is_async) {
    return std::unexpected(ParseFailure{std::string(name),
                                        "an async function returns a Promise, which the wire does "
                                        "not carry",
                                        skip_statement(tokens_, cursor)});
  }
  if (text_at(tokens_, cursor) != "=>") {
    const std::size_t next = skip_statement(tokens_, cursor);
    return std::unexpected(ParseFailure{std::string(name),
                                        "a value that is not an arrow function", next});
  }
  ++cursor;
  const std::size_t next = text_at(tokens_, cursor) == "{" ? skip_block(tokens_, cursor)
                                                           : skip_expression(tokens_, cursor);
  Found found;
  found.name = std::string(name);
  found.line = tokens_[start].line;
  found.call = std::string(name);
  found.params = std::move(params);
  found.returns = returns;
  found.doc = doc(tokens_[start].start);
  return std::pair{std::move(found), next};
}

Outcome Parser::parse_value(std::size_t start, std::string_view name) {
  const std::string_view text = text_at(tokens_, start);
  if (text == "function" || text == "async") {
    auto parsed = parse_function(start, std::string(name));
    if (parsed) return Outcome{OutcomeKind::Found, std::move(parsed->first), "", "", parsed->second};
    return Outcome{OutcomeKind::Skipped, {}, std::string(name), parsed.error().reason,
                   parsed.error().next};
  }
  if (text == "(" || text == "<") {
    auto parsed = parse_arrow(start, name);
    if (parsed) return Outcome{OutcomeKind::Found, std::move(parsed->first), "", "", parsed->second};
    return Outcome{OutcomeKind::Skipped, {}, std::string(name), parsed.error().reason,
                   parsed.error().next};
  }
  if (text == name) {
    if (text_at(tokens_, start + 1) == "=>") {
      auto parsed = parse_arrow(start, name);
      if (parsed) {
        return Outcome{OutcomeKind::Found, std::move(parsed->first), "", "", parsed->second};
      }
      return Outcome{OutcomeKind::Skipped, {}, std::string(name), parsed.error().reason,
                     parsed.error().next};
    }
    return Outcome{OutcomeKind::None, {}, "", "", skip_statement(tokens_, start)};
  }
  return Outcome{OutcomeKind::None, {}, "", "", skip_statement(tokens_, start)};
}

Outcome Parser::parse_var(std::size_t start) {
  std::size_t index = start + 1;
  if (index >= tokens_.size() || tokens_[index].kind != Kind::Ident) {
    return Outcome{OutcomeKind::None, {}, "", "", skip_statement(tokens_, start)};
  }
  const std::string name = tokens_[index].text;
  ++index;
  if (text_at(tokens_, index) == ":") {
    int depth = 0;
    while (index < tokens_.size()) {
      const std::string_view text = text_at(tokens_, index);
      if (text == "(" || text == "[" || text == "{" || text == "<") {
        ++depth;
      } else if (text == ")" || text == "]" || text == "}" || text == ">") {
        --depth;
      } else if (text == "=" && depth == 0) {
        break;
      } else if (text == ";" && depth == 0) {
        return Outcome{OutcomeKind::None, {}, "", "", index + 1};
      }
      ++index;
    }
  }
  if (text_at(tokens_, index) != "=") {
    return Outcome{OutcomeKind::None, {}, "", "", skip_statement(tokens_, start)};
  }
  return parse_value(index + 1, name);
}

Outcome Parser::parse_module_exports(std::size_t start) {
  std::size_t index = start;
  if (text_at(tokens_, index) == "module") {
    if (text_at(tokens_, index + 1) != "." || text_at(tokens_, index + 2) != "exports") {
      return Outcome{OutcomeKind::None, {}, "", "", skip_statement(tokens_, start)};
    }
    index += 3;
    if (text_at(tokens_, index) != ".") {
      return Outcome{OutcomeKind::Skipped, {}, "module.exports",
                     "an object-literal export: only `module.exports.name = ...` is read",
                     skip_statement(tokens_, start)};
    }
    ++index;
  } else {
    if (text_at(tokens_, index + 1) != ".") {
      return Outcome{OutcomeKind::None, {}, "", "", skip_statement(tokens_, start)};
    }
    index += 2;
  }
  if (index >= tokens_.size() || tokens_[index].kind != Kind::Ident) {
    return Outcome{OutcomeKind::Skipped, {}, "module.exports", "a computed export name",
                   skip_statement(tokens_, start)};
  }
  const std::string name = tokens_[index].text;
  ++index;
  if (!is_binding_name(name)) {
    return Outcome{OutcomeKind::Skipped, {}, name,
                   "an exported name that is not a JavaScript binding identifier",
                   skip_statement(tokens_, start)};
  }
  if (text_at(tokens_, index) != "=") {
    return Outcome{OutcomeKind::Skipped, {}, name,
                   "an export assignment the analyzer does not read",
                   skip_statement(tokens_, start)};
  }
  const std::size_t value = index + 1;
  if (value < tokens_.size() && tokens_[value].kind == Kind::Ident) {
    const std::size_t after = value + 1;
    const bool ends = after >= tokens_.size() || tokens_[after].text == ";" ||
                      tokens_[after].line > tokens_[value].line;
    if (ends) {
      promotions_.emplace_back(name, tokens_[value].text);
      return Outcome{OutcomeKind::None, {}, "", "",
                     text_at(tokens_, after) == ";" ? after + 1 : after};
    }
  }
  return parse_value(index + 1, name);
}

std::size_t Parser::parse_import(std::size_t start) {
  const std::size_t end = skip_statement(tokens_, start);
  std::optional<std::string> specifier;
  std::size_t index = start + 1;
  while (index < end && index < tokens_.size()) {
    if (tokens_[index].kind == Kind::Str) specifier = unquote(tokens_[index].text);
    ++index;
  }
  if (specifier.has_value()) imports.push_back(*specifier);
  return end;
}

std::size_t Parser::record(Outcome outcome, bool exported, std::size_t declaration) {
  switch (outcome.kind) {
    case OutcomeKind::Found: {
      if (outcome.found.doc.empty() && declaration < tokens_.size()) {
        outcome.found.doc = doc(tokens_[declaration].start);
      }
      if (exported) {
        functions.push_back(std::move(outcome.found));
      } else {
        skipped.emplace_back(outcome.found.name, "not exported from the module");
        locals_.emplace_back(outcome.found.name, std::move(outcome.found));
      }
      return outcome.next;
    }
    case OutcomeKind::Skipped:
      skipped.emplace_back(outcome.name, outcome.reason);
      return outcome.next;
    case OutcomeKind::None: break;
  }
  return outcome.next;
}

std::size_t Parser::scan_export(std::size_t index) {
  const std::size_t after = index + 1;
  const std::string_view text = text_at(tokens_, after);
  if (text == "function" || text == "async") {
    auto parsed = parse_function(after, std::nullopt);
    Outcome outcome;
    if (parsed) {
      outcome = Outcome{OutcomeKind::Found, std::move(parsed->first), "", "", parsed->second};
    } else {
      outcome = Outcome{OutcomeKind::Skipped, {}, parsed.error().name, parsed.error().reason,
                        parsed.error().next};
    }
    return record(std::move(outcome), true, index);
  }
  if (text == "const" || text == "let" || text == "var") {
    return record(parse_var(after), true, index);
  }
  if (text == "class") {
    const std::string name = after + 1 < tokens_.size() && tokens_[after + 1].kind == Kind::Ident
                                 ? tokens_[after + 1].text
                                 : "class";
    skipped.emplace_back(name, "a class: its methods are not module exports");
    return skip_class(tokens_, after + 1);
  }
  if (text == "abstract" && text_at(tokens_, after + 1) == "class") {
    const std::string name = after + 2 < tokens_.size() && tokens_[after + 2].kind == Kind::Ident
                                 ? tokens_[after + 2].text
                                 : "class";
    skipped.emplace_back(name, "a class: its methods are not module exports");
    return skip_class(tokens_, after + 2);
  }
  if (text == "declare") {
    std::size_t name_token = after + 1;
    const std::string_view next = text_at(tokens_, name_token);
    if (next == "function" || next == "const" || next == "let" || next == "var" ||
        next == "class") {
      ++name_token;
    }
    const std::string name =
        name_token < tokens_.size() && tokens_[name_token].kind == Kind::Ident
            ? tokens_[name_token].text
            : "declare";
    skipped.emplace_back(name, "an ambient declaration has no runtime body");
    return skip_statement(tokens_, after);
  }
  if (text == "default") {
    std::size_t name_token = after + 1;
    const std::string_view next = text_at(tokens_, name_token);
    if (next == "function" || next == "class") ++name_token;
    const std::string name =
        name_token < tokens_.size() && tokens_[name_token].kind == Kind::Ident
            ? tokens_[name_token].text
            : "default";
    skipped.emplace_back(name, "a default export has no stable name the harness can call");
    if (text_at(tokens_, after + 1) == "function") {
      return skip_block_or_statement(tokens_, after + 2);
    }
    if (text_at(tokens_, after + 1) == "class") return skip_class(tokens_, after + 2);
    return skip_statement(tokens_, after + 1);
  }
  if (text == "{") {
    skipped.emplace_back("export { ... }", "an export list: the signature is not restated here");
    return skip_block(tokens_, after);
  }
  if (text == "*") {
    skipped.emplace_back("export *", "a re-export: the functions live in another module");
    return skip_statement(tokens_, after);
  }
  return skip_statement(tokens_, after);
}

void Parser::scan() {
  std::size_t index = 0;
  while (index < tokens_.size()) {
    const std::string text = tokens_[index].text;
    if (text == "import") {
      index = parse_import(index);
    } else if (text == "require") {
      if (text_at(tokens_, index + 1) == "(" && index + 2 < tokens_.size() &&
          tokens_[index + 2].kind == Kind::Str && text_at(tokens_, index + 3) == ")") {
        if (const auto specifier = unquote(tokens_[index + 2].text); specifier.has_value()) {
          imports.push_back(*specifier);
        }
        index += 4;
      } else {
        ++index;
      }
    } else if (text == "export") {
      index = scan_export(index);
    } else if (text == "module" && text_at(tokens_, index + 1) == "." &&
               text_at(tokens_, index + 2) == "exports") {
      index = record(parse_module_exports(index), true, index);
    } else if (text == "exports" && text_at(tokens_, index + 1) == ".") {
      index = record(parse_module_exports(index), true, index);
    } else if (text == "function" || text == "async") {
      auto parsed = parse_function(index, std::nullopt);
      Outcome outcome;
      if (parsed) {
        outcome = Outcome{OutcomeKind::Found, std::move(parsed->first), "", "", parsed->second};
      } else {
        outcome = Outcome{OutcomeKind::Skipped, {}, parsed.error().name, parsed.error().reason,
                          parsed.error().next};
      }
      index = record(std::move(outcome), false, index);
    } else if (text == "class") {
      const std::string name = index + 1 < tokens_.size() && tokens_[index + 1].kind == Kind::Ident
                                   ? tokens_[index + 1].text
                                   : "class";
      skipped.emplace_back(name, "a class: its methods are not module exports");
      index = skip_class(tokens_, index + 1);
    } else if (text == "abstract" && text_at(tokens_, index + 1) == "class") {
      const std::string name = index + 2 < tokens_.size() && tokens_[index + 2].kind == Kind::Ident
                                   ? tokens_[index + 2].text
                                   : "class";
      skipped.emplace_back(name, "a class: its methods are not module exports");
      index = skip_class(tokens_, index + 2);
    } else if (text == "declare") {
      std::size_t name_token = index + 1;
      const std::string_view next = text_at(tokens_, name_token);
      if (next == "function" || next == "const" || next == "let" || next == "var" ||
          next == "class") {
        ++name_token;
      }
      const std::string name =
          name_token < tokens_.size() && tokens_[name_token].kind == Kind::Ident
              ? tokens_[name_token].text
              : "declare";
      skipped.emplace_back(name, "an ambient declaration has no runtime body");
      index = skip_statement(tokens_, index);
    } else if (text == "const" || text == "let" || text == "var") {
      index = record(parse_var(index), false, index);
    } else if (text == "@") {
      std::size_t cursor = index;
      while (text_at(tokens_, cursor) == "@") cursor = skip_decorator(tokens_, cursor);
      const std::string name = cursor + 1 < tokens_.size() && tokens_[cursor + 1].kind == Kind::Ident
                                   ? tokens_[cursor + 1].text
                                   : "decorated";
      skipped.emplace_back(
          name, "a decorated declaration: decorators are outside the supported subset");
      index = skip_class(tokens_, cursor);
    } else if (text == "(" || text == "[" || text == "{") {
      const auto close = matching(tokens_, index);
      index = close.has_value() ? *close + 1 : index + 1;
    } else {
      ++index;
    }
  }
}

void Parser::resolve_promotions() {
  for (const auto& [exported, local] : promotions_) {
    const auto found = std::ranges::find_if(
        locals_, [&](const auto& entry) { return entry.first == local; });
    if (found != locals_.end()) {
      Found promoted = clone_found(found->second);
      promoted.name = exported;
      promoted.call = exported;
      std::erase_if(skipped, [&](const auto& entry) {
        return entry.first == local && entry.second == "not exported from the module";
      });
      if (std::ranges::none_of(functions,
                               [&](const Found& other) { return other.name == exported; })) {
        functions.push_back(std::move(promoted));
      }
    } else if (std::ranges::none_of(
                   functions, [&](const Found& other) { return other.name == exported; })) {
      skipped.emplace_back(exported,
                           "an exported binding whose signature is imported or defined "
                           "elsewhere");
    }
  }
}

}  // namespace

Result<Analysis> analyze(const std::filesystem::path& path, Lang) {
  const auto source = read_file(path);
  if (!source.has_value()) return std::unexpected(path.string() + ": cannot read");
  Parser parser(*source);
  parser.scan();
  parser.resolve_promotions();
  Analysis analysis;
  analysis.functions = std::move(parser.functions);
  analysis.skipped = std::move(parser.skipped);
  analysis.imports = std::move(parser.imports);
  analysis.lines = parser.lines;
  return analysis;
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

namespace {

enum class SpecKind : std::uint8_t { File, Bare };

/// Where the recorder (and the shadow module) imports the original from.
struct Specifier {
  SpecKind kind = SpecKind::File;
  std::filesystem::path path;
  std::string name;
};

std::string js_string(std::string_view text) { return dump(Json::string(std::string(text))); }

std::string replace_all(std::string text, std::string_view needle, std::string_view value) {
  if (needle.empty()) return text;
  std::size_t at = text.find(needle);
  while (at != std::string::npos) {
    text.replace(at, needle.size(), value);
    at = text.find(needle, at + value.size());
  }
  return text;
}

Specifier specifier(const Project& project, const std::optional<std::filesystem::path>& module,
                    const std::optional<std::filesystem::path>& resolve_from) {
  if (!module.has_value()) return {SpecKind::File, project.source_file, {}};
  if (module->is_absolute()) return {SpecKind::File, *module, {}};
  std::vector<std::filesystem::path> bases;
  if (resolve_from.has_value()) bases.push_back(*resolve_from);
  if (project.package_root.has_value()) bases.push_back(*project.package_root);
  bases.push_back(project.dir);
  for (const auto& base : bases) {
    const std::filesystem::path candidate = base / *module;
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) return {SpecKind::File, candidate, {}};
  }
  const std::string text = module->string();
  const auto components = static_cast<std::size_t>(std::distance(module->begin(), module->end()));
  if (components > 1 || (!text.empty() && text.front() == '.')) {
    return {SpecKind::File, project.dir / *module, {}};
  }
  return {SpecKind::Bare, {}, text};
}

std::string specifier_js(const Specifier& spec) {
  if (spec.kind == SpecKind::Bare) return spec.name;
  // The harness turns an absolute path into a file URL; a bare specifier is
  // resolved by node from the cwd/NODE_PATH the recorder passes.
  return spec.path.string();
}

bool target_is_typescript(const Specifier& spec) {
  if (spec.kind != SpecKind::File) return false;
  const std::string extension = spec.path.extension().string();
  return extension == ".ts" || extension == ".mts" || extension == ".cts";
}

std::vector<std::pair<std::string, std::string>> node_environment(
    const std::optional<std::filesystem::path>& resolve_from) {
  std::vector<std::pair<std::string, std::string>> envs;
  if (!resolve_from.has_value()) return envs;
  // `NODE_PATH` entries are searched as module roots, not as parents that
  // contain `node_modules`: include both so `--resolve-from <project>` works
  // whether the caller means the directory itself or its package directory.
  std::vector<std::filesystem::path> paths{*resolve_from};
  const std::filesystem::path modules = *resolve_from / "node_modules";
  std::error_code error;
  if (std::filesystem::is_directory(modules, error)) paths.push_back(modules);
  std::string joined;
  for (const auto& path : paths) {
    if (!joined.empty()) joined += ':';
    joined += path.string();
  }
  envs.emplace_back("NODE_PATH", joined);
  return envs;
}

Result<std::string> run_node(const std::vector<std::string>& args,
                             const std::optional<std::filesystem::path>& cwd,
                             const std::vector<std::pair<std::string, std::string>>& envs) {
  const Run run = run_command("node", args, cwd, envs);
  if (!run.ok) {
    return std::unexpected("the node recorder failed: " + run.failure() + "\n" +
                           indent_text(run.err, 2));
  }
  return run.out;
}

Result<std::string> run_node_with_fallback(const Project& project, const RecordOptions& options,
                                           const std::filesystem::path& harness,
                                           const Specifier& spec) {
  std::optional<std::filesystem::path> cwd = options.resolve_from;
  if (!cwd.has_value()) cwd = project.package_root;
  if (!cwd.has_value()) cwd = project.dir;
  const auto envs = node_environment(options.resolve_from);
  std::vector<std::string> args = options.node_args;
  args.push_back(harness.string());
  auto first = run_node(args, cwd, envs);
  if (first) return first;
  bool flagged = false;
  for (const std::string& arg : options.node_args) {
    if (arg.find("strip-types") != std::string::npos ||
        arg.find("experimental-transform-types") != std::string::npos) {
      flagged = true;
    }
  }
  if (!target_is_typescript(spec) || flagged) {
    return std::unexpected(first.error() +
                           "\n  the original is TypeScript: pass "
                           "`--node-arg=--experimental-strip-types` or "
                           "`--module=<the built JavaScript>`");
  }
  // Node older than 22.6 (and some builds) needs the flag; retry once, visibly.
  std::vector<std::string> retry{"--experimental-strip-types"};
  for (const std::string& arg : options.node_args) retry.push_back(arg);
  retry.push_back(harness.string());
  auto second = run_node(retry, cwd, envs);
  if (second) return second;
  return std::unexpected(first.error() +
                         "\n  retried with `--experimental-strip-types`:\n" + second.error() +
                         "\n  this node cannot run TypeScript; pass "
                         "`--module=<the built JavaScript>`");
}

std::string emit_serde() {
  return R"CPPAN(
function $cppan_emit(value) {
  if (value === null) return "null";
  const type = typeof value;
  if (type === "boolean") return value ? "true" : "false";
  if (type === "number") {
    if (!Number.isFinite(value)) return '"' + String(value) + '"';
    if (Object.is(value, -0)) return "-0.0";
    const text = String(value);
    return /[.eE]/.test(text) ? text : text + ".0";
  }
  if (type === "string") return JSON.stringify(value);
  if (Array.isArray(value)) return "[" + value.map($cppan_emit).join(",") + "]";
  if (type === "object") {
    return "{" + Object.keys(value).map((key) => JSON.stringify(key) + ":" + $cppan_emit(value[key])).join(",") + "}";
  }
  return "null";
}

function $cppan_encode(value, seen) {
  if (value === null) return null;
  const type = typeof value;
  if (type === "string" || type === "boolean") return value;
  if (type === "number") return Number.isFinite(value) ? value : { "$opaque": String(value) };
  if (type === "undefined") return { "$opaque": "undefined" };
  if (type === "bigint") return { "$opaque": "bigint" };
  if (type === "symbol") return { "$opaque": "symbol" };
  if (type === "function") return { "$opaque": "function" };
  seen = seen || new Set();
  if (seen.has(value)) return { "$opaque": "circular" };
  seen.add(value);
  try {
    if (Array.isArray(value)) return value.map((item) => $cppan_encode(item, seen));
    if (value instanceof Promise) return { "$opaque": "Promise" };
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) {
      return { "$opaque": (value.constructor && value.constructor.name) || "Object" };
    }
    const out = {};
    for (const key of Object.keys(value)) out[key] = $cppan_encode(value[key], seen);
    return out;
  } finally {
    seen.delete(value);
  }
}

function $cppan_error_name(error) {
  if (error !== null && typeof error === "object") {
    const name = error.constructor && error.constructor.name;
    if (name) return name;
  }
  return typeof error;
}

function $cppan_error_message(error) {
  const text = error !== null && typeof error === "object" && error.message !== undefined
    ? String(error.message)
    : String(error);
  return text.slice(0, 200);
}
)CPPAN";
}

}  // namespace

Result<std::size_t> record(const Project& project, const Unit&, const std::vector<Json>& calls,
                           const RecordOptions& options, Lang) {
  std::string calls_js;
  for (std::size_t index = 0; index < calls.size(); ++index) {
    if (index > 0) calls_js += ",\n  ";
    calls_js += dump(calls[index]);
  }
  const Specifier spec = specifier(project, options.module, options.resolve_from);
  std::string harness = R"CPPAN(// Generated by cpp-anything record; runs the original and prints one JSON line per case.
import { pathToFileURL } from "node:url";
@ENCODE@
const CALLS = [
  @CALLS@
];

const TARGET = @TARGET@;
const original = await import(
  TARGET.startsWith("/") || /^[A-Za-z]:[\\/]/.test(TARGET) ? pathToFileURL(TARGET).href : TARGET
);
const module_exports = original.default && typeof original.default === "object"
  ? { ...original.default, ...original }
  : original;

for (const call of CALLS) {
  try {
    const fn = module_exports[call.fn];
    if (typeof fn !== "function") throw new TypeError(`no exported function ${call.fn}`);
    const value = fn.apply(module_exports, call.args);
    process.stdout.write($cppan_emit({ id: call.id, ok: $cppan_encode(value) }) + "\n");
  } catch (error) {
    process.stdout.write(
      $cppan_emit({ id: call.id, err: { type: $cppan_error_name(error), msg: $cppan_error_message(error) } }) + "\n"
    );
  }
}
)CPPAN";
  harness = replace_all(std::move(harness), "@ENCODE@", emit_serde());
  harness = replace_all(std::move(harness), "@CALLS@", calls_js);
  harness = replace_all(std::move(harness), "@TARGET@", js_string(specifier_js(spec)));
  const std::filesystem::path harness_path = project.dir / "build" / "record.mjs";
  {
    auto written = write_file(harness_path, harness);
    if (!written) return std::unexpected(written.error());
  }
  // `record` has no options: the module override and resolve-from live on the
  // project once `bridge`/`record` saved them.
  RecordOptions bench_options;
  bench_options.module = project.source_module.has_value()
                             ? std::optional<std::filesystem::path>(*project.source_module)
                             : std::nullopt;
  bench_options.resolve_from = project.resolve_from.has_value()
                                   ? std::optional<std::filesystem::path>(*project.resolve_from)
                                   : std::nullopt;
  bench_options.node_args = project.node_args;
  auto output = run_node_with_fallback(project, bench_options, harness_path, spec);
  if (!output) return std::unexpected(output.error());
  return write_cases(project, calls, *output);
}

// ---------------------------------------------------------------------------
// benchmark
// ---------------------------------------------------------------------------

Result<double> bench(const Project& project, std::size_t repeat, Lang) {
  const std::filesystem::path cases_path = project.dir / "cases.jsonl";
  const auto text = read_file(cases_path);
  if (!text.has_value()) return std::unexpected(cases_path.string() + ": cannot read");
  // One call per function: the first case whose expectation is a value. A
  // function whose only recorded cases raise is measured on the first case
  // anyway, with the exception swallowed inside the timing loop.
  std::vector<std::pair<std::string, std::vector<Json>>> chosen;
  std::vector<std::pair<std::string, std::vector<Json>>> fallback;
  const auto has = [](const auto& entries, const std::string& name) {
    return std::ranges::any_of(entries,
                               [&](const auto& entry) { return entry.first == name; });
  };
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
    if (fn == nullptr || !fn->is_string()) continue;
    const std::string function = fn->as_string();
    if (has(chosen, function)) continue;
    std::vector<Json> args;
    if (const Json* value = doc.find("args"); value != nullptr && value->is_array()) {
      args = value->as_array();
    }
    const Json* expect = doc.find("expect");
    const bool ok = expect != nullptr && expect->find("ok") != nullptr;
    if (ok) {
      std::erase_if(fallback,
                    [&](const auto& entry) { return entry.first == function; });
      chosen.emplace_back(function, std::move(args));
    } else if (!has(fallback, function)) {
      fallback.emplace_back(function, std::move(args));
    }
  }
  for (auto& [name, args] : fallback) {
    if (!has(chosen, name)) chosen.emplace_back(name, std::move(args));
  }
  std::vector<Json> calls;
  calls.reserve(chosen.size());
  for (auto& [name, args] : chosen) {
    calls.push_back(Json::object({
        {"id", Json::string(name)},
        {"fn", Json::string(name)},
        {"args", Json::array(std::move(args))},
    }));
  }
  std::string calls_js;
  for (std::size_t index = 0; index < calls.size(); ++index) {
    if (index > 0) calls_js += ",\n  ";
    calls_js += dump(calls[index]);
  }
  const Specifier spec =
      specifier(project,
                project.source_module.has_value()
                    ? std::optional<std::filesystem::path>(*project.source_module)
                    : std::nullopt,
                project.resolve_from.has_value()
                    ? std::optional<std::filesystem::path>(*project.resolve_from)
                    : std::nullopt);
  std::string harness = R"CPPAN(// Generated by cpp-anything bench; loads the original once and times it.
import { pathToFileURL } from "node:url";
const CALLS = [
  @CALLS@
];
const REPEAT = @REPEAT@;
const TARGET = @TARGET@;
const original = await import(
  TARGET.startsWith("/") || /^[A-Za-z]:[\\/]/.test(TARGET) ? pathToFileURL(TARGET).href : TARGET
);
const module_exports = original.default && typeof original.default === "object"
  ? { ...original.default, ...original }
  : original;

for (const call of CALLS) {
  try { module_exports[call.fn].apply(module_exports, call.args); } catch {}
}
const started = process.hrtime.bigint();
for (let round = 0; round < REPEAT; round++) {
  for (const call of CALLS) {
    try { module_exports[call.fn].apply(module_exports, call.args); } catch {}
  }
}
const finished = process.hrtime.bigint();
process.stdout.write(`{"ns":${finished - started},"calls":${CALLS.length * REPEAT}}\n`);
)CPPAN";
  harness = replace_all(std::move(harness), "@CALLS@", calls_js);
  harness = replace_all(std::move(harness), "@REPEAT@",
                        std::to_string(repeat == 0 ? 1 : repeat));
  harness = replace_all(std::move(harness), "@TARGET@", js_string(specifier_js(spec)));
  const std::filesystem::path harness_path = project.dir / "build" / "bench.mjs";
  {
    auto written = write_file(harness_path, harness);
    if (!written) return std::unexpected(written.error());
  }
  RecordOptions bench_options;
  bench_options.module = project.source_module.has_value()
                             ? std::optional<std::filesystem::path>(*project.source_module)
                             : std::nullopt;
  bench_options.resolve_from = project.resolve_from.has_value()
                                   ? std::optional<std::filesystem::path>(*project.resolve_from)
                                   : std::nullopt;
  bench_options.node_args = project.node_args;
  auto output = run_node_with_fallback(project, bench_options, harness_path, spec);
  if (!output) return std::unexpected(output.error());
  std::string_view trimmed = *output;
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
    trimmed.remove_prefix(1);
  }
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
    trimmed.remove_suffix(1);
  }
  Json doc;
  try {
    doc = cppan::parse(trimmed);
  } catch (const JsonError& error) {
    return std::unexpected(error.what());
  }
  const Json* nanos = doc.find("ns");
  const Json* total_field = doc.find("calls");
  const double ns = nanos != nullptr && nanos->is_number() ? nanos->as_number() : 0.0;
  const double total =
      total_field != nullptr && total_field->is_number() ? total_field->as_number() : 0.0;
  if (total <= 0.0) return std::unexpected("the node benchmark ran no calls");
  return ns / total;
}

// ---------------------------------------------------------------------------
// shadow binding
// ---------------------------------------------------------------------------

namespace {

std::string addon_name(std::string_view ns) { return std::string(ns) + "_shadow"; }

}  // namespace

std::vector<std::pair<std::string, std::string>> shadow_files(const Project& project,
                                                              std::string_view ns, Lang) {
  const std::string library = codegen::shadow_library(project).string();
  const std::string log = shadow::log_path(project).string();
  const std::string addon = (project.dir / "shadow" / (addon_name(ns) + ".node")).string();
  const Specifier spec =
      specifier(project,
                project.source_module.has_value()
                    ? std::optional<std::filesystem::path>(*project.source_module)
                    : std::nullopt,
                project.resolve_from.has_value()
                    ? std::optional<std::filesystem::path>(*project.resolve_from)
                    : std::nullopt);
  std::string names;
  for (std::size_t index = 0; index < project.functions.size(); ++index) {
    if (index > 0) names += ", ";
    names += js_string(project.functions[index].name);
  }
  std::string wrappers;
  for (const Function& function : project.functions) {
    if (!wrappers.empty()) wrappers += '\n';
    std::string wrapper = R"CPPAN(export function @NAME@(...args) {
  let value;
  try {
    value = module_exports[@NAME_JS@].apply(module_exports, args);
  } catch (error) {
    if (process.env.CPPAN_SHADOW !== "off") $cppan_observe(@NAME_JS@, args, { err: $cppan_error_shape(error) });
    throw error;
  }
  if (process.env.CPPAN_SHADOW !== "off") $cppan_observe(@NAME_JS@, args, { ok: $cppan_encode(value) });
  return value;
}
)CPPAN";
    wrapper = replace_all(std::move(wrapper), "@NAME_JS@", js_string(function.name));
    wrapper = replace_all(std::move(wrapper), "@NAME@", function.name);
    wrappers += wrapper;
  }
  std::string module = R"CPPAN(// Generated by cpp-anything: the original runs first, the C++ port runs second,
// and callers only ever see the original's result. Disagreements go to the log.
//
// Build the port and the addon first:
//   cpp-anything build @PROJECT_DIR@
//   bash shadow/build.sh
import { appendFileSync } from "node:fs";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
@ENCODE@
const PROJECT = @PROJECT@;
const LIBRARY = @LIBRARY@;
const ADDON = @ADDON@;
const LOG = process.env.CPPAN_SHADOW_LOG || @LOG@;
const TARGET = @TARGET@;
const FUNCTIONS = [@NAMES@];

const require = createRequire(import.meta.url);
const addon = require(ADDON);
if (!addon || typeof addon.call !== "function") {
  throw new Error(`cpp-anything shadow: ${ADDON} does not export call() (build it with bash shadow/build.sh)`);
}

let original;
try {
  original = await import(
    TARGET.startsWith("/") || /^[A-Za-z]:[\\/]/.test(TARGET) ? pathToFileURL(TARGET).href : TARGET
  );
} catch (error) {
  throw new Error(
    `cpp-anything shadow: cannot load ${TARGET}: ${error && error.message ? error.message : error}. ` +
    "If it is TypeScript, run node with --experimental-strip-types or point the project at the built JavaScript (--module).",
  );
}
const module_exports = original.default && typeof original.default === "object"
  ? { ...original.default, ...original }
  : original;
for (const name of FUNCTIONS) {
  if (typeof module_exports[name] !== "function") {
    throw new Error(`cpp-anything shadow: ${TARGET} does not export ${name}`);
  }
}

function $cppan_error_shape(error) {
  if (error !== null && typeof error === "object") {
    return {
      type: (error.constructor && error.constructor.name) || "Error",
      msg: String(error.message !== undefined ? error.message : error).slice(0, 200),
    };
  }
  return { type: typeof error, msg: String(error).slice(0, 200) };
}

function $cppan_same(left, right) {
  if (left === right) return true;
  if (typeof left !== typeof right) return false;
  if (left && right && typeof left === "object") {
    if (Array.isArray(left) !== Array.isArray(right)) return false;
    if (Array.isArray(left)) {
      return left.length === right.length && left.every((item, index) => $cppan_same(item, right[index]));
    }
    const left_keys = Object.keys(left);
    const right_keys = Object.keys(right);
    return left_keys.length === right_keys.length
      && left_keys.every((key) => Object.prototype.hasOwnProperty.call(right, key) && $cppan_same(left[key], right[key]));
  }
  return false;
}

function $cppan_compare(expected, got) {
  const expected_error = expected && expected.err;
  const got_error = got && got.err;
  if (expected_error || got_error) {
    if (expected_error && got_error) {
      return expected_error.type === got_error.type
        ? null
        : `error type: expected ${expected_error.type}, got ${got_error.type}`;
    }
    if (!expected_error) return `the original returns a value, the port raises ${got_error.type}`;
    return `the original raises ${expected_error.type}, the port returns a value`;
  }
  if (!got || !Object.prototype.hasOwnProperty.call(got, "ok")) return "the port returned no value";
  return $cppan_same(expected.ok, got.ok)
    ? null
    : `values differ: expected ${$cppan_emit(expected.ok)}, got ${$cppan_emit(got.ok)}`;
}

function $cppan_native_call(fn, args) {
  const encoded = JSON.stringify(args.map((arg) => $cppan_encode(arg)));
  return JSON.parse(addon.call(fn, encoded));
}

function $cppan_observe(fn, args, expected) {
  let got;
  try {
    got = $cppan_native_call(fn, args);
  } catch (error) {
    got = { err: { type: "ShadowError", msg: String(error && error.message ? error.message : error).slice(0, 200) } };
  }
  const reason = $cppan_compare(expected, got);
  if (reason === null) return;
  try {
    appendFileSync(
      LOG,
      $cppan_emit({
        at: new Date().toISOString().replace(/\.\d{3}Z$/, "Z"),
        fn,
        args: args.map((arg) => $cppan_encode(arg)),
        expected,
        got,
        reason,
      }) + "\n",
    );
  } catch {
    // The log is best effort: never change what the caller sees.
  }
}

@WRAPPERS@
)CPPAN";
  const std::string project_dir = project.dir.string();
  module = replace_all(std::move(module), "@ENCODE@", emit_serde());
  module = replace_all(std::move(module), "@PROJECT_DIR@", project_dir);
  module = replace_all(std::move(module), "@PROJECT@", js_string(project_dir));
  module = replace_all(std::move(module), "@LIBRARY@", js_string(library));
  module = replace_all(std::move(module), "@ADDON@", js_string(addon));
  module = replace_all(std::move(module), "@LOG@", js_string(log));
  module = replace_all(std::move(module), "@TARGET@", js_string(specifier_js(spec)));
  module = replace_all(std::move(module), "@NAMES@", names);
  module = replace_all(std::move(module), "@WRAPPERS@", wrappers);

  std::string addon_cc = R"CPPAN(// Node-API addon for the @NS@ shadow: one string in, one JSON string out.
// Generated by cpp-anything; the C ABI is cppan_shadow_call/cppan_shadow_free.
#include <node_api.h>

#include <cstdlib>
#include <string>

extern "C" {
const char* cppan_shadow_call(const char* fn, const char* args_json);
void cppan_shadow_free(const char* pointer);
}

namespace {

bool get_string(napi_env env, napi_value value, std::string& out) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return false;
  out.resize(length + 1);
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) {
    out.clear();
    return false;
  }
  out.resize(copied);
  return true;
}

napi_value call(napi_env env, napi_callback_info info) {
  size_t count = 2;
  napi_value argv[2];
  if (napi_get_cb_info(env, info, &count, argv, nullptr, nullptr) != napi_ok || count < 2) {
    napi_throw_type_error(env, nullptr, "call(fn, argsJson) needs two strings");
    return nullptr;
  }
  std::string fn;
  std::string args;
  if (!get_string(env, argv[0], fn) || !get_string(env, argv[1], args)) {
    napi_throw_type_error(env, nullptr, "call(fn, argsJson) needs two strings");
    return nullptr;
  }
  const char* result = cppan_shadow_call(fn.c_str(), args.c_str());
  if (result == nullptr) {
    napi_throw_error(env, nullptr, "cppan_shadow_call returned no result");
    return nullptr;
  }
  napi_value out;
  const napi_status status = napi_create_string_utf8(env, result, NAPI_AUTO_LENGTH, &out);
  cppan_shadow_free(result);
  if (status != napi_ok) {
    napi_throw_error(env, nullptr, "cannot allocate the result string");
    return nullptr;
  }
  return out;
}

napi_value init(napi_env env, napi_value exports) {
  napi_value function = nullptr;
  if (napi_create_function(env, "call", NAPI_AUTO_LENGTH, call, nullptr, &function) != napi_ok) {
    return nullptr;
  }
  napi_set_named_property(env, exports, "call", function);
  return exports;
}

}  // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
)CPPAN";
  addon_cc = replace_all(std::move(addon_cc), "@NS@", std::string(ns));

  const std::string name = addon_name(ns);
  std::string build_sh = R"CPPAN(#!/bin/sh
# Builds the Node-API addon that loads libcppan_shadow.so. Run after `cpp-anything build`.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NODE="${NODE:-node}"
CXX="${CXX:-g++}"
BUILD=@BUILD@
OUT="$here/@ADDON@.node"
NODE_PREFIX=$("$NODE" -p "require('path').dirname(process.execPath)")
INCLUDE="$NODE_PREFIX/../include/node"
if [ ! -f "$INCLUDE/node_api.h" ]; then
  INCLUDE="$NODE_PREFIX/include/node"
fi
if [ ! -f "$INCLUDE/node_api.h" ]; then
  echo "cpp-anything: node_api.h not found under $NODE_PREFIX; install node headers" >&2
  exit 1
fi

if [ "${CPPAN_USE_NODE_GYP:-0}" = "1" ] && command -v node-gyp >/dev/null 2>&1; then
  cd "$here"
  node-gyp rebuild --nodedir="$NODE_PREFIX/.." >/dev/null
  cp "$here/build/Release/@ADDON@.node" "$OUT"
  echo "cpp-anything: built $OUT (node-gyp)"
  exit 0
fi

case "$(uname -s)" in
  Darwin)
    SHARED="-bundle -undefined dynamic_lookup"
    ;;
  *)
    SHARED="-shared"
    ;;
esac

"$CXX" -std=c++17 -O2 -fPIC $SHARED \
  -I"$INCLUDE" -DNODE_GYP_MODULE_NAME=@ADDON@ \
  "$here/addon.cc" -o "$OUT" \
  -L"$BUILD" -lcppan_shadow -Wl,-rpath,"$BUILD"
echo "cpp-anything: built $OUT"
)CPPAN";
  build_sh = replace_all(std::move(build_sh), "@ADDON@", name);
  build_sh = replace_all(std::move(build_sh), "@BUILD@", (project.dir / "build").string());

  std::string binding_gyp = R"CPPAN({
  "targets": [
    {
      "target_name": "@ADDON@",
      "sources": ["addon.cc"],
      "libraries": ["-L@BUILD@", "-lcppan_shadow", "-Wl,-rpath,@BUILD@"],
      "defines": ["NODE_GYP_MODULE_NAME=@ADDON@"]
    }
  ]
}
)CPPAN";
  binding_gyp = replace_all(std::move(binding_gyp), "@ADDON@", name);
  binding_gyp = replace_all(std::move(binding_gyp), "@BUILD@", (project.dir / "build").string());

  std::vector<std::pair<std::string, std::string>> files;
  files.emplace_back("shadow/" + std::string(ns) + "_shadow.mjs", std::move(module));
  files.emplace_back("shadow/addon.cc", std::move(addon_cc));
  files.emplace_back("shadow/build.sh", std::move(build_sh));
  files.emplace_back("shadow/binding.gyp", std::move(binding_gyp));
  return files;
}

}  // namespace cppan::langs::ts
