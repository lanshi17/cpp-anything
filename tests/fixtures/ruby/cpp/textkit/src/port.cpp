// Port of textkit (ruby) to C++23.
#include "textkit/port.hpp"

#include "cppan/profile.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace textkit {

namespace {

std::string text_of(const char* name, const cppan::Json& value) {
  if (!value.is_string()) {
    throw PortError{"NoMethodError", std::string("undefined method `split' for ") + name};
  }
  return value.as_string();
}

std::vector<std::string> words_of(const std::string& text) {
  std::vector<std::string> words;
  std::string current;
  for (char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) words.push_back(current);
  return words;
}

std::int64_t code_points(const std::string& text) {
  std::int64_t count = 0;
  for (char ch : text) {
    if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++count;
  }
  return count;
}

}  // namespace

[[nodiscard]] cppan::Result<cppan::Json> word_count(const cppan::Json& text) {
  try {
    const std::string value = text_of("nil", text);
    return cppan::Json::integer(static_cast<std::int64_t>(words_of(value).size()));
  } catch (const cppan::PortError& error) {
    return cppan::unexpected(error);
  }
}

[[nodiscard]] cppan::Result<cppan::Json> digits_only(const cppan::Json& text) {
  if (text.is_string()) {
    std::int64_t count = 0;
    for (char ch : text.as_string()) {
      if (ch >= '0' && ch <= '9') ++count;
    }
    return cppan::Json::integer(count);
  }
  // Ruby's #count exists on Array and Hash too; the string argument never equals
  // an element or a pair, so it answers 0 rather than raising.
  if (text.is_array() || text.is_object()) return cppan::Json::integer(0);
  return cppan::unexpected(PortError{"NoMethodError", "undefined method `count' for the argument"});
}

[[nodiscard]] cppan::Result<cppan::Json> repeat_count(const cppan::Json& text, const cppan::Json& needle) {
  try {
    const std::string value = text_of("nil", text);
    if (!needle.is_string()) {
      throw PortError{"NoMethodError", "undefined method `scan' for the needle"};
    }
    const std::string part = needle.as_string();
    std::int64_t count = 0;
    if (part.empty()) {
      return cppan::Json::integer(code_points(value) + 1);
    }
    std::size_t at = value.find(part);
    while (at != std::string::npos) {
      ++count;
      at = value.find(part, at + part.size());
    }
    return cppan::Json::integer(count);
  } catch (const cppan::PortError& error) {
    return cppan::unexpected(error);
  }
}

[[nodiscard]] cppan::Result<cppan::Json> average_word_length(const cppan::Json& text) {
  try {
    const std::string value = text_of("nil", text);
    const std::vector<std::string> words = words_of(value);
    if (words.empty()) return cppan::Json::real(0.0);
    std::int64_t total = 0;
    for (const std::string& word : words) total += code_points(word);
    return cppan::Json::real(static_cast<double>(total) / static_cast<double>(words.size()));
  } catch (const cppan::PortError& error) {
    return cppan::unexpected(error);
  }
}

}  // namespace textkit
