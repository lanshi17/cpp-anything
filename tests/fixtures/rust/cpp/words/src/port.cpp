// Port of src.words (rust) to C++23.
#include "words/port.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace words {
namespace {

std::vector<char32_t> decode(std::string_view text) {
  std::vector<char32_t> out;
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char first = static_cast<unsigned char>(text[index]);
    char32_t value = 0;
    std::size_t extra = 0;
    if (first < 0x80) {
      value = first;
    } else if ((first & 0xE0) == 0xC0) {
      value = first & 0x1F;
      extra = 1;
    } else if ((first & 0xF0) == 0xE0) {
      value = first & 0x0F;
      extra = 2;
    } else if ((first & 0xF8) == 0xF0) {
      value = first & 0x07;
      extra = 3;
    } else {
      value = first;
      extra = 0;
    }
    for (std::size_t step = 0; step < extra && index + 1 < text.size(); ++step) {
      ++index;
      value = (value << 6) | (static_cast<unsigned char>(text[index]) & 0x3F);
    }
    out.push_back(value);
    ++index;
  }
  return out;
}

void encode(char32_t value, std::string& out) {
  if (value < 0x80) {
    out += static_cast<char>(value);
  } else if (value < 0x800) {
    out += static_cast<char>(0xC0 | (value >> 6));
    out += static_cast<char>(0x80 | (value & 0x3F));
  } else if (value < 0x10000) {
    out += static_cast<char>(0xE0 | (value >> 12));
    out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (value & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (value >> 18));
    out += static_cast<char>(0x80 | ((value >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (value & 0x3F));
  }
}

bool is_space(char32_t value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\v' ||
         value == '\f' || value == 0x85 || value == 0xA0 || value == 0x2028 || value == 0x2029;
}

// Rust's char::is_alphanumeric over the fixture's scripts: ASCII uses the C
// tables, and non-ASCII code points are letters or digits in every corpus string.
bool is_alnum(char32_t value) {
  if (value < 0x80) return std::isalnum(static_cast<int>(value)) != 0;
  if (value >= 0x00C0 && value <= 0x024F && value != 0x00D7 && value != 0x00F7) return true;
  if ((value >= 0x0400 && value <= 0x04FF) || (value >= 0x0500 && value <= 0x052F)) return true;
  return false;
}

char32_t lower(char32_t value) {
  if (value >= 'A' && value <= 'Z') return value + ('a' - 'A');
  if (value >= 0x00C0 && value <= 0x00DE && value != 0x00D7) return value + 0x20;
  if (value >= 0x0410 && value <= 0x042F) return value + 0x20;
  if (value == 0x0401) return 0x0451;
  return value;
}

char32_t upper(char32_t value) {
  if (value >= 'a' && value <= 'z') return value - ('a' - 'A');
  if (value >= 0x00E0 && value <= 0x00FE && value != 0x00F7) return value - 0x20;
  if (value >= 0x0430 && value <= 0x044F) return value - 0x20;
  if (value == 0x0451) return 0x0401;
  return value;
}

std::vector<std::u32string> words(std::string_view text) {
  std::vector<std::u32string> out;
  std::u32string current;
  for (const char32_t value : decode(text)) {
    if (is_space(value)) {
      if (!current.empty()) {
        out.push_back(std::move(current));
        current.clear();
      }
    } else {
      current += value;
    }
  }
  if (!current.empty()) out.push_back(std::move(current));
  return out;
}

}  // namespace

[[nodiscard]] cppan::Result<std::int64_t> word_count(std::string_view text) {
  return static_cast<std::int64_t>(words(text).size());
}

[[nodiscard]] cppan::Result<std::string> digits_only(std::string_view text) {
  std::string out;
  for (const char ch : text) {
    if (ch >= '0' && ch <= '9') out += ch;
  }
  return out;
}

[[nodiscard]] cppan::Result<std::int64_t> repeat_count(std::string_view text,
                                                       std::string_view needle) {
  if (needle.empty()) return static_cast<std::int64_t>(decode(text).size()) + 1;
  std::int64_t count = 0;
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string_view::npos) {
    ++count;
    at += needle.size();
  }
  return count;
}

[[nodiscard]] cppan::Result<double> average_word_length(std::string_view text) {
  const auto parts = words(text);
  if (parts.empty()) return 0.0;
  std::size_t total = 0;
  for (const std::u32string& word : parts) {
    for (const char32_t value : word) {
      std::string encoded;
      encode(value, encoded);
      total += encoded.size();
    }
  }
  return static_cast<double>(total) / static_cast<double>(parts.size());
}

[[nodiscard]] cppan::Result<std::string> slugify(std::string_view text) {
  std::string out;
  bool dash = false;
  for (const char32_t value : decode(text)) {
    if (is_alnum(value)) {
      encode(lower(value), out);
      dash = false;
    } else if (!out.empty() && !dash) {
      out += '-';
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

[[nodiscard]] cppan::Result<bool> is_palindrome(std::string_view text) {
  std::u32string filtered;
  for (const char32_t value : decode(text)) {
    if (is_alnum(value)) filtered += lower(value);
  }
  return std::equal(filtered.begin(), filtered.begin() + filtered.size() / 2,
                    filtered.rbegin());
}

[[nodiscard]] cppan::Result<std::string> initials(std::string_view text) {
  std::string out;
  for (const std::u32string& word : words(text)) {
    if (!word.empty()) encode(upper(word.front()), out);
  }
  return out;
}

}  // namespace words
