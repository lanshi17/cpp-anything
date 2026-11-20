// Java: a unit is a class file. The analyzer reads public static methods, the
// recorder compiles a harness with javac, and the shadow binding is a JNI library
// over the C ABI.
//
// The supported subset is a class (ideally public) with `public static` methods
// whose parameters and return value cross the wire: int/long/short/byte,
// double/float, boolean, String, List<String> and Map<String,String>. Everything
// else (arrays, varargs, other generic types, instance methods, constructors) is
// reported as skipped with the type that refused it.
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppan/langs.hpp"
#include "cppan/shadow.hpp"

namespace cppan::langs::java {
namespace {

struct JavaParam {
  std::string name;
  /// The type as written (`List<String>`, `int`): what the harness declares.
  std::string raw;
  /// The wire annotation (`list[str]`, `int`): what the port receives.
  std::string ty;
};

struct JavaFunction {
  std::string name;
  std::uint32_t line = 0;
  std::string returns_raw;
  std::string returns_ty;
  std::vector<JavaParam> params;
  std::vector<std::string> throws;
  std::string doc;
};

struct Parsed {
  std::string package;
  std::string class_name;
  std::vector<std::string> imports;
  std::vector<JavaFunction> functions;
  std::vector<std::pair<std::string, std::string>> skipped;
  std::uint32_t lines = 0;
};

struct ClassDecl {
  std::string name;
  std::string kind;
  std::size_t open = 0;
  std::size_t close = 0;
  bool public_class = false;
};

std::vector<std::string_view> split_words(std::string_view text);

bool is_ident_byte(char byte) {
  const unsigned char ch = static_cast<unsigned char>(byte);
  return std::isalnum(ch) != 0 || byte == '_' || byte == '$';
}

bool is_ident_start(char byte) {
  return std::isalpha(static_cast<unsigned char>(byte)) != 0 || byte == '_' || byte == '$';
}

bool is_java_identifier(std::string_view text) {
  if (text.empty()) return false;
  if (!(std::isalpha(static_cast<unsigned char>(text.front())) != 0 || text.front() == '_' ||
        text.front() == '$')) {
    return false;
  }
  for (const char ch : text) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$')) {
      return false;
    }
  }
  return true;
}

std::size_t ident_end(std::string_view text, std::size_t start) {
  std::size_t index = start;
  while (index < text.size() &&
         (is_ident_byte(text[index]) || static_cast<unsigned char>(text[index]) >= 0x80)) {
    ++index;
  }
  return index;
}

std::size_t skip_ws(std::string_view text, std::size_t index) {
  while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
  return index;
}

std::optional<std::size_t> matching(std::string_view text, std::size_t open) {
  if (open >= text.size()) return std::nullopt;
  const char open_byte = text[open];
  char close_byte = 0;
  if (open_byte == '{') {
    close_byte = '}';
  } else if (open_byte == '(') {
    close_byte = ')';
  } else if (open_byte == '[') {
    close_byte = ']';
  } else {
    return std::nullopt;
  }
  std::size_t depth = 0;
  for (std::size_t index = open; index < text.size(); ++index) {
    if (text[index] == open_byte) {
      ++depth;
    } else if (text[index] == close_byte) {
      --depth;
      if (depth == 0) return index;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> skip_angles(std::string_view text, std::size_t open) {
  if (open >= text.size() || text[open] != '<') return std::nullopt;
  std::size_t depth = 0;
  for (std::size_t index = open; index < text.size(); ++index) {
    if (text[index] == '<') {
      ++depth;
    } else if (text[index] == '>') {
      --depth;
      if (depth == 0) return index;
    }
  }
  return std::nullopt;
}

std::vector<std::size_t> keyword_positions(std::string_view text, std::string_view keyword) {
  std::vector<std::size_t> out;
  std::size_t index = 0;
  while (index + keyword.size() <= text.size()) {
    if (text.substr(index, keyword.size()) == keyword &&
        (index == 0 || !is_ident_byte(text[index - 1])) &&
        (index + keyword.size() == text.size() || !is_ident_byte(text[index + keyword.size()]))) {
      out.push_back(index);
      index += keyword.size();
    } else {
      ++index;
    }
  }
  return out;
}

std::optional<std::pair<std::string, std::size_t>> statement_after(std::string_view text,
                                                                   std::optional<std::size_t> position) {
  if (!position.has_value()) return std::nullopt;
  const std::size_t rest = skip_ws(text, *position);
  const std::size_t end = text.find(';', rest);
  if (end == std::string_view::npos) return std::nullopt;
  const std::string_view body = text.substr(rest, end - rest);
  std::size_t begin = 0;
  std::size_t last = body.size();
  while (begin < last && std::isspace(static_cast<unsigned char>(body[begin]))) ++begin;
  while (last > begin && std::isspace(static_cast<unsigned char>(body[last - 1]))) --last;
  return std::pair{std::string(body.substr(begin, last - begin)), end + 1};
}

std::size_t annotation_end(std::string_view text) {
  std::size_t index = 1;
  while (index < text.size() && (is_ident_byte(text[index]) || text[index] == '.' ||
                                 static_cast<unsigned char>(text[index]) >= 0x80)) {
    ++index;
  }
  index = skip_ws(text, index);
  if (index < text.size() && text[index] == '(') {
    return matching(text, index).value_or(text.size() - 1) + 1;
  }
  return index;
}

std::size_t skip_annotations(std::string_view text, std::size_t index) {
  while (true) {
    index = skip_ws(text, index);
    if (index >= text.size() || text[index] != '@') return index;
    index += annotation_end(text.substr(index));
  }
}

std::size_t skip_member(std::string_view text, std::size_t index) {
  int paren = 0;
  int brace = 0;
  int bracket = 0;
  while (index < text.size()) {
    const char ch = text[index];
    if (ch == '(') {
      ++paren;
    } else if (ch == ')') {
      --paren;
    } else if (ch == '{') {
      ++brace;
    } else if (ch == '}') {
      --brace;
      if (brace < 0) return index + 1;
    } else if (ch == '[') {
      ++bracket;
    } else if (ch == ']') {
      --bracket;
    } else if (ch == ';' && paren == 0 && brace == 0 && bracket == 0) {
      return index + 1;
    }
    ++index;
  }
  return index;
}

std::vector<std::string_view> split_angles(std::string_view text, char separator) {
  std::vector<std::string_view> out;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
      ++depth;
    } else if (ch == ')' || ch == ']' || ch == '}' || ch == '>') {
      --depth;
    } else if (ch == separator && depth <= 0) {
      out.push_back(text.substr(start, index - start));
      start = index + 1;
    }
  }
  out.push_back(text.substr(start));
  return out;
}

std::optional<std::string_view> strip_word(std::string_view text, std::string_view word) {
  if (!text.starts_with(word)) return std::nullopt;
  const std::string_view rest = text.substr(word.size());
  if (rest.empty() || std::isspace(static_cast<unsigned char>(rest.front()))) {
    std::size_t index = 0;
    while (index < rest.size() && std::isspace(static_cast<unsigned char>(rest[index]))) ++index;
    return rest.substr(index);
  }
  return std::nullopt;
}

struct Modifiers {
  bool public_access = false;
  bool is_static = false;
  bool is_private = false;
  std::string rest;
};

Modifiers strip_modifiers(std::string_view text) {
  Modifiers out;
  std::string rest(text);
  while (true) {
    bool matched = false;
    for (const std::string_view word :
         {"public", "private", "protected", "static", "final", "abstract", "synchronized",
          "native", "strictfp", "default", "transient", "volatile", "sealed", "non-sealed"}) {
      if (const auto after = strip_word(rest, word); after.has_value()) {
        if (word == "public") out.public_access = true;
        if (word == "private") out.is_private = true;
        if (word == "static") out.is_static = true;
        rest = std::string(*after);
        matched = true;
        break;
      }
    }
    if (!matched) break;
  }
  // trim
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) {
    rest.erase(rest.begin());
  }
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
  out.rest = std::move(rest);
  return out;
}

