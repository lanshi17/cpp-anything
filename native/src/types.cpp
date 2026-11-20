#include "cppan/types.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace cppan {
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

std::optional<std::string_view> strip_generic(std::string_view text, std::string_view name) {
  if (text.size() > name.size() + 2 && text.starts_with(name) && text[name.size()] == '[' &&
      text.back() == ']') {
    return text.substr(name.size() + 1, text.size() - name.size() - 2);
  }
  return std::nullopt;
}

std::optional<std::pair<std::string_view, std::string_view>> split_union(std::string_view text) {
  const auto parts = split_top_level(text, '|');
  if (parts.size() != 2) return std::nullopt;
  return std::pair{trim(parts[0]), trim(parts[1])};
}

std::string join_strings(const std::vector<std::string>& items, std::string_view separator) {
  std::string out;
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index > 0) out += separator;
    out += items[index];
  }
  return out;
}

std::string format_real(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
  if (value == 0.0 && std::signbit(value)) return "-0.0";
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
  const std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
  if (text.find_first_of(".eE") != std::string_view::npos) return std::string(text);
  return std::string(text) + ".0";
}

}  // namespace

std::vector<std::string_view> split_top_level(std::string_view text, char separator) {
  std::vector<std::string_view> out;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == '[' || ch == '(' || ch == '{') {
      ++depth;
    } else if (ch == ']' || ch == ')' || ch == '}') {
      --depth;
    } else if (ch == separator && depth == 0) {
      out.push_back(text.substr(start, index - start));
      start = index + 1;
    }
  }
  out.push_back(text.substr(start));
  return out;
}

Ty Ty::parse(std::string_view text) {
  text = trim(text);
  if (text.empty() || text == "Any" || text == "object" || text == "...") return Ty{};
  if (const auto inner = strip_generic(text, "Optional")) {
    return Ty{Kind::Optional, {Ty::parse(*inner)}};
  }
  if (const auto parts = split_union(text)) {
    if (parts->second == "None" || parts->first == "None") {
      const std::string_view inner = parts->second == "None" ? parts->first : parts->second;
      return Ty{Kind::Optional, {Ty::parse(inner)}};
    }
    return Ty{};
  }
  for (const std::string_view container : {"list", "Sequence", "Iterable", "set"}) {
    if (const auto inner = strip_generic(text, container)) {
      return Ty{Kind::List, {Ty::parse(*inner)}};
    }
  }
  if (const auto inner = strip_generic(text, "dict")) {
    const auto parts = split_top_level(*inner, ',');
    std::string_view value = *inner;
    if (parts.size() > 1) value = parts[1];
    return Ty{Kind::Dict, {Ty::parse(value)}};
  }
  if (const auto inner = strip_generic(text, "tuple")) {
    std::vector<Ty> items;
    for (std::string_view item : split_top_level(*inner, ',')) {
      const std::string_view trimmed = trim(item);
      if (trimmed.empty() || trimmed == "...") continue;
      items.push_back(Ty::parse(trimmed));
    }
    return Ty{Kind::Tuple, std::move(items)};
  }
  if (text == "str") return Ty{Kind::Str, {}};
  if (text == "int") return Ty{Kind::Int, {}};
  if (text == "float") return Ty{Kind::Float, {}};
  if (text == "bool") return Ty{Kind::Bool, {}};
  if (text == "None" || text == "NoneType") return Ty{Kind::None, {}};
  return Ty{};
}

std::string Ty::cpp_param(bool top_level) const {
  switch (kind) {
    case Kind::Str: return "std::string_view";
    case Kind::Int: return "std::int64_t";
    case Kind::Float: return "double";
    case Kind::Bool: return "bool";
    case Kind::None: return "const cppan::Json&";
    case Kind::List:
      if (top_level) return "std::span<const " + items.at(0).cpp_value() + ">";
      return "const std::vector<" + items.at(0).cpp_param(false) + ">&";
    case Kind::Dict: return "const std::map<std::string, " + items.at(0).cpp_param(false) + ">&";
    case Kind::Tuple: {
      std::vector<std::string> parts;
      for (const Ty& item : items) parts.push_back(item.cpp_param(false));
      return "const std::tuple<" + join_strings(parts, ", ") + ">&";
    }
    case Kind::Optional: return "std::optional<" + items.at(0).cpp_param(false) + ">";
    case Kind::Any: break;
  }
  return "const cppan::Json&";
}

