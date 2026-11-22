#include "xsv/regex.hpp"

#if XSV_USE_RE2
#include <re2/re2.h>

#include <memory>
#include <utility>

namespace xsv {

Regex::~Regex() = default;

Regex::Regex(Regex&& other) noexcept = default;

Regex& Regex::operator=(Regex&& other) noexcept = default;

Result<Regex> Regex::compile(std::string_view pattern, bool ignore_case) {
  re2::RE2::Options options;
  options.set_log_errors(false);
  options.set_case_sensitive(!ignore_case);
  options.set_encoding(re2::RE2::Options::EncodingUTF8);
  auto impl = std::make_unique<re2::RE2>(re2::StringPiece(pattern.data(),
                                                         pattern.size()),
                                         options);
  if (!impl->ok()) return fail(impl->error());
  Regex regex;
  regex.impl_ = std::move(impl);
  return regex;
}

bool Regex::is_match(std::string_view text) const {
  if (!impl_) return false;
  return re2::RE2::PartialMatch(
      std::string_view(text.data(), text.size()), *impl_);
}

}  // namespace xsv

#else

namespace xsv {

Regex::~Regex() = default;
Regex::Regex(Regex&& other) noexcept = default;
Regex& Regex::operator=(Regex&& other) noexcept = default;

Result<Regex> Regex::compile(std::string_view, bool) {
  return fail("regex support was disabled at build time (XSV_WITH_RE2=OFF)");
}

bool Regex::is_match(std::string_view) const { return false; }

}  // namespace xsv

#endif