std::string blank_literals(std::string_view source) {
  std::string out;
  out.reserve(source.size());
  std::size_t index = 0;
  while (index < source.size()) {
    const char ch = source[index];
    const bool slash_slash = ch == '/' && index + 1 < source.size() && source[index + 1] == '/';
    const bool slash_star = ch == '/' && index + 1 < source.size() && source[index + 1] == '*';
    if (slash_slash) {
      out += "  ";
      index += 2;
      while (index < source.size() && source[index] != '\n') {
        out += ' ';
        ++index;
      }
    } else if (slash_star) {
      out += "  ";
      index += 2;
      while (index < source.size()) {
        if (source[index] == '*' && index + 1 < source.size() && source[index + 1] == '/') {
          out += "  ";
          index += 2;
          break;
        }
        out += source[index] == '\n' ? '\n' : ' ';
        ++index;
      }
    } else if (ch == '"') {
      const bool text_block = index + 2 < source.size() && source[index + 1] == '"' &&
                              source[index + 2] == '"';
      if (text_block) {
        out += "   ";
        index += 3;
        while (index < source.size()) {
          if (source[index] == '"' && index + 2 < source.size() && source[index + 1] == '"' &&
              source[index + 2] == '"') {
            out += "   ";
            index += 3;
            break;
          }
          out += source[index] == '\n' ? '\n' : ' ';
          ++index;
        }
      } else {
        out += '"';
        ++index;
        while (index < source.size()) {
          if (source[index] == '\\') {
            out += "  ";
            index += 2;
            continue;
          }
          if (source[index] == '"') {
            out += '"';
            ++index;
            break;
          }
          out += source[index] == '\n' ? '\n' : ' ';
          ++index;
        }
      }
    } else if (ch == '\'') {
      out += '\'';
      ++index;
      while (index < source.size()) {
        if (source[index] == '\\') {
          out += "  ";
          index += 2;
          continue;
        }
        if (source[index] == '\'') {
          out += '\'';
          ++index;
          break;
        }
        out += source[index] == '\n' ? '\n' : ' ';
        ++index;
      }
    } else {
      out += ch;
      ++index;
    }
  }
  return out;
}

Result<std::vector<ClassDecl>> top_level_classes(std::string_view text) {
  std::vector<ClassDecl> out;
  std::size_t index = 0;
  int depth = 0;
  while (index < text.size()) {
    const char ch = text[index];
    if (ch == '{') {
      ++depth;
      ++index;
      continue;
    }
    if (ch == '}') {
      --depth;
      ++index;
      continue;
    }
    if (depth == 0 && is_ident_start(ch)) {
      const std::size_t end = ident_end(text, index);
      const std::string_view word = text.substr(index, end - index);
      if (word == "class" || word == "interface" || word == "enum" || word == "record") {
        const std::string kind(word);
        std::size_t name_start = skip_ws(text, end);
        if (name_start < text.size() && text[name_start] == '<') {
          name_start = skip_angles(text, name_start).value_or(text.size() - 1) + 1;
        }
        name_start = skip_ws(text, name_start);
        const std::size_t name_end = ident_end(text, name_start);
        if (name_end == name_start) {
          index = end;
          continue;
        }
        const std::string name(text.substr(name_start, name_end - name_start));
        std::size_t open = name_end;
        int angle = 0;
        while (open < text.size()) {
          const char c = text[open];
          if (c == '<') {
            ++angle;
          } else if (c == '>') {
            --angle;
          } else if ((c == '{' || c == ';') && angle <= 0) {
            break;
          }
          ++open;
        }
        if (open >= text.size() || text[open] != '{') {
          index = end;
          continue;
        }
        const auto close = matching(text, open);
        if (!close.has_value()) return std::unexpected("a class body is not closed");
        std::size_t start = index;
        while (start > 0 && text[start - 1] != ';' && text[start - 1] != '}') --start;
        const std::string_view modifiers = text.substr(start, index - start);
        bool public_class = false;
        for (const std::string_view word_part : split_words(modifiers)) {
          if (word_part == "public") public_class = true;
        }
        out.push_back(ClassDecl{name, kind, open, *close, public_class});
        index = *close + 1;
        continue;
      }
      index = end;
      continue;
    }
    ++index;
  }
  return out;
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

Result<std::pair<std::string, std::string>> parse_param(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  while (true) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.remove_prefix(1);
    }
    if (!text.starts_with('@')) break;
    text.remove_prefix(annotation_end(text));
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.remove_prefix(1);
    }
  }
  if (const auto rest = strip_word(text, "final"); rest.has_value()) text = *rest;
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  if (text.find("...") != std::string_view::npos) {
    return std::unexpected("a varargs parameter has no wire form");
  }
  std::size_t split = std::string_view::npos;
  for (std::size_t index = text.size(); index > 0; --index) {
    if (std::isspace(static_cast<unsigned char>(text[index - 1]))) {
      split = index - 1;
      break;
    }
  }
  if (split == std::string_view::npos) {
    return std::unexpected("`" + std::string(text) + "` is not a typed parameter");
  }
  std::string type(text.substr(0, split));
  std::string name(text.substr(split));
  while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) type.pop_back();
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
    name.erase(name.begin());
  }
  if (name.empty() || type.empty()) {
    return std::unexpected("`" + std::string(text) + "` is not a typed parameter");
  }
  if (name.find('[') != std::string::npos || type.ends_with("[]")) {
    return std::unexpected("the array parameter `" + name + "` has no wire form");
  }
  if (!is_java_identifier(name)) {
    return std::unexpected("`" + name + "` is not a plain parameter name");
  }
  return std::pair{name, type};
}

std::optional<std::string_view> wire_type(std::string_view raw) {
  std::string compact;
  for (const char ch : raw) {
    if (!std::isspace(static_cast<unsigned char>(ch))) compact += ch;
  }
  if (compact == "int" || compact == "long" || compact == "short" || compact == "byte") {
    return "int";
  }
  if (compact == "double" || compact == "float") return "float";
  if (compact == "boolean") return "bool";
  if (compact == "String" || compact == "java.lang.String") return "str";
  for (const std::string_view candidate :
       {"List<String>", "ArrayList<String>", "LinkedList<String>", "Collection<String>",
        "java.util.List<String>", "java.util.ArrayList<String>", "java.util.LinkedList<String>",
        "java.util.Collection<String>"}) {
    if (compact == candidate) return "list[str]";
  }
  for (const std::string_view candidate :
       {"Map<String,String>", "HashMap<String,String>", "LinkedHashMap<String,String>",
        "java.util.Map<String,String>", "java.util.HashMap<String,String>",
        "java.util.LinkedHashMap<String,String>"}) {
    if (compact == candidate) return "dict[str]";
  }
  return std::nullopt;
}

std::string doc_before(const std::vector<std::string_view>& lines, std::size_t line) {
  std::size_t index = line;
  while (index > 0) {
    std::string_view text = lines[index - 1];
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
      text.remove_suffix(1);
    }
    if (text.empty()) {
      --index;
      continue;
    }
    if (!text.ends_with("*/")) return {};
    std::vector<std::string> collected{std::string(text)};
    std::size_t start = index - 1;
    while (lines[start].find("/**") == std::string_view::npos) {
      if (start == 0) return {};
      --start;
      std::string_view part = lines[start];
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
        part.remove_prefix(1);
      }
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) {
        part.remove_suffix(1);
      }
      collected.emplace_back(part);
    }
    std::ranges::reverse(collected);
    std::string joined;
    for (std::size_t part = 0; part < collected.size(); ++part) {
      if (part > 0) joined += '\n';
      joined += collected[part];
    }
    const std::size_t open = joined.find("/**");
    if (open == std::string::npos) return {};
    const std::size_t close = joined.rfind("*/");
    if (close == std::string::npos) return {};
    std::string inner = joined.substr(open + 3, close - open - 3);
    std::vector<std::string> parts;
    for (const std::string_view raw_line : split_lines(inner)) {
      std::string_view part = raw_line;
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
        part.remove_prefix(1);
      }
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) {
        part.remove_suffix(1);
      }
      if (part.starts_with('*')) part = part.substr(1);
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
        part.remove_prefix(1);
      }
      while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) {
        part.remove_suffix(1);
      }
      if (!part.empty()) parts.emplace_back(part);
    }
    std::string out;
    for (const std::string& part : parts) {
      if (!out.empty()) out += ' ';
      out += part;
    }
    return out;
  }
  return {};
}