std::string Ty::cpp_value() const {
  switch (kind) {
    case Kind::List: return "std::vector<" + items.at(0).cpp_value() + ">";
    case Kind::Dict: return "std::map<std::string, " + items.at(0).cpp_value() + ">";
    case Kind::Tuple: {
      std::vector<std::string> parts;
      for (const Ty& item : items) parts.push_back(item.cpp_value());
      return "std::tuple<" + join_strings(parts, ", ") + ">";
    }
    case Kind::Optional: return "std::optional<" + items.at(0).cpp_value() + ">";
    default: return cpp_param(false);
  }
}

std::string Ty::cpp_storage() const {
  switch (kind) {
    case Kind::List: return "std::vector<" + items.at(0).cpp_value() + ">";
    case Kind::Any: return "cppan::Json";
    default: return cpp_param(false);
  }
}

std::string Ty::cpp_return() const {
  switch (kind) {
    case Kind::Str: return "std::string";
    case Kind::Int: return "std::int64_t";
    case Kind::Float: return "double";
    case Kind::Bool: return "bool";
    case Kind::None: return "cppan::Json";
    case Kind::List: return "std::vector<" + items.at(0).cpp_return() + ">";
    case Kind::Dict: return "std::map<std::string, " + items.at(0).cpp_return() + ">";
    case Kind::Tuple: {
      std::vector<std::string> parts;
      for (const Ty& item : items) parts.push_back(item.cpp_return());
      return "std::tuple<" + join_strings(parts, ", ") + ">";
    }
    case Kind::Optional: return "std::optional<" + items.at(0).cpp_return() + ">";
    case Kind::Any: break;
  }
  return "cppan::Json";
}

std::string Ty::label() const {
  switch (kind) {
    case Kind::Str: return "str";
    case Kind::Int: return "int";
    case Kind::Float: return "float";
    case Kind::Bool: return "bool";
    case Kind::None: return "None";
    case Kind::List: return "list[" + items.at(0).label() + "]";
    case Kind::Dict: return "dict[str, " + items.at(0).label() + "]";
    case Kind::Tuple: {
      std::vector<std::string> parts;
      for (const Ty& item : items) parts.push_back(item.label());
      return "tuple[" + join_strings(parts, ", ") + "]";
    }
    case Kind::Optional: return "Optional[" + items.at(0).label() + "]";
    case Kind::Any: break;
  }
  return "Any";
}

void Ty::headers(std::vector<std::string>& out) const {
  const auto need = [&](std::string_view header) {
    if (std::ranges::find(out, header) == out.end()) out.emplace_back(header);
  };
  switch (kind) {
    case Kind::Str: need("<string_view>"); break;
    case Kind::Int: need("<cstdint>"); break;
    case Kind::Float: need("<cmath>"); break;
    case Kind::Bool: break;
    case Kind::None: break;
    case Kind::List:
      need("<span>");
      need("<vector>");
      items.at(0).headers(out);
      break;
    case Kind::Dict:
      need("<map>");
      need("<string>");
      items.at(0).headers(out);
      break;
    case Kind::Tuple:
      need("<tuple>");
      for (const Ty& item : items) item.headers(out);
      break;
    case Kind::Optional:
      need("<optional>");
      items.at(0).headers(out);
      break;
    case Kind::Any: break;
  }
}

std::optional<std::string> Ty::cpp_default(const Json& value) const {
  if (value.is_null()) return "std::nullopt";
  if (kind == Kind::Int && value.is_integer()) return std::to_string(value.as_int());
  if (kind == Kind::Float && value.is_real()) return format_real(value.as_real());
  if (kind == Kind::Float && value.is_integer()) return std::to_string(value.as_int()) + ".0";
  if (kind == Kind::Bool && value.is_bool()) return value.as_bool() ? "true" : "false";
  if (kind == Kind::Str && value.is_string()) return cpp_string_literal(value.as_string());
  if (kind == Kind::Optional) return items.at(0).cpp_default(value);
  if (kind == Kind::List && value.is_array() && value.as_array().empty()) return "{}";
  if (kind == Kind::Dict && value.is_object() && value.as_object().empty()) return "{}";
  return std::nullopt;
}

std::string cpp_string_literal(std::string_view text) {
  std::string out = "\"";
  for (const char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\x%02x", static_cast<unsigned>(ch));
          out += buffer;
        } else {
          out += ch;
        }
    }
  }
  out += '"';
  return out;
}

}  // namespace cppan
