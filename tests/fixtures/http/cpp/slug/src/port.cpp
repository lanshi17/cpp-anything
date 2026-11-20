#include "slug/port.hpp"

#include "cppan/profile.hpp"

#include <cctype>
#include <string>

namespace slug {

[[nodiscard]] cppan::Result<std::string> slug(std::string_view text) {
  // Python iterates code points and lowercases first; only ASCII survives the
  // alnum test, so byte-wise ASCII handling gives the same result.
  std::string lowered;
  lowered.reserve(text.size());
  for (char ch : text) {
    lowered.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch))));
  }
  std::size_t begin = 0;
  std::size_t end = lowered.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(lowered[begin])) != 0) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(lowered[end - 1])) != 0) --end;
  std::string out;
  bool dash = false;
  for (std::size_t index = begin; index < end; ++index) {
    const char ch = lowered[index];
    const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    if (alnum) {
      out.push_back(ch);
      dash = false;
    } else if (!dash && !out.empty()) {
      out.push_back('-');
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

}  // namespace slug