Result<Parsed> parse_java(const std::filesystem::path& path) {
  const auto source = read_file(path);
  if (!source.has_value()) return std::unexpected(path.string() + ": cannot read");
  const auto source_lines = split_lines(*source);
  const std::string blank = blank_literals(*source);
  Parsed parsed;
  parsed.lines = static_cast<std::uint32_t>(line_count(*source));

  const auto packages = keyword_positions(blank, "package");
  if (!packages.empty()) {
    if (const auto statement = statement_after(blank, packages.front() + 7);
        statement.has_value()) {
      parsed.package = statement->first;
    }
  }
  if (!parsed.package.empty()) parsed.imports.push_back(parsed.package);
  for (const std::size_t position : keyword_positions(blank, "import")) {
    if (const auto statement = statement_after(blank, position + 6); statement.has_value()) {
      if (!statement->first.empty()) parsed.imports.push_back(statement->first);
    }
  }

  auto classes = top_level_classes(blank);
  if (!classes) return std::unexpected(classes.error());
  const ClassDecl* chosen = nullptr;
  for (const ClassDecl& decl : *classes) {
    if (decl.kind != "class") continue;
    if (decl.public_class) {
      chosen = &decl;
      break;
    }
    if (chosen == nullptr) chosen = &decl;
  }
  if (chosen == nullptr) {
    return std::unexpected(std::format(
        "{}: no top-level class declaration (the Java plugin ports a class with public static "
        "methods)",
        path.string()));
  }
  parsed.class_name = chosen->name;
  const std::string_view body = std::string_view(blank).substr(chosen->open + 1,
                                                               chosen->close - chosen->open - 1);
  const std::size_t base = chosen->open + 1;

  for (const ClassDecl& decl : *classes) {
    if (decl.open == chosen->open) continue;
    const std::string reason =
        decl.kind == "class"
            ? std::format("another top-level class: only `{}` is ported (one class per unit)",
                          chosen->name)
            : std::format("a top-level {}: only a class is ported (one class per unit)",
                          decl.kind);
    parsed.skipped.emplace_back(decl.name, reason);
  }

  std::size_t index = 0;
  while (index < body.size()) {
    index = skip_ws(body, index);
    if (index >= body.size()) break;
    const std::size_t decl_start = index;
    const std::size_t head = skip_annotations(body, index);
    if (head >= body.size()) break;

    std::size_t cursor = head;
    int angle = 0;
    std::optional<std::size_t> opener;
    std::optional<std::size_t> block;
    while (cursor < body.size()) {
      const char ch = body[cursor];
      if (ch == '<') {
        ++angle;
      } else if (ch == '>') {
        --angle;
      } else if (ch == '(' && angle <= 0) {
        opener = cursor;
        break;
      } else if ((ch == '{' || ch == ';') && angle <= 0) {
        block = cursor;
        break;
      }
      ++cursor;
    }
    if (!opener.has_value() && !block.has_value()) break;
    const std::size_t stop = opener.has_value() ? *opener : *block;
    if (body[stop] != '(') {
      const std::string_view head_text = body.substr(head, stop - head);
      const std::string_view rest = strip_modifiers(head_text).rest;
      for (const std::string_view keyword : {"class", "interface", "enum", "record"}) {
        if (!strip_word(rest, keyword).has_value()) continue;
        const std::size_t name_start = skip_ws(rest, keyword.size());
        const std::size_t name_end = ident_end(rest, name_start);
        const std::string name = name_end > name_start
                                     ? std::string(rest.substr(name_start, name_end - name_start))
                                     : std::string(keyword);
        parsed.skipped.emplace_back(
            name, std::format("a nested {}: only the outer class's methods are ported", keyword));
        break;
      }
      if (body[stop] == ';') {
        index = stop + 1;
        continue;
      }
      std::size_t after = matching(body, stop).value_or(body.size() - 1) + 1;
      const std::size_t next = skip_ws(body, after);
      if (next < body.size() && body[next] == ';') after = next + 1;
      index = std::max(after, index + 1);
      continue;
    }
    if (body.substr(head, stop - head).find('=') != std::string_view::npos ||
        body.substr(head, stop - head).find('@') != std::string_view::npos) {
      index = skip_member(body, stop);
      continue;
    }
    std::size_t name_start = stop;
    while (name_start > head && std::isspace(static_cast<unsigned char>(body[name_start - 1]))) {
      --name_start;
    }
    const std::size_t name_end = name_start;
    while (name_start > head && is_ident_byte(body[name_start - 1])) --name_start;
    const std::string name(body.substr(name_start, name_end - name_start));
    const auto close = matching(body, stop);
    if (!close.has_value()) {
      parsed.skipped.emplace_back(name, "an unclosed parameter list");
      break;
    }
    std::size_t after = skip_ws(body, *close + 1);
    std::vector<std::string> throws;
    if (body.substr(after).starts_with("throws")) {
      const std::size_t start = after + 6;
      int throw_angle = 0;
      std::size_t end = body.size();
      for (std::size_t k = start; k < body.size(); ++k) {
        if (body[k] == '<') {
          ++throw_angle;
        } else if (body[k] == '>') {
          --throw_angle;
        } else if ((body[k] == '{' || body[k] == ';') && throw_angle <= 0) {
          end = k;
          break;
        }
      }
      for (std::string_view part : split_angles(body.substr(start, end - start), ',')) {
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
          part.remove_prefix(1);
        }
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) {
          part.remove_suffix(1);
        }
        if (!part.empty()) throws.emplace_back(part);
      }
      after = skip_ws(body, end);
    }
    const bool has_body = after < body.size() && body[after] == '{';
    if (has_body) {
      const auto end = matching(body, after);
      if (!end.has_value()) {
        parsed.skipped.emplace_back(name, "an unclosed method body");
        break;
      }
      after = *end + 1;
    } else if (after < body.size() && body[after] == ';') {
      ++after;
    }
    index = std::max(after, index + 1);

    std::uint32_t declaration_line = 1;
    for (std::size_t byte = 0; byte < base + decl_start && byte < blank.size(); ++byte) {
      if (blank[byte] == '\n') ++declaration_line;
    }
    const std::string doc = doc_before(source_lines, declaration_line - 1);

    const Modifiers modifiers = strip_modifiers(body.substr(head, name_start - head));
    if (modifiers.rest.empty()) {
      parsed.skipped.emplace_back(
          name, "a constructor: the recorder constructs no instance");
      continue;
    }
    if (modifiers.rest.starts_with('<')) {
      parsed.skipped.emplace_back(
          name, "a generic method: its type parameter has no wire form");
      continue;
    }
    if (modifiers.is_private) {
      parsed.skipped.emplace_back(
          name, "a private method: not part of the class's public surface");
      continue;
    }
    if (!modifiers.is_static) {
      parsed.skipped.emplace_back(
          name, "an instance method: the recorder calls public static methods only");
      continue;
    }
    if (!modifiers.public_access) {
      parsed.skipped.emplace_back(
          name, "not public: the recorder calls public static methods only");
      continue;
    }
    if (!has_body) {
      parsed.skipped.emplace_back(
          name, "a native or abstract method: there is no body to observe");
      continue;
    }
    std::string returns_raw = modifiers.rest;
    while (!returns_raw.empty() && std::isspace(static_cast<unsigned char>(returns_raw.front()))) {
      returns_raw.erase(returns_raw.begin());
    }
    while (!returns_raw.empty() && std::isspace(static_cast<unsigned char>(returns_raw.back()))) {
      returns_raw.pop_back();
    }
    if (returns_raw == "void") {
      parsed.skipped.emplace_back(name, "returns void: the wire carries values, not effects");
      continue;
    }
    const auto returns_ty = wire_type(returns_raw);
    if (!returns_ty.has_value()) {
      parsed.skipped.emplace_back(
          name, std::format("the return type `{}` has no wire form", returns_raw));
      continue;
    }
    std::vector<JavaParam> params;
    std::optional<std::string> refused;
    const std::string_view param_text = body.substr(stop + 1, *close - stop - 1);
    for (std::string_view part : split_angles(param_text, ',')) {
      bool empty = true;
      for (const char ch : part) {
        if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
      }
      if (empty) continue;
      auto parsed_param = parse_param(part);
      if (!parsed_param) {
        refused = parsed_param.error();
        break;
      }
      const auto [param_name, raw] = *parsed_param;
      const auto ty = wire_type(raw);
      if (!ty.has_value()) {
        refused = std::format("the parameter `{}` has type `{}`, which has no wire form",
                              param_name, raw);
        break;
      }
      params.push_back(JavaParam{param_name, raw, std::string(*ty)});
    }
    if (refused.has_value()) {
      parsed.skipped.emplace_back(name, *refused);
      continue;
    }
    if (std::ranges::any_of(parsed.functions,
                            [&](const JavaFunction& function) { return function.name == name; })) {
      parsed.skipped.emplace_back(
          name, "an overload: the wire names functions by name, so one signature per name");
      continue;
    }
    parsed.functions.push_back(JavaFunction{name, declaration_line, returns_raw,
                                            std::string(*returns_ty), std::move(params),
                                            std::move(throws), doc});
  }

  if (parsed.functions.empty() && parsed.skipped.empty()) {
    parsed.skipped.emplace_back("class " + chosen->name,
                                "no method declarations found in the class body");
  }
  return parsed;
}

}  // namespace

