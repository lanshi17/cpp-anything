#include <algorithm>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include "harness.hpp"
#include "xsv/simd.hpp"

namespace {

std::size_t scalar_find(std::span<const char> bytes, char needle) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] == needle) return i;
  }
  return xsv::npos;
}

std::size_t scalar_special(std::span<const char> bytes, char delimiter,
                           char quote) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const char c = bytes[i];
    if (c == delimiter || c == quote || c == '\r' || c == '\n') return i;
  }
  return xsv::npos;
}

}  // namespace

TEST(simd_find_matches_scalar_for_all_lengths) {
  std::mt19937 rng(20261120);
  for (std::size_t length = 0; length < 200; ++length) {
    std::string bytes(length, 'x');
    for (char& c : bytes) c = static_cast<char>(rng() % 250 + 1);
    const std::size_t expected = scalar_find(bytes, 'q');
    CHECK_EQ(xsv::simd::find_byte(bytes, 'q'), expected);
    CHECK_EQ(xsv::simd::count_byte(bytes, 'q'),
             static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), 'q')));
  }
}

TEST(simd_field_end_matches_scalar) {
  std::mt19937 rng(99);
  for (std::size_t length = 0; length < 300; ++length) {
    std::string bytes(length, 'x');
    for (char& c : bytes) c = static_cast<char>(rng() % 250 + 1);
    std::size_t expected = xsv::npos;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      const char c = bytes[i];
      if (c == ',' || c == '\r' || c == '\n') {
        expected = i;
        break;
      }
    }
    CHECK_EQ(xsv::simd::find_field_end(bytes, ','), expected);
  }
}

TEST(simd_record_special_matches_scalar) {
  std::mt19937 rng(7);
  for (std::size_t length = 0; length < 300; ++length) {
    std::string bytes(length, 'x');
    for (char& c : bytes) c = static_cast<char>(rng() % 250 + 1);
    const std::size_t expected = scalar_special(bytes, ',', '"');
    CHECK_EQ(xsv::simd::find_record_special(bytes, ',', '"'), expected);
  }
}

TEST(simd_forced_hits) {
  std::string bytes(100, 'a');
  bytes[63] = ',';
  CHECK_EQ(xsv::simd::find_record_special(bytes, ',', '"'), 63u);
  bytes[63] = 'a';
  bytes[99] = '\n';
  CHECK_EQ(xsv::simd::find_record_special(bytes, ',', '"'), 99u);
  CHECK_EQ(xsv::simd::find_byte(bytes, '\n'), 99u);
}

TEST(simd_avx2_available_or_not_is_consistent) {
  const bool first = xsv::simd::has_avx2();
  const bool second = xsv::simd::has_avx2();
  CHECK_EQ(first, second);
}
