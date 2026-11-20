// Port of textpkg (Go) to C++23.
//
// The recorded corpus only reaches ASCII whitespace and UTF-8 bytes, so the
// helpers below mirror Go's byte-oriented strings.Count and its simple
// ParseInt grammar; the Go risks section documents the wider Unicode rules.
#include "textpkg/port.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace textpkg {
namespace {

[[nodiscard]] bool is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
         ch == '\f';
}

[[nodiscard]] std::string_view trim(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

// Go counts code points, not bytes, when the needle is empty.
[[nodiscard]] std::int64_t code_points(std::string_view text) {
  std::int64_t count = 0;
  for (const char ch : text) {
    if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) ++count;
  }
  return count;
}

// strings.Count is non-overlapping and byte-oriented.
[[nodiscard]] std::int64_t count_bytes(std::string_view text, std::string_view needle) {
  if (needle.empty()) return code_points(text) + 1;
  std::int64_t count = 0;
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string_view::npos) {
    ++count;
    at += needle.size();
  }
  return count;
}

[[nodiscard]] bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

// strconv.ParseInt in base 10: an optional sign, then ASCII digits, int64 only.
[[nodiscard]] cppan::Result<std::int64_t> parse_int(std::string_view body) {
  if (body.empty()) {
    return cppan::unexpected(PortError{"panic", "not a base-10 number: empty input"});
  }
  std::size_t at = 0;
  bool negative = false;
  if (body[at] == '+' || body[at] == '-') {
    negative = body[at] == '-';
    ++at;
  }
  if (at == body.size()) {
    return cppan::unexpected(PortError{"panic", "not a base-10 number: no digits"});
  }
  const std::uint64_t limit = negative
                                  ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U
                                  : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  std::uint64_t value = 0;
  for (; at < body.size(); ++at) {
    const char ch = body[at];
    if (!is_digit(ch)) {
      return cppan::unexpected(PortError{"panic", "not a base-10 number: invalid digit"});
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (value > (limit - digit) / 10U) {
      return cppan::unexpected(PortError{"panic", "not a base-10 number: out of range"});
    }
    value = value * 10U + digit;
  }
  if (negative) {
    if (value == limit) return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(value);
  }
  return static_cast<std::int64_t>(value);
}

}  // namespace

cppan::Result<std::int64_t> WordCount(std::string_view text) {
  std::int64_t count = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && is_space(text[at])) ++at;
    if (at == text.size()) break;
    ++count;
    while (at < text.size() && !is_space(text[at])) ++at;
  }
  return count;
}

cppan::Result<std::string> Slash(std::string_view text) {
  const std::string_view body = trim(text);
  std::string out;
  out.reserve(body.size());
  for (const char ch : body) out.push_back(ch == ' ' ? '-' : ch);
  return out;
}

cppan::Result<std::int64_t> CountBetween(std::string_view text, std::string_view needle) {
  return count_bytes(text, needle);
}

cppan::Result<bool> Greater(std::int64_t a, std::int64_t b) { return a > b; }

cppan::Result<std::int64_t> MustParse(std::string_view text) {
  return parse_int(trim(text));
}

}  // namespace textpkg