Result<Analysis> analyze(const std::filesystem::path& path) {
  auto parsed = parse_java(path);
  if (!parsed) return std::unexpected(parsed.error());
  Analysis analysis;
  for (const JavaFunction& function : parsed->functions) {
    Found found;
    found.name = function.name;
    found.line = function.line;
    found.call = parsed->class_name + "." + function.name;
    for (const JavaParam& param : function.params) {
      found.params.push_back(FoundParam{param.name, param.ty, false, std::nullopt});
    }
    found.returns = function.returns_ty;
    found.doc = function.doc;
    analysis.functions.push_back(std::move(found));
  }
  analysis.skipped = parsed->skipped;
  analysis.imports = parsed->imports;
  analysis.lines = parsed->lines;
  return analysis;
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

namespace {

std::string_view trim_space(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

/// The shortest round-trip digits and decimal exponent of `magnitude`, with
/// Rust's tie rule: when the exact value sits halfway between two shortest
/// decimals, round the last digit away from zero instead of to even.
std::pair<std::string, int> shortest_decimal(double magnitude) {
  char buffer[64];
  const auto result =
      std::to_chars(buffer, buffer + sizeof buffer, magnitude, std::chars_format::scientific);
  const std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
  const std::size_t marker = text.find('e');
  std::string digits;
  for (const char ch : text.substr(0, marker)) {
    if (ch != '.') digits += ch;
  }
  int exponent = std::stoi(std::string(text.substr(marker + 1)));

  // Enough digits past the shortest form to tell an exact halfway point from a
  // value merely near it: two distinct doubles are at least ~1e-17 apart.
  const std::size_t precision = digits.size() + 24;
  char wide[256];
  const auto wide_result = std::to_chars(wide, wide + sizeof wide, magnitude,
                                         std::chars_format::scientific,
                                         static_cast<int>(precision));
  const std::string_view wide_text(wide, static_cast<std::size_t>(wide_result.ptr - wide));
  const std::size_t wide_marker = wide_text.find('e');
  std::string wide_digits;
  for (const char ch : wide_text.substr(0, wide_marker)) {
    if (ch != '.') wide_digits += ch;
  }
  const std::size_t shortest = digits.size();
  bool tie = wide_digits.size() > shortest && wide_digits[shortest] == '5';
  for (std::size_t index = shortest + 1; tie && index < wide_digits.size(); ++index) {
    if (wide_digits[index] != '0') tie = false;
  }
  // std::to_chars rounds a halfway value to the even candidate; Rust rounds it
  // away from zero. Only the even-and-lower choice needs the increment.
  if (!tie || wide_digits.substr(0, shortest) != digits) {
    return {std::move(digits), exponent};
  }

  bool carry = true;
  for (std::size_t index = digits.size(); carry && index > 0; --index) {
    if (digits[index - 1] == '9') {
      digits[index - 1] = '0';
    } else {
      ++digits[index - 1];
      carry = false;
    }
  }
  if (carry) digits.insert(digits.begin(), '1');
  if (digits.size() > shortest) exponent += static_cast<int>(digits.size() - shortest);
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  return {std::move(digits), exponent};
}

/// Rust's `{:?}` for f64: shortest round-trip digits, decimal between 1e-4 and
/// 1e16, scientific outside that, always at least one fractional digit.
std::string debug_float(double value) {
  if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
  const bool negative = std::signbit(value);
  const double magnitude = negative ? -value : value;
  auto [digits, exponent] = shortest_decimal(magnitude);
  std::string out;
  if (negative) out += '-';
  if (exponent < -4 || exponent >= 16) {
    out += digits.front();
    if (digits.size() > 1) {
      out += '.';
      out += digits.substr(1);
    }
    out += 'e';
    out += std::to_string(exponent);
  } else if (exponent >= 0) {
    if (digits.size() <= static_cast<std::size_t>(exponent) + 1) {
      out += digits;
      out.append(static_cast<std::size_t>(exponent) + 1 - digits.size(), '0');
      out += ".0";
    } else {
      out += digits.substr(0, static_cast<std::size_t>(exponent) + 1);
      out += '.';
      out += digits.substr(static_cast<std::size_t>(exponent) + 1);
    }
  } else {
    out += "0.";
    out.append(static_cast<std::size_t>(-exponent) - 1, '0');
    out += digits;
  }
  return out;
}

std::pair<std::uint32_t, std::size_t> decode_utf8(std::string_view text, std::size_t index) {
  const unsigned char first = static_cast<unsigned char>(text[index]);
  std::size_t length = 1;
  std::uint32_t code = first;
  std::size_t minimum = 0;
  if (first >= 0xC2 && first <= 0xDF) {
    length = 2;
    code = first & 0x1F;
    minimum = 0x80;
  } else if (first >= 0xE0 && first <= 0xEF) {
    length = 3;
    code = first & 0x0F;
    minimum = 0x800;
  } else if (first >= 0xF0 && first <= 0xF4) {
    length = 4;
    code = first & 0x07;
    minimum = 0x10000;
  } else {
    return {first, 1};
  }
  if (index + length > text.size()) return {first, 1};
  for (std::size_t offset = 1; offset < length; ++offset) {
    const unsigned char next = static_cast<unsigned char>(text[index + offset]);
    if ((next & 0xC0) != 0x80) return {first, 1};
    code = (code << 6) | (next & 0x3F);
  }
  if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
    return {first, 1};
  }
  return {code, length};
}

std::string java_quote(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out += '"';
  const auto escape_unit = [&out](std::uint32_t unit) {
    out += std::format("\\u{:04x}", unit);
  };
  for (std::size_t index = 0; index < text.size();) {
    const auto [code, length] = decode_utf8(text, index);
    index += length;
    switch (code) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (code < 0x20 || code > 0x7e) {
          if (code > 0xFFFF) {
            const std::uint32_t rest = code - 0x10000;
            escape_unit(0xD800 + (rest >> 10));
            escape_unit(0xDC00 + (rest & 0x3FF));
          } else {
            escape_unit(code);
          }
        } else {
          out += static_cast<char>(code);
        }
    }
  }
  out += '"';
  return out;
}

std::string java_float_literal(double value, const std::string& raw) {
  const bool single = raw == "float";
  if (std::isnan(value)) return single ? "Float.NaN" : "Double.NaN";
  if (std::isinf(value)) {
    const std::string_view name =
        value > 0.0 ? "POSITIVE_INFINITY" : "NEGATIVE_INFINITY";
    return (single ? "Float." : "Double.") + std::string(name);
  }
  std::string text = debug_float(value);
  if (single) text += 'f';
  return text;
}

/// Rust's `raw.chars().filter(|ch| !ch.is_whitespace())`: the type without spaces.
std::string compact_type(std::string_view raw) {
  std::string out;
  for (const char ch : raw) {
    if (!std::isspace(static_cast<unsigned char>(ch))) out += ch;
  }
  return out;
}

std::string collection_literal(const std::string& raw, const std::string& base) {
  const std::string compact = compact_type(raw);
  if (compact.find("ArrayList") != std::string::npos ||
      compact.find("LinkedList") != std::string::npos) {
    return "new java.util.ArrayList<>(" + base + ")";
  }
  return base;
}

std::string map_literal(const std::string& raw, const std::string& base) {
  const std::string compact = compact_type(raw);
  if (compact.find("HashMap") != std::string::npos ||
      compact.find("LinkedHashMap") != std::string::npos) {
    return "new java.util.HashMap<>(" + base + ")";
  }
  return base;
}

std::optional<std::string> java_literal(const Json& value, const std::string& raw,
                                        const std::string& ty) {
  if (value.is_null()) return std::string("null");
  if (ty == "int") {
    if (!value.is_integer()) return std::nullopt;
    const std::string number = std::to_string(value.as_int());
    return raw == "long" ? number + "L" : number;
  }
  if (ty == "float") {
    if (!value.is_number()) return std::nullopt;
    return java_float_literal(value.as_number(), raw);
  }
  if (ty == "bool") {
    if (!value.is_bool()) return std::nullopt;
    return std::string(value.as_bool() ? "true" : "false");
  }
  if (ty == "str") {
    if (!value.is_string()) return std::nullopt;
    return java_quote(value.as_string());
  }
  if (ty == "list[str]") {
    if (!value.is_array()) return std::nullopt;
    std::string base = "java.util.List.of(";
    for (std::size_t index = 0; index < value.as_array().size(); ++index) {
      if (index > 0) base += ", ";
      const Json& item = value.as_array()[index];
      if (item.is_null()) {
        base += "null";
      } else if (item.is_string()) {
        base += java_quote(item.as_string());
      } else {
        return std::nullopt;
      }
    }
    base += ')';
    return collection_literal(raw, base);
  }
  if (ty == "dict[str]") {
    if (!value.is_object()) return std::nullopt;
    std::string base = "java.util.Map.of(";
    bool first = true;
    for (const auto& [key, item] : value.as_object()) {
      if (!first) base += ", ";
      first = false;
      base += java_quote(key);
      base += ", ";
      if (item.is_null()) {
        base += "null";
      } else if (item.is_string()) {
        base += java_quote(item.as_string());
      } else {
        return std::nullopt;
      }
    }
    base += ')';
    return map_literal(raw, base);
  }
  return std::nullopt;
}

std::string value_expression(const std::string& ty, const std::string& value,
                             const std::string& raw) {
  (void)raw;
  if (ty == "int") return "Long.toString((long) " + value + ")";
  if (ty == "float") return "numJson(" + value + ")";
  if (ty == "bool") return "Boolean.toString(" + value + ")";
  if (ty == "str") return "strJson(" + value + ")";
  if (ty == "list[str]") return "listJson(" + value + ")";
  if (ty == "dict[str]") return "mapJson(" + value + ")";
  std::string escaped = ty;
  for (char& ch : escaped) {
    if (ch == '"') ch = '\'';
  }
  return "opaque(\"" + escaped + "\")";
}

std::string args_expression(const std::vector<JavaParam>& params) {
  if (params.empty()) return "\"[]\"";
  std::string out = "\"[\"";
  for (std::size_t index = 0; index < params.size(); ++index) {
    out += index == 0 ? " + " : " + \",\" + ";
    out += value_expression(params[index].ty, params[index].name, params[index].raw);
  }
  out += " + \"]\"";
  return out;
}

std::string package_target(const Parsed& parsed, const JavaFunction& function,
                           const std::string& arguments) {
  std::string target;
  if (!parsed.package.empty()) target += parsed.package + ".";
  target += parsed.class_name + "." + function.name + "(" + arguments + ")";
  return target;
}

std::filesystem::path source_root(const std::filesystem::path& file, const std::string& package) {
  std::filesystem::path root = file.parent_path();
  std::size_t start = 0;
  while (start <= package.size()) {
    std::size_t end = package.find('.', start);
    if (end == std::string::npos) end = package.size();
    if (end > start) root = root.parent_path();
    if (end >= package.size()) break;
    start = end + 1;
  }
  return root;
}

Result<std::filesystem::path> compile_java(const Project& project, const Parsed& parsed,
                                           const std::vector<std::filesystem::path>& sources) {
  const std::filesystem::path classes = project.dir / "build" / "classes";
  std::error_code error;
  std::filesystem::remove_all(classes, error);
  error.clear();
  std::filesystem::create_directories(classes, error);
  if (error) return std::unexpected(classes.string() + ": " + error.message());
  const std::filesystem::path root = source_root(project.source_file, parsed.package);
  std::vector<std::string> args = {
      "-encoding",     "UTF-8",         "-proc:none", "-d",
      classes.string(), "-sourcepath",  root.string(),
  };
  for (const std::filesystem::path& source : sources) args.push_back(source.string());
  args.push_back(project.source_file.string());
  const auto run = run_harness(project, "javac", args, std::nullopt);
  if (!run) return std::unexpected(run.error());
  return classes;
}

Result<std::string> run_java(const Project& project, const std::filesystem::path& classes,
                             const std::vector<std::string>& extra) {
  std::vector<std::string> args = {"-Dstdout.encoding=UTF-8", "-cp", classes.string()};
  args.insert(args.end(), extra.begin(), extra.end());
  return run_harness(project, "java", args, std::nullopt);
}

Result<std::string> render_record(const Parsed& parsed, const std::vector<Json>& calls) {
  std::string out;
  out += "// Generated by cpp-anything record: one JSON line per call, no JSON parser needed.\n";
  out += "public final class Record {\n";
  out += "  interface Case { void run() throws Throwable; }\n\n";
  out += R"CPPAN(  static void run(String id, Case body) {
    try {
      body.run();
    } catch (Throwable error) {
      System.out.println("{\"id\":" + quote(id) + ",\"err\":{\"type\":"
          + quote(error.getClass().getSimpleName()) + ",\"msg\":" + quote(message(error)) + "}}");
    }
  }

  static String message(Throwable error) {
    String text = error.getMessage();
    if (text == null) return "";
    return text.length() > 200 ? text.substring(0, 200) : text;
  }

  static String quote(String text) {
    StringBuilder out = new StringBuilder("\"");
    for (int i = 0; i < text.length(); i++) {
      char c = text.charAt(i);
      switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
          if (c < 0x20) out.append(String.format("\\u%04x", (int) c));
          else out.append(c);
      }
    }
    out.append('"');
    return out.toString();
  }

  static String numJson(double value) {
    if (!Double.isFinite(value)) return "{\"$opaque\":\"Double\"}";
    return Double.toString(value);
  }

  static String strJson(String value) {
    return value == null ? "null" : quote(value);
  }

  static String listJson(java.util.Collection<String> values) {
    if (values == null) return "null";
    StringBuilder out = new StringBuilder("[");
    boolean first = true;
    for (String item : values) {
      if (!first) out.append(',');
      first = false;
      out.append(item == null ? "null" : quote(item));
    }
    return out.append(']').toString();
  }

  static String mapJson(java.util.Map<String, String> values) {
    if (values == null) return "null";
    java.util.TreeMap<String, String> sorted = new java.util.TreeMap<>(values);
    StringBuilder out = new StringBuilder("{");
    boolean first = true;
    for (java.util.Map.Entry<String, String> entry : sorted.entrySet()) {
      if (!first) out.append(',');
      first = false;
      out.append(quote(entry.getKey())).append(':');
      out.append(entry.getValue() == null ? "null" : quote(entry.getValue()));
    }
    return out.append('}').toString();
  }

  static String opaque(String type) {
    return "{\"$opaque\":" + quote(type) + "}";
  }

  public static void main(String[] args) {
)CPPAN";
  for (const Json& call : calls) {
    const Json* fn = call.find("fn");
    if (fn == nullptr || !fn->is_string()) return std::unexpected("a recorded call without fn");
    const std::string function_name = fn->as_string();
    const Json* id_field = call.find("id");
    const std::string id =
        id_field != nullptr && id_field->is_string() ? id_field->as_string() : function_name;
    std::vector<Json> args;
    if (const Json* value = call.find("args"); value != nullptr && value->is_array()) {
      args = value->as_array();
    }
    const JavaFunction* function = nullptr;
    for (const JavaFunction& candidate : parsed.functions) {
      if (candidate.name == function_name) {
        function = &candidate;
        break;
      }
    }
    if (function == nullptr) {
      return std::unexpected("the recorder has no supported Java signature for `" +
                             function_name + "`");
    }
    if (args.size() != function->params.size()) {
      return std::unexpected("`" + function_name + "` takes " +
                             std::to_string(function->params.size()) + " arguments, the call has " +
                             std::to_string(args.size()));
    }
    std::vector<std::string> literals;
    for (std::size_t index = 0; index < function->params.size(); ++index) {
      const JavaParam& param = function->params[index];
      const auto literal = java_literal(args[index], param.raw, param.ty);
      if (!literal.has_value()) {
        return std::unexpected("`" + function_name + "` cannot be written as a `" + param.raw +
                               "` literal in the Java harness: " + dump(args[index]));
      }
      literals.push_back(*literal);
    }
    std::string arguments;
    for (std::size_t index = 0; index < literals.size(); ++index) {
      if (index > 0) arguments += ", ";
      arguments += literals[index];
    }
    const std::string target = package_target(parsed, *function, arguments);
    const std::string id_literal = java_quote(id);
    out += "    run(";
    out += id_literal;
    out += ", () -> {\n      ";
    out += function->returns_raw + " cppanResult = " + target + ";\n";
    out += "      System.out.println(\"{\\\"id\\\":\" + quote(";
    out += id_literal;
    out += ") + \",\\\"ok\\\":\" + ";
    out += value_expression(function->returns_ty, "cppanResult", function->returns_raw);
    out += " + \"}\");\n    });\n";
  }
  out += "  }\n}\n";
  return out;
}

