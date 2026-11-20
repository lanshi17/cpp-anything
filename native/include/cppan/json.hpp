// cppan::Json — a small, dependency-free JSON codec for cpp-anything drivers.
//
// Generated projects embed this file. It exists because the driver must speak the
// same JSON as the recorder, exactly: ints stay ints, reals stay reals, keys keep
// their order, NaN/Infinity round-trip, and `-0.0` is distinguishable from `0.0`.
// A general-purpose library (nlohmann, RapidJSON) would do all of that too, but
// would add a dependency to every port; the port itself never sees this file.
//
// C++23 throughout: string_view, span, expected, ranges, concepts, designated
// initializers, [[nodiscard]].
#pragma once

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cppan {

class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string& what) : std::runtime_error(what) {}
};

class Json {
 public:
  enum class Kind : std::uint8_t { Null, Bool, Int, Real, String, Array, Object };

  using Array = std::vector<Json>;
  using Object = std::vector<std::pair<std::string, Json>>;

  Json() = default;

  static Json null() { return Json{}; }
  static Json boolean(bool value);
  static Json integer(std::int64_t value);
  static Json real(double value);
  static Json string(std::string value);
  static Json array(Array value = {});
  static Json object(Object value = {});

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return kind_ == Kind::Bool; }
  [[nodiscard]] bool is_number() const noexcept {
    return kind_ == Kind::Int || kind_ == Kind::Real;
  }
  [[nodiscard]] bool is_integer() const noexcept { return kind_ == Kind::Int; }
  [[nodiscard]] bool is_real() const noexcept { return kind_ == Kind::Real; }
  [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::String; }
  [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::Array; }
  [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::Object; }

  [[nodiscard]] bool as_bool() const;
  [[nodiscard]] std::int64_t as_int() const;
  [[nodiscard]] double as_real() const;
  // Ints and reals both widen; Python's `1` and `1.0` compare equal here but not
  // in `equal()`, which is the check the diff uses.
  [[nodiscard]] double as_number() const;
  [[nodiscard]] const std::string& as_string() const;
  [[nodiscard]] const Array& as_array() const;
  [[nodiscard]] const Object& as_object() const;

  [[nodiscard]] const Json* find(std::string_view key) const;
  [[nodiscard]] const Json& at(std::size_t index) const;
  [[nodiscard]] std::size_t size() const;

  void push(Json value);

  // Objects compare unordered (a Python dict becomes std::map); arrays ordered.
  friend bool operator==(const Json& lhs, const Json& rhs) { return equal(lhs, rhs); }
  friend bool operator!=(const Json& lhs, const Json& rhs) { return !equal(lhs, rhs); }

  static bool equal(const Json& lhs, const Json& rhs, double tolerance = 0.0);

 private:
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  std::int64_t int_ = 0;
  double real_ = 0.0;
  std::string string_;
  Array array_;
  Object object_;
};

// Parses one JSON value. Accepts the non-standard literals the Python recorder
// writes: NaN, Infinity, -Infinity. Rejects trailing input.
[[nodiscard]] Json parse(std::string_view text);

// Serialises; `pretty` is for humans and never used on the wire.
[[nodiscard]] std::string dump(const Json& value, bool pretty = false);

// Shortest text that round-trips, for error messages.
[[nodiscard]] std::string repr(const Json& value, std::size_t budget = 120);

namespace detail {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Json run();
  void expect_eof();

 private:
  [[nodiscard]] char peek() const;
  [[nodiscard]] bool at_end() const { return pos_ >= text_.size(); }
  void skip_space();
  void expect(char wanted);
  [[noreturn]] void fail(const std::string& message) const;

  Json parse_value();
  Json parse_object();
  Json parse_array();
  std::string parse_string();
  Json parse_number();
  Json parse_literal();
  [[nodiscard]] unsigned parse_hex4();
  static void append_utf8(std::string& out, std::uint32_t cp);

  std::string_view text_;
  std::size_t pos_ = 0;
};

inline void append_escaped(std::string& out, char ch) {
  switch (ch) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        char buffer[7];
        std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(ch));
        out += buffer;
      } else {
        out += ch;
      }
  }
}

