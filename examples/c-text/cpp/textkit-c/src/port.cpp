// Port of textkit.c to C++23.
//
// The C signature is the contract: a null pointer cannot cross (the driver never
// sends one), and a failure is a return code here exactly as it is there.
#include "textkit/port.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace textkit {
namespace {

[[nodiscard]] bool is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
}

[[nodiscard]] bool is_alnum(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

[[nodiscard]] std::size_t find_from(std::string_view haystack, std::string_view needle,
                                    std::size_t from) {
  if (needle.empty() || from > haystack.size()) return std::string_view::npos;
  return haystack.find(needle, from);
}

}  // namespace

cppan::Result<std::int64_t> word_count(std::string_view text) {
  std::int64_t words = 0;
  bool in_word = false;
  for (const char ch : text) {
    if (is_space(ch)) {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      ++words;
    }
  }
  return words;
}

cppan::Result<std::int64_t> digits_only(std::string_view text) {
  std::int64_t value = 0;
  bool seen = false;
  for (const char ch : text) {
    if (ch == ' ' || ch == '\t') continue;
    if (ch < '0' || ch > '9') return std::int64_t{-1};
    value = value * 10 + (ch - '0');
    seen = true;
  }
  return seen ? value : std::int64_t{-1};
}

cppan::Result<std::int64_t> repeat_count(std::string_view text, std::string_view needle) {
  if (needle.empty()) return std::int64_t{0};
  std::int64_t count = 0;
  std::size_t at = 0;
  while (true) {
    const std::size_t found = find_from(text, needle, at);
    if (found == std::string_view::npos) break;
    ++count;
    at = found + needle.size();
  }
  return count;
}

cppan::Result<double> average_word_length(std::string_view text) {
  std::int64_t words = 0;
  std::int64_t letters = 0;
  bool in_word = false;
  for (const char ch : text) {
    if (is_space(ch)) {
      in_word = false;
    } else {
      if (!in_word) {
        in_word = true;
        ++words;
      }
      ++letters;
    }
  }
  return words == 0 ? 0.0 : static_cast<double>(letters) / static_cast<double>(words);
}

cppan::Result<std::int64_t> alnum_count(std::string_view text) {
  std::int64_t count = 0;
  for (const char ch : text) {
    if (is_alnum(ch)) ++count;
  }
  return count;
}

}  // namespace textkit