Result<std::string> render_bench(const Parsed& parsed, const Json& chosen) {
  std::vector<std::string> calls;
  for (const JavaFunction& function : parsed.functions) {
    const Json* doc = chosen.find(function.name);
    if (doc == nullptr || !doc->is_array()) continue;
    std::vector<std::string> literals;
    for (std::size_t index = 0; index < function.params.size(); ++index) {
      const JavaParam& param = function.params[index];
      if (index >= doc->as_array().size()) break;
      const auto literal = java_literal(doc->as_array()[index], param.raw, param.ty);
      if (!literal.has_value()) {
        return std::unexpected("`" + function.name + "` cannot be written as a `" + param.raw +
                               "` literal in the Java harness");
      }
      literals.push_back(*literal);
    }
    std::string arguments;
    for (std::size_t index = 0; index < literals.size(); ++index) {
      if (index > 0) arguments += ", ";
      arguments += literals[index];
    }
    calls.push_back(package_target(parsed, function, arguments));
  }
  std::string out;
  out += "// Generated by cpp-anything bench: warm up once, then time the whole loop.\n";
  out += "public final class Bench {\n";
  out += "  public static void main(String[] args) {\n";
  out += "    int repeat = Integer.parseInt(args[0]);\n";
  out += "    for (int cppanWarm = 0; cppanWarm < 1; cppanWarm++) {\n";
  for (const std::string& target : calls) {
    out += "      try { " + target + "; } catch (Throwable cppanIgnored) {}\n";
  }
  out += "    }\n";
  out += "    long started = System.nanoTime();\n";
  out += "    int calls = 0;\n";
  out += "    for (int cppanI = 0; cppanI < repeat; cppanI++) {\n";
  for (const std::string& target : calls) {
    out += "      try { " + target + "; } catch (Throwable cppanIgnored) {}\n";
  }
  out += "      calls += " + std::to_string(calls.size()) + ";\n";
  out += "    }\n";
  out += "    long elapsed = System.nanoTime() - started;\n";
  out +=
      "    System.out.println(\"{\\\"ns\\\":\" + elapsed + \",\\\"calls\\\":\" + calls + "
      "\"}\");\n";
  out += "  }\n}\n";
  return out;
}

}  // namespace