inline void dump_into(std::string& out, const Json& value, int indent, int depth) {
  const auto newline = [&](int level) {
    if (indent <= 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent * level), ' ');
  };
  auto number = [&](double v) {
    if (std::isnan(v)) {
      out += "NaN";
    } else if (std::isinf(v)) {
      out += v > 0 ? "Infinity" : "-Infinity";
    } else {
      char buffer[40];
      const auto result = std::to_chars(buffer, buffer + sizeof buffer, v);
      const std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
      out.append(text);
      if (text.find_first_of(".eE") == std::string_view::npos) out += ".0";
    }
  };

  switch (value.kind()) {
    case Json::Kind::Null: out += "null"; break;
    case Json::Kind::Bool: out += value.as_bool() ? "true" : "false"; break;
    case Json::Kind::Int: {
      char buffer[24];
      const auto result = std::to_chars(buffer, buffer + sizeof buffer, value.as_int());
      out.append(buffer, result.ptr);
      break;
    }
    case Json::Kind::Real: number(value.as_real()); break;
    case Json::Kind::String: {
      out += '"';
      for (const char ch : value.as_string()) append_escaped(out, ch);
      out += '"';
      break;
    }
    case Json::Kind::Array: {
      const auto& items = value.as_array();
      if (items.empty()) {
        out += "[]";
        break;
      }
      out += '[';
      bool first = true;
      for (const auto& item : items) {
        if (!first) out += ',';
        first = false;
        newline(depth + 1);
        dump_into(out, item, indent, depth + 1);
      }
      newline(depth);
      out += ']';
      break;
    }
    case Json::Kind::Object: {
      const auto& items = value.as_object();
      if (items.empty()) {
        out += "{}";
        break;
      }
      out += '{';
      bool first = true;
      for (const auto& [key, item] : items) {
        if (!first) out += ',';
        first = false;
        newline(depth + 1);
        out += '"';
        for (const char ch : key) append_escaped(out, ch);
        out += "\":";
        if (indent > 0) out += ' ';
        dump_into(out, item, indent, depth + 1);
      }
      newline(depth);
      out += '}';
      break;
    }
  }
}

}  // namespace detail

inline Json Json::boolean(bool value) {
  Json out;
  out.kind_ = Kind::Bool;
  out.bool_ = value;
  return out;
}

inline Json Json::integer(std::int64_t value) {
  Json out;
  out.kind_ = Kind::Int;
  out.int_ = value;
  return out;
}

inline Json Json::real(double value) {
  Json out;
  out.kind_ = Kind::Real;
  out.real_ = value;
  return out;
}

inline Json Json::string(std::string value) {
  Json out;
  out.kind_ = Kind::String;
  out.string_ = std::move(value);
  return out;
}

inline Json Json::array(Array value) {
  Json out;
  out.kind_ = Kind::Array;
  out.array_ = std::move(value);
  return out;
}

inline Json Json::object(Object value) {
  Json out;
  out.kind_ = Kind::Object;
  out.object_ = std::move(value);
  return out;
}

inline bool Json::as_bool() const {
  if (kind_ != Kind::Bool) throw JsonError("not a boolean");
  return bool_;
}

inline std::int64_t Json::as_int() const {
  if (kind_ != Kind::Int) throw JsonError("not an integer");
  return int_;
}

inline double Json::as_real() const {
  if (kind_ != Kind::Real) throw JsonError("not a real");
  return real_;
}

inline double Json::as_number() const {
  return kind_ == Kind::Int ? static_cast<double>(int_) : as_real();
}

inline const std::string& Json::as_string() const {
  if (kind_ != Kind::String) throw JsonError("not a string");
  return string_;
}

inline const Json::Array& Json::as_array() const {
  if (kind_ != Kind::Array) throw JsonError("not an array");
  return array_;
}

inline const Json::Object& Json::as_object() const {
  if (kind_ != Kind::Object) throw JsonError("not an object");
  return object_;
}

inline const Json* Json::find(std::string_view key) const {
  if (kind_ != Kind::Object) return nullptr;
  for (const auto& [name, value] : object_) {
    if (name == key) return &value;
  }
  return nullptr;
}

inline const Json& Json::at(std::size_t index) const {
  const auto& items = as_array();
  if (index >= items.size()) throw JsonError("array index out of range");
  return items[index];
}

