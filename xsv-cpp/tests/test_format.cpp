#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "harness.hpp"
#include "xsv/format.hpp"

TEST(format_floats_match_rust_display) {
  CHECK_EQ(xsv::format_f64(0.0), std::string("0"));
  CHECK_EQ(xsv::format_f64(-0.0), std::string("-0"));
  CHECK_EQ(xsv::format_f64(1.0), std::string("1"));
  CHECK_EQ(xsv::format_f64(1.5), std::string("1.5"));
  CHECK_EQ(xsv::format_f64(0.1), std::string("0.1"));
  CHECK_EQ(xsv::format_f64(1e20), std::string("100000000000000000000"));
  CHECK_EQ(xsv::format_f64(1e-7), std::string("0.0000001"));
  CHECK_EQ(xsv::format_f64(123456789012345678901.0),
           std::string("123456789012345680000"));
  CHECK_EQ(xsv::format_f64(std::numeric_limits<double>::infinity()),
           std::string("inf"));
  CHECK_EQ(xsv::format_f64(-std::numeric_limits<double>::infinity()),
           std::string("-inf"));
  CHECK_EQ(xsv::format_f64(std::numeric_limits<double>::quiet_NaN()),
           std::string("NaN"));
}

TEST(format_integers) {
  CHECK_EQ(xsv::format_u64(0), std::string("0"));
  CHECK_EQ(xsv::format_u64(18446744073709551615ULL),
           std::string("18446744073709551615"));
  CHECK_EQ(xsv::format_i64(std::numeric_limits<std::int64_t>::min()),
           std::string("-9223372036854775808"));
}

TEST(parse_floats_accepts_rust_grammar) {
  const auto plus = xsv::parse_f64("+1.5");
  CHECK(plus.has_value());
  CHECK_EQ(*plus, 1.5);
  const auto trailing_dot = xsv::parse_f64("1.");
  CHECK(trailing_dot.has_value());
  CHECK_EQ(*trailing_dot, 1.0);
  const auto leading_dot = xsv::parse_f64(".5");
  CHECK(leading_dot.has_value());
  CHECK_EQ(*leading_dot, 0.5);
  const auto infinity = xsv::parse_f64("Infinity");
  CHECK(infinity.has_value());
  CHECK(std::isinf(*infinity));
  const auto nan = xsv::parse_f64("nan");
  CHECK(nan.has_value());
  CHECK(std::isnan(*nan));
  CHECK(!xsv::parse_f64("1e").has_value());
  CHECK(!xsv::parse_f64(" 1").has_value());
  CHECK(!xsv::parse_f64("1_0").has_value());
  CHECK(!xsv::parse_f64("").has_value());
}

TEST(parse_integers_accepts_rust_grammar) {
  CHECK_EQ(xsv::parse_i64("42").value_or(0), 42);
  CHECK_EQ(xsv::parse_i64("-42").value_or(0), -42);
  CHECK_EQ(xsv::parse_i64("+7").value_or(0), 7);
  CHECK_EQ(xsv::parse_i64("-9223372036854775808").value_or(0),
           std::numeric_limits<std::int64_t>::min());
  CHECK(!xsv::parse_i64("9223372036854775808").has_value());
  CHECK(!xsv::parse_i64("1.0").has_value());
  CHECK(!xsv::parse_i64("").has_value());
}
