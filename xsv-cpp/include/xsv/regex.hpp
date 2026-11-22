#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "xsv/common.hpp"

#if XSV_USE_RE2
namespace re2 {
class RE2;
}
#endif

namespace xsv {

// Byte-oriented regex used by `search`. RE2 is the closest C++ analogue of the
// Rust regex crate the reference uses: linear-time, no backreferences, UTF-8
// aware. Pattern-syntax differences are recorded in docs/COMPATIBILITY.md.
class Regex {
 public:
  Regex() = default;
  ~Regex();
  Regex(const Regex&) = delete;
  Regex& operator=(const Regex&) = delete;
  Regex(Regex&& other) noexcept;
  Regex& operator=(Regex&& other) noexcept;

  static Result<Regex> compile(std::string_view pattern, bool ignore_case);

  [[nodiscard]] bool is_match(std::string_view text) const;

 private:
#if XSV_USE_RE2
  std::unique_ptr<re2::RE2> impl_;
#endif
};

}  // namespace xsv