Result<std::size_t> record(const Project& project, const Unit& unit,
                           const std::vector<Json>& calls) {
  (void)unit;
  const auto parsed = parse_java(project.source_file);
  if (!parsed) return std::unexpected(parsed.error());
  auto harness = render_record(*parsed, calls);
  if (!harness) return std::unexpected(harness.error());
  const std::filesystem::path harness_path = project.dir / "build" / "Record.java";
  if (auto written = write_file(harness_path, *harness); !written) {
    return std::unexpected(written.error());
  }
  auto classes = compile_java(project, *parsed, {harness_path});
  if (!classes) return std::unexpected(classes.error());
  auto output = run_java(project, *classes, {"Record"});
  if (!output) return std::unexpected(output.error());
  return write_cases(project, calls, *output);
}

Result<double> bench(const Project& project, std::size_t repeat) {
  const auto parsed = parse_java(project.source_file);
  if (!parsed) return std::unexpected(parsed.error());
  const auto text = read_file(project.dir / "cases.jsonl");
  if (!text.has_value()) {
    return std::unexpected((project.dir / "cases.jsonl").string() + ": cannot read");
  }
  std::map<std::string, std::pair<bool, Json>> first;
  for (const std::string_view line : split_lines(*text)) {
    std::string_view trimmed = trim_space(line);
    if (trimmed.empty()) continue;
    Json doc;
    try {
      doc = cppan::parse(trimmed);
    } catch (const JsonError& error) {
      return std::unexpected(error.what());
    }
    const Json* name_field = doc.find("fn");
    if (name_field == nullptr || !name_field->is_string()) continue;
    const std::string name = name_field->as_string();
    const Json* expect = doc.find("expect");
    const bool ok = expect != nullptr && expect->find("ok") != nullptr;
    const auto found = first.find(name);
    if (found == first.end()) {
      first.emplace(name, std::pair{ok, doc});
    } else if (ok && !found->second.first) {
      found->second = std::pair{ok, doc};
    }
  }
  std::map<std::string, std::vector<Json>> chosen;
  for (const auto& [name, entry] : first) {
    std::vector<Json> args;
    if (const Json* value = entry.second.find("args");
        value != nullptr && value->is_array()) {
      args = value->as_array();
    }
    chosen.emplace(name, std::move(args));
  }
  std::vector<std::pair<std::string, Json>> pairs;
  pairs.reserve(chosen.size());
  for (const auto& [name, args] : chosen) pairs.emplace_back(name, Json::array(args));
  const Json chosen_json = Json::object(std::move(pairs));
  auto harness = render_bench(*parsed, chosen_json);
  if (!harness) return std::unexpected(harness.error());
  const std::filesystem::path harness_path = project.dir / "build" / "Bench.java";
  if (auto written = write_file(harness_path, *harness); !written) {
    return std::unexpected(written.error());
  }
  auto classes = compile_java(project, *parsed, {harness_path});
  if (!classes) return std::unexpected(classes.error());
  auto output = run_java(project, *classes, {"Bench", std::to_string(repeat)});
  if (!output) return std::unexpected(output.error());
  Json doc;
  try {
    doc = cppan::parse(trim_space(*output));
  } catch (const JsonError& error) {
    return std::unexpected(error.what());
  }
  const Json* ns_field = doc.find("ns");
  const double ns =
      ns_field != nullptr && ns_field->is_number() ? ns_field->as_number() : 0.0;
  const Json* calls_field = doc.find("calls");
  const double calls =
      calls_field != nullptr && calls_field->is_number() ? calls_field->as_number() : 1.0;
  if (calls <= 0.0) return std::unexpected("the Java benchmark ran no calls");
  return ns / calls;
}

