// Port of src.demo.TextTool (java) to C++23.
//
// Every function mirrors the original's observable behaviour: wordCount counts
// non-whitespace runs (Java's \s is ASCII), slug lowercases ASCII and folds every
// other byte into a single dash, and the comparisons keep Java's Integer.compare
// (-1, 0, 1). compareLengths measures UTF-16 code units, as String.length() does,
// not the UTF-8 byte count.
#include "texttool/port.hpp"

#include "cppan/profile.hpp"

namespace texttool {

namespace {

bool ascii_space(unsigned char byte) {
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == 0x0B || byte == '\f' ||
         byte == '\r';
}

// Java's String.length(): the number of UTF-16 code units. A 4-byte UTF-8
// sequence is one supplementary code point, hence a surrogate pair, hence two.
std::int64_t utf16_length(std::string_view text) {
  std::int64_t length = 0;
  std::size_t index = 0;
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80) {
      index += 1;
      length += 1;
    } else if ((byte & 0xE0) == 0xC0 && index + 1 < text.size()) {
      index += 2;
      length += 1;
    } else if ((byte & 0xF0) == 0xE0 && index + 2 < text.size()) {
      index += 3;
      length += 1;
    } else if ((byte & 0xF8) == 0xF0 && index + 3 < text.size()) {
      index += 4;
      length += 2;
    } else {
      index += 1;
      length += 1;
    }
  }
  return length;
}

}  // namespace

[[nodiscard]] cppan::Result<std::int64_t> wordCount(std::string_view text) {
  if (text.empty()) {
    return cppan::unexpected(PortError{"IllegalArgumentException", "text is empty"});
  }
  std::int64_t count = 0;
  bool in_word = false;
  for (const char byte : text) {
    if (ascii_space(static_cast<unsigned char>(byte))) {
      in_word = false;
    } else if (!in_word) {
      count += 1;
      in_word = true;
    }
  }
  return count;
}

[[nodiscard]] cppan::Result<std::string> slug(std::string_view text) {
  std::string out;
  bool pending_dash = false;
  for (const char byte : text) {
    const auto c = static_cast<unsigned char>(byte);
    const unsigned char lower =
        (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
    if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
      if (pending_dash && !out.empty()) {
        out.push_back('-');
      }
      pending_dash = false;
      out.push_back(static_cast<char>(lower));
    } else {
      pending_dash = true;
    }
  }
  return out;
}

[[nodiscard]] cppan::Result<std::int64_t> compareLengths(std::string_view left,
                                                         std::string_view right) {
  const std::int64_t a = utf16_length(left);
  const std::int64_t b = utf16_length(right);
  return a < b ? -1 : (a > b ? 1 : 0);
}

[[nodiscard]] cppan::Result<std::int64_t> compareInts(std::int64_t left, std::int64_t right) {
  return left < right ? -1 : (left > right ? 1 : 0);
}

}  // namespace texttool