inline std::size_t Json::size() const {
  switch (kind_) {
    case Kind::Array: return array_.size();
    case Kind::Object: return object_.size();
    case Kind::String: return string_.size();
    default: return 0;
  }
}

inline void Json::push(Json value) {
  kind_ = Kind::Array;
  array_.push_back(std::move(value));
}

inline bool Json::equal(const Json& lhs, const Json& rhs, double tolerance) {
  if (lhs.kind_ == rhs.kind_) {
    switch (lhs.kind_) {
      case Kind::Null: return true;
      case Kind::Bool: return lhs.bool_ == rhs.bool_;
      case Kind::Int: return lhs.int_ == rhs.int_;
      case Kind::Real: {
        if (std::isnan(lhs.real_) || std::isnan(rhs.real_)) {
          return std::isnan(lhs.real_) && std::isnan(rhs.real_);
        }
        if (tolerance > 0.0) {
          if (std::isinf(lhs.real_) || std::isinf(rhs.real_)) return lhs.real_ == rhs.real_;
          const double scale = std::max({1.0, std::fabs(lhs.real_), std::fabs(rhs.real_)});
          return std::fabs(lhs.real_ - rhs.real_) <= tolerance * scale;
        }
        return std::bit_cast<std::uint64_t>(lhs.real_) == std::bit_cast<std::uint64_t>(rhs.real_);
      }
      case Kind::String: return lhs.string_ == rhs.string_;
      case Kind::Array: {
        if (lhs.array_.size() != rhs.array_.size()) return false;
        for (std::size_t i = 0; i < lhs.array_.size(); ++i) {
          if (!equal(lhs.array_[i], rhs.array_[i], tolerance)) return false;
        }
        return true;
      }
      case Kind::Object: {
        if (lhs.object_.size() != rhs.object_.size()) return false;
        for (const auto& [key, value] : lhs.object_) {
          const Json* other = rhs.find(key);
          if (other == nullptr) return false;
          if (!equal(value, *other, tolerance)) return false;
        }
        return true;
      }
    }
    return false;
  }
  // Int against Real: equal as numbers when the real is integral, which is how
  // Python compares 1 == 1.0. The recorder never produces this shape from one
  // side only, so it only matters for hand-written cases.
  if (lhs.is_number() && rhs.is_number()) {
    const double left = lhs.as_number();
    const double right = rhs.as_number();
    if (tolerance > 0.0) {
      const double scale = std::max({1.0, std::fabs(left), std::fabs(right)});
      return std::fabs(left - right) <= tolerance * scale;
    }
    return left == right;
  }
  return false;
}

inline std::string dump(const Json& value, bool pretty) {
  std::string out;
  out.reserve(256);
  detail::dump_into(out, value, pretty ? 2 : 0, 0);
  return out;
}

inline std::string repr(const Json& value, std::size_t budget) {
  std::string text = dump(value);
  if (text.size() > budget) {
    text.resize(budget);
    text += "...";
  }
  return text;
}

namespace detail {

inline char Parser::peek() const {
  if (at_end()) fail("unexpected end of input");
  return text_[pos_];
}

inline void Parser::skip_space() {
  while (!at_end()) {
    const char ch = text_[pos_];
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      ++pos_;
    } else {
      break;
    }
  }
}

inline void Parser::fail(const std::string& message) const {
  throw JsonError(message + " at offset " + std::to_string(pos_) + " in " +
                  std::string(text_.substr(0, std::min<std::size_t>(text_.size(), 40))));
}

inline void Parser::expect(char wanted) {
  if (at_end() || text_[pos_] != wanted) {
    fail(std::string("expected '") + wanted + "'");
  }
  ++pos_;
}

inline Json Parser::run() {
  skip_space();
  Json value = parse_value();
  skip_space();
  return value;
}

inline void Parser::expect_eof() {
  skip_space();
  if (!at_end()) fail("trailing input");
}

inline Json Parser::parse_value() {
  if (at_end()) fail("expected a value");
  switch (text_[pos_]) {
    case '{': return parse_object();
    case '[': return parse_array();
    case '"': return Json::string(parse_string());
    case 't':
    case 'f':
    case 'n': return parse_literal();
    default: return parse_number();
  }
}