// ---------------------------------------------------------------------------
// shadow
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> shadow_files(const Project& project,
                                                              std::string_view ns) {
  (void)ns;
  const auto parsed = parse_java(project.source_file);
  if (!parsed) return {};
  const std::string log = shadow::log_path(project).string();
  const std::string qualified =
      parsed->package.empty() ? parsed->class_name : parsed->package + "." + parsed->class_name;
  std::string out;
  out += "// Generated by cpp-anything shadow: the original runs first, the port second,\n";
  out += "// and callers always see the original's result. Disagreements go to the log.\n";
  out += "import java.math.BigDecimal;\n";
  out += "import java.time.Instant;\n";
  out += "import java.util.ArrayList;\n";
  out += "import java.util.LinkedHashMap;\n";
  out += "import java.util.List;\n";
  out += "import java.util.Map;\n\n";
  out += "public final class CppanShadow {\n";
  out += "  static { System.loadLibrary(\"cppan_shadow_jni\"); }\n\n";
  out += "  public static native String call(String fn, String argsJson);\n\n";
  out += "  private static final String LOG = System.getenv(\"CPPAN_SHADOW_LOG\") != null\n";
  out += "      ? System.getenv(\"CPPAN_SHADOW_LOG\")\n";
  out += "      : " + java_quote(log) + ";\n";
  out += "  private static final boolean OFF = \"off\".equals(System.getenv(\"CPPAN_SHADOW\"));\n\n";
  out += "  private CppanShadow() {}\n\n";
  out += R"CPPAN(  static <T extends Throwable> T sneak(Throwable error) throws T {
    throw (T) error;
  }

  static String quote(String text) {
    StringBuilder out = new StringBuilder("\"");
    for (int i = 0; i < text.length(); i++) {
      char c = text.charAt(i);
      switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
          if (c < 0x20) out.append(String.format("\\u%04x", (int) c));
          else out.append(c);
      }
    }
    out.append('"');
    return out.toString();
  }

  static String numJson(double value) {
    if (!Double.isFinite(value)) return "{\"$opaque\":\"Double\"}";
    return Double.toString(value);
  }

  static String strJson(String value) {
    return value == null ? "null" : quote(value);
  }

  static String listJson(java.util.Collection<String> values) {
    if (values == null) return "null";
    StringBuilder out = new StringBuilder("[");
    boolean first = true;
    for (String item : values) {
      if (!first) out.append(',');
      first = false;
      out.append(item == null ? "null" : quote(item));
    }
    return out.append(']').toString();
  }

  static String mapJson(java.util.Map<String, String> values) {
    if (values == null) return "null";
    java.util.TreeMap<String, String> sorted = new java.util.TreeMap<>(values);
    StringBuilder out = new StringBuilder("{");
    boolean first = true;
    for (java.util.Map.Entry<String, String> entry : sorted.entrySet()) {
      if (!first) out.append(',');
      first = false;
      out.append(quote(entry.getKey())).append(':');
      out.append(entry.getValue() == null ? "null" : quote(entry.getValue()));
    }
    return out.append('}').toString();
  }

  static String nativeResult(String fn, String args) {
    try {
      return call(fn, args);
    } catch (Throwable error) {
      return "{\"err\":{\"type\":" + quote(error.getClass().getSimpleName())
          + ",\"msg\":" + quote(String.valueOf(error.getMessage())) + "}}";
    }
  }

  static void check(String fn, String args, String expected) {
    if (OFF) return;
    String got = nativeResult(fn, args);
    String reason = compare(expected, got);
    if (reason != null) log(fn, args, expected, got, reason);
  }

  static void checkError(String fn, String args, Throwable error) {
    if (OFF) return;
    String got = nativeResult(fn, args);
    String expected = "{\"err\":{\"type\":" + quote(error.getClass().getSimpleName())
        + ",\"msg\":" + quote(String.valueOf(error.getMessage())) + "}}";
    String reason = compare(expected, got);
    if (reason != null) log(fn, args, expected, got, reason);
  }

  static void log(String fn, String args, String expected, String got, String reason) {
    try (java.io.FileWriter file = new java.io.FileWriter(LOG, true)) {
      file.write("{\"at\":" + quote(Instant.now().toString())
          + ",\"fn\":" + quote(fn)
          + ",\"args\":" + args
          + ",\"expected\":" + expected
          + ",\"got\":" + got
          + ",\"reason\":" + quote(reason) + "}\n");
    } catch (java.io.IOException ignored) {
    }
  }

  static String compare(String expectedJson, String gotJson) {
    Object expected;
    Object got;
    try {
      expected = parseJson(expectedJson);
      got = parseJson(gotJson);
    } catch (RuntimeException error) {
      return expectedJson.equals(gotJson) ? null : "values differ";
    }
    if (!(expected instanceof Map) || !(got instanceof Map)) {
      return sameValue(expected, got) ? null : "values differ";
    }
    Map<?, ?> left = (Map<?, ?>) expected;
    Map<?, ?> right = (Map<?, ?>) got;
    Object leftError = left.get("err");
    Object rightError = right.get("err");
    if (leftError != null || rightError != null) {
      if (leftError == null) return "the original returns a value, the port raises";
      if (rightError == null) return "the original raises, the port returns a value";
      String expectedType = errorType(leftError);
      String gotType = errorType(rightError);
      if (expectedType.equals(gotType)) return null;
      return "error type: expected " + expectedType + ", got " + gotType;
    }
    Object leftOk = left.containsKey("ok") ? left.get("ok") : null;
    Object rightOk = right.containsKey("ok") ? right.get("ok") : null;
    return sameValue(leftOk, rightOk) ? null : "values differ";
  }

  static String errorType(Object error) {
    if (error instanceof Map && ((Map<?, ?>) error).get("type") instanceof String) {
      return (String) ((Map<?, ?>) error).get("type");
    }
    return "?";
  }

  static boolean sameValue(Object left, Object right) {
    if (left == null || right == null) return left == null && right == null;
    if (left instanceof BigDecimal && right instanceof BigDecimal) {
      return ((BigDecimal) left).compareTo((BigDecimal) right) == 0;
    }
    if (left instanceof Double && right instanceof Double) {
      double a = (Double) left;
      double b = (Double) right;
      return (Double.isNaN(a) && Double.isNaN(b)) || Double.compare(a, b) == 0;
    }
    if (left instanceof Map && right instanceof Map) {
      Map<?, ?> a = (Map<?, ?>) left;
      Map<?, ?> b = (Map<?, ?>) right;
      if (!a.keySet().equals(b.keySet())) return false;
      for (Object key : a.keySet()) {
        if (!sameValue(a.get(key), b.get(key))) return false;
      }
      return true;
    }
    if (left instanceof List && right instanceof List) {
      List<?> a = (List<?>) left;
      List<?> b = (List<?>) right;
      if (a.size() != b.size()) return false;
      for (int i = 0; i < a.size(); i++) {
        if (!sameValue(a.get(i), b.get(i))) return false;
      }
      return true;
    }
    return left.equals(right);
  }

  static Object parseJson(String text) {
    Parser parser = new Parser(text);
    Object value = parser.value();
    parser.space();
    if (parser.pos != text.length()) throw new IllegalArgumentException("trailing input");
    return value;
  }

  static final class Parser {
    final String text;
    int pos;

    Parser(String text) {
      this.text = text;
    }

    void space() {
      while (pos < text.length() && Character.isWhitespace(text.charAt(pos))) pos++;
    }

    Object value() {
      space();
      if (pos >= text.length()) throw new IllegalArgumentException("empty");
      char c = text.charAt(pos);
      switch (c) {
        case '{': return object();
        case '[': return array();
        case '"': return string();
        case 't': literal("true"); return Boolean.TRUE;
        case 'f': literal("false"); return Boolean.FALSE;
        case 'n': literal("null"); return null;
        default: return number();
      }
    }

    void literal(String expected) {
      if (!text.startsWith(expected, pos)) throw new IllegalArgumentException("literal");
      pos += expected.length();
    }

    Object number() {
      if (text.startsWith("NaN", pos)) { pos += 3; return Double.NaN; }
      if (text.startsWith("Infinity", pos)) { pos += 8; return Double.POSITIVE_INFINITY; }
      if (text.startsWith("-Infinity", pos)) { pos += 9; return Double.NEGATIVE_INFINITY; }
      int start = pos;
      while (pos < text.length() && "+-0123456789.eE".indexOf(text.charAt(pos)) >= 0) pos++;
      if (start == pos) throw new IllegalArgumentException("number");
      return new BigDecimal(text.substring(start, pos));
    }

    Object string() {
      StringBuilder out = new StringBuilder();
      pos++;
      while (pos < text.length()) {
        char c = text.charAt(pos++);
        if (c == '"') return out.toString();
        if (c != '\\') { out.append(c); continue; }
        char escape = text.charAt(pos++);
        switch (escape) {
          case '"': out.append('"'); break;
          case '\\': out.append('\\'); break;
          case '/': out.append('/'); break;
          case 'b': out.append('\b'); break;
          case 'f': out.append('\f'); break;
          case 'n': out.append('\n'); break;
          case 'r': out.append('\r'); break;
          case 't': out.append('\t'); break;
          case 'u':
            out.append((char) Integer.parseInt(text.substring(pos, pos + 4), 16));
            pos += 4;
            break;
          default: throw new IllegalArgumentException("escape");
        }
      }
      throw new IllegalArgumentException("unterminated string");
    }

    Object array() {
      List<Object> out = new ArrayList<>();
      pos++;
      space();
      if (pos < text.length() && text.charAt(pos) == ']') { pos++; return out; }
      while (true) {
        out.add(value());
        space();
        char c = text.charAt(pos++);
        if (c == ']') return out;
        if (c != ',') throw new IllegalArgumentException("array");
      }
    }

    Object object() {
      Map<String, Object> out = new LinkedHashMap<>();
      pos++;
      space();
      if (pos < text.length() && text.charAt(pos) == '}') { pos++; return out; }
      while (true) {
        space();
        String key = (String) string();
        space();
        if (text.charAt(pos++) != ':') throw new IllegalArgumentException("object");
        out.put(key, value());
        space();
        char c = text.charAt(pos++);
        if (c == '}') return out;
        if (c != ',') throw new IllegalArgumentException("object");
      }
    }
  }
)CPPAN";
  for (const JavaFunction& function : parsed->functions) {
    std::string params;
    std::string names;
    for (std::size_t index = 0; index < function.params.size(); ++index) {
      if (index > 0) {
        params += ", ";
        names += ", ";
      }
      params += function.params[index].raw + " " + function.params[index].name;
      names += function.params[index].name;
    }
    std::string throws;
    if (!function.throws.empty()) {
      throws = " throws ";
      for (std::size_t index = 0; index < function.throws.size(); ++index) {
        if (index > 0) throws += ", ";
        throws += function.throws[index];
      }
    }
    const std::string args = args_expression(function.params);
    out += "\n  public static " + function.returns_raw + " " + function.name + "(" + params + ")" +
           throws + " {\n";
    out += "\n    String cppanArgs = " + args + ";\n";
    out += "    " + function.returns_raw + " cppanValue;\n";
    out += "    try {\n      cppanValue = " + qualified + "." + function.name + "(" + names +
           ");\n    } catch (Throwable cppanError) {\n      checkError(\"" + function.name +
           "\", cppanArgs, cppanError);\n      throw CppanShadow.<RuntimeException>sneak("
           "cppanError);\n    }\n";
    out += "    check(\"" + function.name + "\", cppanArgs, \"{\\\"ok\\\":\" + ";
    out += value_expression(function.returns_ty, "cppanValue", function.returns_raw);
    out += " + \"}\");\n    return cppanValue;\n  }\n";
  }
  out += "}\n";

  const std::string jni = R"CPPAN(// Generated by cpp-anything shadow: the JNI entry point over the C ABI.
