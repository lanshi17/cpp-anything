#include <string>

#include "harness.hpp"
#include "xsv/regex.hpp"

TEST(regex_literals_classes_and_anchors) {
#if XSV_USE_RE2
  auto literal = xsv::Regex::compile("abc", false);
  CHECK(literal.has_value());
  CHECK(literal->is_match("xxabcxx"));
  CHECK(!literal->is_match("ab"));

  auto digits = xsv::Regex::compile("^[0-9]+$", false);
  CHECK(digits.has_value());
  CHECK(digits->is_match("12345"));
  CHECK(!digits->is_match("12a45"));

  auto unicode = xsv::Regex::compile("\\p{Greek}+", false);
  CHECK(unicode.has_value());
  CHECK(unicode->is_match("\xCE\xB1\xCE\xB2"));
#endif
}

TEST(regex_ignore_case) {
#if XSV_USE_RE2
  auto caseless = xsv::Regex::compile("hello", true);
  CHECK(caseless.has_value());
  CHECK(caseless->is_match("HeLLo"));
  auto sensitive = xsv::Regex::compile("hello", false);
  CHECK(sensitive.has_value());
  CHECK(!sensitive->is_match("HeLLo"));
#endif
}

TEST(regex_invalid_pattern_is_an_error) {
#if XSV_USE_RE2
  CHECK(!xsv::Regex::compile("(", false).has_value());
#endif
}