inline Json Parser::parse_object() {
  expect('{');
  Json::Object items;
  skip_space();
  if (!at_end() && peek() == '}') {
    ++pos_;
    return Json::object(std::move(items));
  }
  while (true) {
    skip_space();
    if (at_end() || peek() != '"') fail("expected a key");
    std::string key = parse_string();
    skip_space();
    expect(':');
    skip_space();
    Json value = parse_value();
    items.emplace_back(std::move(key), std::move(value));
    skip_space();
    if (at_end()) fail("unterminated object");
    if (peek() == ',') {
      ++pos_;
      continue;
    }
    expect('}');
    break;
  }
  return Json::object(std::move(items));
}

inline Json Parser::parse_array() {
  expect('[');
  Json::Array items;
  skip_space();
  if (!at_end() && peek() == ']') {
    ++pos_;
    return Json::array(std::move(items));
  }
  while (true) {
    skip_space();
    items.push_back(parse_value());
    skip_space();
    if (at_end()) fail("unterminated array");
    if (peek() == ',') {
      ++pos_;
      continue;
    }
    expect(']');
    break;
  }
  return Json::array(std::move(items));
}

inline unsigned Parser::parse_hex4() {
  unsigned value = 0;
  for (int i = 0; i < 4; ++i) {
    if (at_end()) fail("truncated \\u escape");
    const char ch = text_[pos_++];
    value <<= 4;
    if (ch >= '0' && ch <= '9') {
      value |= static_cast<unsigned>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value |= static_cast<unsigned>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      value |= static_cast<unsigned>(ch - 'A' + 10);
    } else {
      fail("bad \\u escape");
    }
  }
  return value;
}

inline void Parser::append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

inline std::string Parser::parse_string() {
  expect('"');
  std::string out;
  while (true) {
    if (at_end()) fail("unterminated string");
    const char ch = text_[pos_++];
    if (ch == '"') break;
    if (ch != '\\') {
      out += ch;
      continue;
    }
    if (at_end()) fail("unterminated escape");
    const char esc = text_[pos_++];
    switch (esc) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        std::uint32_t cp = parse_hex4();
        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
            text_[pos_ + 1] == 'u') {
          pos_ += 2;
          const std::uint32_t low = parse_hex4();
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(out, cp);
        break;
      }
      default: fail("unknown escape");
    }
  }
  return out;
}

inline Json Parser::parse_literal() {
  if (text_.substr(pos_, 4) == "true") {
    pos_ += 4;
    return Json::boolean(true);
  }
  if (text_.substr(pos_, 5) == "false") {
    pos_ += 5;
    return Json::boolean(false);
  }
  if (text_.substr(pos_, 4) == "null") {
    pos_ += 4;
    return Json::null();
  }
  if (text_.substr(pos_, 8) == "Infinity") {
    pos_ += 8;
    return Json::real(std::numeric_limits<double>::infinity());
  }
  if (text_.substr(pos_, 9) == "-Infinity") {
    pos_ += 9;
    return Json::real(-std::numeric_limits<double>::infinity());
  }
  if (text_.substr(pos_, 3) == "NaN") {
    pos_ += 3;
    return Json::real(std::numeric_limits<double>::quiet_NaN());
  }
  fail("expected a literal");
}

inline Json Parser::parse_number() {
  const std::size_t start = pos_;
  if (!at_end() && (text_[pos_] == '-' || text_[pos_] == '+')) {
    if (text_[pos_] == '+') fail("leading '+' is not JSON");
    ++pos_;
  }
  bool is_real = false;
  bool digits = false;
  while (!at_end()) {
    const char ch = text_[pos_];
    if (ch >= '0' && ch <= '9') {
      digits = true;
      ++pos_;
    } else if (ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
      is_real = true;
      ++pos_;
    } else {
      break;
    }
  }
  if (!digits) fail("expected a number");
  const std::string_view token = text_.substr(start, pos_ - start);
  if (!is_real) {
    std::int64_t value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) {
      return Json::integer(value);
    }
    // Out of int64 range: keep it a real so nothing is silently truncated.
    is_real = true;
  }
  double value = 0.0;
  const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
  if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
    fail("malformed number");
  }
  return Json::real(value);
}

}  // namespace detail

inline Json parse(std::string_view text) {
  detail::Parser parser(text);
  Json value = parser.run();
  parser.expect_eof();
  return value;
}

}  // namespace cppan
