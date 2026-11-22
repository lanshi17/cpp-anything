#include "xsv/format.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

#include <fast_float/fast_float.h>

namespace xsv {
namespace {

bool iequals(std::string_view text, std::string_view wanted) {
  if (text.size() != wanted.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char a = text[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (a != wanted[i]) return false;
  }
  return true;
}

std::string expand_exponent(std::string text) {
  // Input is std::to_chars' shortest round-trip form: [-]D[.D](e[+-]D+)?
  std::string sign;
  std::size_t at = 0;
  if (at < text.size() && (text[at] == '-' || text[at] == '+')) {
    if (text[at] == '-') sign.push_back('-');
    ++at;
  }

  std::string digits;
  std::size_t integer_digits = 0;
  while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
    digits.push_back(text[at++]);
    ++integer_digits;
  }
  if (at < text.size() && text[at] == '.') {
    ++at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      digits.push_back(text[at++]);
    }
  }

  int exponent = 0;
  if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
    ++at;
    int exponent_sign = 1;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
      if (text[at] == '-') exponent_sign = -1;
      ++at;
    }
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      exponent = exponent * 10 + (text[at] - '0');
      ++at;
    }
    exponent *= exponent_sign;
  }

  // Trim trailing fractional zeros that to_chars never emits but a future
  // implementation might.
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

  const long decimal =
      static_cast<long>(integer_digits) + static_cast<long>(exponent);
  std::string out = sign;
  if (digits == "0") {
    out.push_back('0');
    return out;
  }
  if (decimal <= 0) {
    out += "0.";
    out.append(static_cast<std::size_t>(-decimal), '0');
    out += digits;
  } else if (decimal >= static_cast<long>(digits.size())) {
    out += digits;
    out.append(static_cast<std::size_t>(decimal - static_cast<long>(digits.size())),
               '0');
  } else {
    out.append(digits, 0, static_cast<std::size_t>(decimal));
    out.push_back('.');
    out.append(digits, static_cast<std::size_t>(decimal), std::string::npos);
  }
  return out;
}

}  // namespace

std::string format_u64(std::uint64_t value) {
  char buffer[32];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) return "0";
  return std::string(buffer, end);
}

std::string format_i64(std::int64_t value) {
  char buffer[32];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) return "0";
  return std::string(buffer, end);
}

std::string format_f64(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
  char buffer[64];
  const auto [end, error] = std::to_chars(
      buffer, buffer + sizeof(buffer), value, std::chars_format::general);
  if (error != std::errc{}) return "0";
  return expand_exponent(std::string(buffer, end));
}

Result<std::int64_t> parse_i64(std::string_view text) {
  if (text.empty()) return fail("cannot parse integer from empty string");
  bool negative = false;
  if (text.front() == '+' || text.front() == '-') {
    negative = text.front() == '-';
    text.remove_prefix(1);
    if (text.empty()) return fail("invalid digit found in string");
  }
  std::uint64_t magnitude = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), magnitude);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return fail("invalid digit found in string");
  }
  if (negative) {
    if (magnitude > static_cast<std::uint64_t>(INT64_MAX) + 1) {
      return fail("number too small to fit in target type");
    }
    if (magnitude == (std::uint64_t{1} << 63)) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
  }
  if (magnitude > static_cast<std::uint64_t>(INT64_MAX)) {
    return fail("number too large to fit in target type");
  }
  return static_cast<std::int64_t>(magnitude);
}

Result<double> parse_f64(std::string_view text) {
  if (text.empty()) return fail("cannot parse float from empty string");
  double sign = 1.0;
  if (text.front() == '+' || text.front() == '-') {
    if (text.front() == '-') sign = -1.0;
    text.remove_prefix(1);
  }
  if (text.empty()) return fail("invalid float literal");

  if (iequals(text, "inf") || iequals(text, "infinity")) {
    return sign * std::numeric_limits<double>::infinity();
  }
  if (iequals(text, "nan")) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  double value = 0.0;
  const auto result =
      fast_float::from_chars(text.data(), text.data() + text.size(), value,
                             fast_float::chars_format::general);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return fail("invalid float literal");
  }
  return sign * value;
}

}  // namespace xsv
