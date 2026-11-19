// Port of textkit.words (Python) to C++23.
//
// This is the reference port cpp-anything ships as an example, and the one
// `cpp-anything rsi check` rebuilds to verify the generator. It is deliberately
// written the way the tool asks for: string_view in, std::expected out, UTF-8
// offsets handled where Python's code-point offsets reach the output.
#include "textkit_words/port.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace textkit_words {
namespace {

[[nodiscard]] bool is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

[[nodiscard]] bool is_ascii_alnum(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

[[nodiscard]] char to_lower(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] std::string_view trim(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

// Python indexes str by code point; C++ indexes by byte. Offsets that reach the
// output go through these two.
[[nodiscard]] std::size_t code_points(std::string_view text) {
  return static_cast<std::size_t>(std::ranges::count_if(text, [](char ch) {
    return (static_cast<unsigned char>(ch) & 0xC0U) != 0x80U;
  }));
}

[[nodiscard]] std::size_t byte_offset(std::string_view text, std::size_t points) {
  std::size_t seen = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if ((static_cast<unsigned char>(text[index]) & 0xC0U) != 0x80U) {
      if (seen == points) return index;
      ++seen;
    }
  }
  return text.size();
}

[[nodiscard]] std::vector<std::string_view> split_words(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && is_space(text[index])) ++index;
    const std::size_t start = index;
    while (index < text.size() && !is_space(text[index])) ++index;
    if (index > start) words.emplace_back(text.substr(start, index - start));
  }
  return words;
}

}  // namespace

cppan::Result<std::string> slug(std::string_view text) {
  std::string out;
  bool dash = false;
  for (const char ch : trim(text)) {
    const char lower = to_lower(ch);
    // Python's isalnum() knows unicode; this function only keeps ASCII, so any
    // other byte (part of a multi-byte character included) becomes a separator.
    if (is_ascii_alnum(ch)) {
      out.push_back(lower);
      dash = false;
    } else if (!dash && !out.empty()) {
      out.push_back('-');
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

cppan::Result<std::string> truncate(std::string_view text, std::int64_t limit) {
  if (limit < 0) {
    return cppan::unexpected(
        PortError{"ValueError", "limit must be >= 0, got " + std::to_string(limit)});
  }
  const auto points = static_cast<std::size_t>(limit);
  if (code_points(text) <= points) return std::string(text);
  return std::string(text.substr(0, byte_offset(text, points))) + "\u2026";
}

cppan::Result<std::int64_t> word_count(std::string_view text) {
  return static_cast<std::int64_t>(split_words(text).size());
}

cppan::Result<std::int64_t> digits_only(std::string_view text) {
  const std::string_view body = trim(text);
  std::int64_t value = 0;
  const char* first = body.data();
  const char* last = body.data() + body.size();
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last) {
    // Python's int() also accepts unicode digits and underscores; the analyzer
    // flags that, and this port keeps the ASCII grammar the corpus covers.
    return cppan::unexpected(PortError{
        "ValueError", "invalid literal for int() with base 10: '" + std::string(body) + "'"});
  }
  return value;
}

cppan::Result<std::vector<std::string>> summarize(std::string_view text,
                                                            std::int64_t max_words) {
  const auto words = split_words(text);
  if (words.empty()) return cppan::unexpected(PortError{"ValueError", "nothing to summarize"});
  // Python's words[:n] with a negative n drops the last -n words rather than
  // returning nothing: the recorded cases include max_words = -100.
  const auto available = static_cast<std::int64_t>(words.size());
  std::int64_t keep = 0;
  if (max_words >= 0) {
    keep = std::min(max_words, available);
  } else if (max_words > -available) {
    keep = available + max_words;
  }
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(keep));
  for (const auto& word : words | std::views::take(static_cast<std::size_t>(keep))) {
    out.emplace_back(word);
  }
  return out;
}

cppan::Result<std::int64_t> require_positive(std::int64_t n) {
  if (n <= 0) return cppan::unexpected(PortError{"ValueError", "expected a positive number"});
  return n * 2;
}

}  // namespace textkit_words