//
// Strings cross through JNI's modified UTF-8 (GetStringUTFChars/NewStringUTF): for
// ordinary text that is standard UTF-8, but U+0000 and supplementary characters
// (surrogate pairs) use a different encoding, so a JSON argument containing those
// must not be assumed byte-identical to the wire's UTF-8.
#include <jni.h>

extern "C" {
const char* cppan_shadow_call(const char* fn, const char* args_json);
void cppan_shadow_free(const char* ptr);
}

extern "C" JNIEXPORT jstring JNICALL Java_CppanShadow_call(JNIEnv* env, jclass, jstring fn,
                                                          jstring args) {
  if (fn == nullptr || args == nullptr) {
    return env->NewStringUTF(
        "{\"err\":{\"type\":\"TypeError\",\"msg\":\"missing function or arguments\"}}");
  }
  const char* fn_chars = env->GetStringUTFChars(fn, nullptr);
  const char* args_chars = env->GetStringUTFChars(args, nullptr);
  if (fn_chars == nullptr || args_chars == nullptr) {
    if (fn_chars != nullptr) env->ReleaseStringUTFChars(fn, fn_chars);
    if (args_chars != nullptr) env->ReleaseStringUTFChars(args, args_chars);
    return env->NewStringUTF(
        "{\"err\":{\"type\":\"OutOfMemoryError\",\"msg\":\"GetStringUTFChars\"}}");
  }
  const char* result = cppan_shadow_call(fn_chars, args_chars);
  env->ReleaseStringUTFChars(fn, fn_chars);
  env->ReleaseStringUTFChars(args, args_chars);
  if (result == nullptr) {
    return env->NewStringUTF("{\"err\":{\"type\":\"ShadowError\",\"msg\":\"no result\"}}");
  }
  jstring out = env->NewStringUTF(result);
  cppan_shadow_free(result);
  return out;
}
)CPPAN";

  const std::string build = R"CPPAN(#!/bin/sh
# Generated by cpp-anything shadow. Builds the JNI bridge against the port's
# libcppan_shadow.so (built by `cpp-anything build <project>`).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -n "${JAVA_HOME:-}" ] && [ -f "$JAVA_HOME/include/jni.h" ]; then
  java_home=$JAVA_HOME
else
  javac_path=$(command -v javac || true)
  if [ -z "$javac_path" ]; then
    echo "build.sh: javac is not on PATH and JAVA_HOME is unset" >&2
    exit 1
  fi
  resolved=$(readlink -f "$javac_path" 2>/dev/null || printf '%s' "$javac_path")
  java_home=$(dirname "$(dirname "$resolved")")
fi
platform=linux
if [ "$(uname -s)" = "Darwin" ]; then
  platform=darwin
fi
suffix=so
if [ "$(uname -s)" = "Darwin" ]; then
  suffix=dylib
fi
cxx=${CXX:-c++}
"$cxx" -std=c++17 -O2 -fPIC -shared \
  -I"$java_home/include" -I"$java_home/include/$platform" \
  "$here/cppan_shadow_jni.cpp" \
  -o "$here/libcppan_shadow_jni.$suffix" \
  -L"$here/../build" -lcppan_shadow -Wl,-rpath,"$here/../build"
printf 'built %s\n' "$here/libcppan_shadow_jni.$suffix"
)CPPAN";

  std::vector<std::pair<std::string, std::string>> files;
  files.emplace_back("shadow/CppanShadow.java", std::move(out));
  files.emplace_back("shadow/cppan_shadow_jni.cpp", jni);
  files.emplace_back("shadow/build.sh", build);
  return files;
}

}  // namespace cppan::langs::java
