// The modern port: same results as legacy.cpp, none of its idioms.
//
// string_view in, std::string out, ranges and views for the loops, and formatting
// gated by what the target toolchain reports in include/cppan/profile.hpp.
#include "legacy/port.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <ranges>
#include <string>
#include <string_view>

#include "cppan/profile.hpp"

#if CPPAN_HAS_PRINT
#include <print>
#elif CPPAN_HAS_FORMAT
#include <format>
#endif

namespace legacy {
namespace {

[[nodiscard]] constexpr bool is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
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

cppan::Result<double> average_word_length(std::string_view text) {
  std::int64_t words = 0;
  std::int64_t letters = 0;
  bool in_word = false;
  for (const char ch : text) {
    if (is_space(ch)) {
      in_word = false;
      continue;
    }
    if (!in_word) {
      in_word = true;
      ++words;
    }
    ++letters;
  }
  return words == 0 ? 0.0 : static_cast<double>(letters) / static_cast<double>(words);
}

cppan::Result<std::int64_t> sum_ascii(std::string_view text) {
  const auto bytes = std::views::transform(text, [](char ch) {
    return static_cast<std::int64_t>(static_cast<unsigned char>(ch));
  });
  return std::accumulate(bytes.begin(), bytes.end(), std::int64_t{0});
}

cppan::Result<std::string> trim_copy(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return std::string{text};
}

cppan::Result<std::string> repeat_join(std::string_view text, std::int64_t times) {
  if (times <= 0) return std::string{};
  const auto count = static_cast<std::size_t>(times);
  std::string out;
  out.reserve(count * text.size() + (count - 1));
  for (std::size_t index = 0; index < count; ++index) {
    if (index > 0) out.push_back(' ');
    out.append(text);
  }
  return out;
}

cppan::Result<cppan::Json> print_words(std::string_view text) {
  const auto count = word_count(text);
  const std::int64_t words = count.has_value() ? *count : 0;
#if CPPAN_HAS_PRINT
  std::println("words={}", words);
#elif CPPAN_HAS_FORMAT
  // GCC 13's library has <format> but not <print>: build the line, write it once.
  const std::string line = std::format("words={}\n", words);
  std::fwrite(line.data(), 1, line.size(), stdout);
#else
  std::cout << "words=" << words << '\n';
#endif
  return cppan::Json::null();
}

}  // namespace legacy
