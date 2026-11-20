// Port of src.textkit (ts) to C++23. JavaScript semantics: number is a double,
// string lengths are UTF-16 code units, and `\s` is the ECMAScript whitespace set.
#include "textkit/port.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

#include "cppan/profile.hpp"

namespace textkit {
namespace {

std::u32string decode(std::string_view text) {
  std::u32string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size();) {
    const auto byte = static_cast<unsigned char>(text[index]);
    char32_t code = byte;
    std::size_t width = 1;
    if (byte < 0x80) {
      code = byte;
    } else if ((byte & 0xE0) == 0xC0 && index + 1 < text.size()) {
      code = static_cast<char32_t>(byte & 0x1F) << 6 | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
      width = 2;
    } else if ((byte & 0xF0) == 0xE0 && index + 2 < text.size()) {
      code = static_cast<char32_t>(byte & 0x0F) << 12 |
             (static_cast<char32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[index + 2]) & 0x3F);
      width = 3;
    } else if ((byte & 0xF8) == 0xF0 && index + 3 < text.size()) {
      code = static_cast<char32_t>(byte & 0x07) << 18 |
             (static_cast<char32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
             (static_cast<char32_t>(static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[index + 3]) & 0x3F);
      width = 4;
    }
    out.push_back(code);
    index += width;
  }
  return out;
}

std::u16string utf16(std::string_view text) {
  std::u16string out;
  for (const char32_t code : decode(text)) {
    if (code <= 0xFFFF) {
      out.push_back(static_cast<char16_t>(code));
    } else {
      const char32_t rest = code - 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (rest >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (rest & 0x3FF)));
    }
  }
  return out;
}

bool js_space(char32_t code) {
  if (code >= 0x2000 && code <= 0x200A) return true;
  switch (code) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
    case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F:
    case 0x205F: case 0x3000: case 0xFEFF:
      return true;
    default:
      return false;
  }
}

void append(std::string& out, char32_t code) {
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

}  // namespace

[[nodiscard]] cppan::Result<std::string> squish(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (const char32_t code : decode(text)) {
    if (js_space(code)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out.push_back(' ');
    pending_space = false;
    append(out, code);
  }
  return out;
}

[[nodiscard]] cppan::Result<double> parseCount(std::string_view text) {
  std::size_t index = 0;
  if (index < text.size() && text[index] == '-') index += 1;
  const std::size_t digits = index;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') index += 1;
  if (index != text.size() || index == digits) {
    return cppan::unexpected(PortError{"TypeError", "parseCount expects a decimal count"});
  }
  return std::strtod(std::string(text).c_str(), nullptr);
}

[[nodiscard]] cppan::Result<double> commonPrefix(std::string_view left, std::string_view right) {
  const std::u16string lhs = utf16(left);
  const std::u16string rhs = utf16(right);
  const std::size_t limit = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
  std::size_t index = 0;
  while (index < limit && lhs[index] == rhs[index]) index += 1;
  return static_cast<double>(index);
}

[[nodiscard]] cppan::Result<double> compare(double left, double right) {
  return left < right ? -1.0 : left > right ? 1.0 : 0.0;
}

}  // namespace textkit
