#include "xsv/simd.hpp"

#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace xsv::simd {
namespace {

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2"))) std::size_t find_byte_avx2(
    std::span<const char> haystack, char needle) noexcept {
  const __m256i wanted =
      _mm256_set1_epi8(static_cast<char>(needle));
  std::size_t at = 0;
  const std::size_t size = haystack.size();
  for (; at + 32 <= size; at += 32) {
    const __m256i block = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(haystack.data() + at));
    const unsigned mask = static_cast<unsigned>(
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, wanted)));
    if (mask != 0) {
      return at + static_cast<std::size_t>(__builtin_ctz(mask));
    }
  }
  for (; at < size; ++at) {
    if (haystack[at] == needle) return at;
  }
  return npos;
}

__attribute__((target("avx2"))) std::size_t find_record_special_avx2(
    std::span<const char> haystack, char delimiter, char quote) noexcept {
  const __m256i delim = _mm256_set1_epi8(delimiter);
  const __m256i quoted = _mm256_set1_epi8(quote);
  const __m256i cr = _mm256_set1_epi8('\r');
  const __m256i lf = _mm256_set1_epi8('\n');
  std::size_t at = 0;
  const std::size_t size = haystack.size();
  for (; at + 32 <= size; at += 32) {
    const __m256i block = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(haystack.data() + at));
    const __m256i hit = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(block, delim),
                        _mm256_cmpeq_epi8(block, quoted)),
        _mm256_or_si256(_mm256_cmpeq_epi8(block, cr),
                        _mm256_cmpeq_epi8(block, lf)));
    const unsigned mask =
        static_cast<unsigned>(_mm256_movemask_epi8(hit));
    if (mask != 0) {
      return at + static_cast<std::size_t>(__builtin_ctz(mask));
    }
  }
  for (; at < size; ++at) {
    const char c = haystack[at];
    if (c == delimiter || c == quote || c == '\r' || c == '\n') return at;
  }
  return npos;
}


__attribute__((target("avx2"))) std::size_t find_field_end_avx2(
    std::span<const char> haystack, char delimiter) noexcept {
  const __m256i delim = _mm256_set1_epi8(delimiter);
  const __m256i cr = _mm256_set1_epi8('\r');
  const __m256i lf = _mm256_set1_epi8('\n');
  std::size_t at = 0;
  const std::size_t size = haystack.size();
  for (; at + 32 <= size; at += 32) {
    const __m256i block = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(haystack.data() + at));
    const __m256i hit = _mm256_or_si256(
        _mm256_cmpeq_epi8(block, delim),
        _mm256_or_si256(_mm256_cmpeq_epi8(block, cr),
                        _mm256_cmpeq_epi8(block, lf)));
    const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(hit));
    if (mask != 0) return at + static_cast<std::size_t>(__builtin_ctz(mask));
  }
  for (; at < size; ++at) {
    const char c = haystack[at];
    if (c == delimiter || c == '\r' || c == '\n') return at;
  }
  return npos;
}

__attribute__((target("avx2"))) std::size_t count_byte_avx2(
    std::span<const char> haystack, char needle) noexcept {
  const __m256i wanted = _mm256_set1_epi8(needle);
  std::size_t at = 0;
  std::size_t count = 0;
  const std::size_t size = haystack.size();
  for (; at + 32 <= size; at += 32) {
    const __m256i block = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(haystack.data() + at));
    const unsigned mask = static_cast<unsigned>(
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, wanted)));
    count += static_cast<std::size_t>(__builtin_popcount(mask));
  }
  for (; at < size; ++at) {
    if (haystack[at] == needle) ++count;
  }
  return count;
}
#endif

std::size_t find_byte_scalar(std::span<const char> haystack,
                             char needle) noexcept {
  if (haystack.empty()) return npos;
  const void* hit = std::memchr(haystack.data(), static_cast<unsigned char>(needle),
                                haystack.size());
  if (hit == nullptr) return npos;
  return static_cast<std::size_t>(
      static_cast<const char*>(hit) - haystack.data());
}

std::size_t find_field_end_scalar(std::span<const char> haystack,
                                  char delimiter) noexcept {
  for (std::size_t i = 0; i < haystack.size(); ++i) {
    const char c = haystack[i];
    if (c == delimiter || c == '\r' || c == '\n') return i;
  }
  return npos;
}

std::size_t find_record_special_scalar(std::span<const char> haystack,
                                       char delimiter, char quote) noexcept {
  for (std::size_t i = 0; i < haystack.size(); ++i) {
    const char c = haystack[i];
    if (c == delimiter || c == quote || c == '\r' || c == '\n') return i;
  }
  return npos;
}

std::size_t count_byte_scalar(std::span<const char> haystack,
                              char needle) noexcept {
  std::size_t count = 0;
  for (const char c : haystack) {
    if (c == needle) ++count;
  }
  return count;
}

}  // namespace

bool has_avx2() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  static const bool available = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
  }();
  return available;
#else
  return false;
#endif
}

std::size_t find_byte(std::span<const char> haystack, char needle) noexcept {
#if XSV_USE_SIMD && (defined(__x86_64__) || defined(__i386__))
  if (has_avx2()) return find_byte_avx2(haystack, needle);
#endif
  return find_byte_scalar(haystack, needle);
}

std::size_t find_field_end(std::span<const char> haystack,
                           char delimiter) noexcept {
#if XSV_USE_SIMD && (defined(__x86_64__) || defined(__i386__))
  if (has_avx2()) return find_field_end_avx2(haystack, delimiter);
#endif
  return find_field_end_scalar(haystack, delimiter);
}

std::size_t find_record_special(std::span<const char> haystack, char delimiter,
                                char quote) noexcept {
#if XSV_USE_SIMD && (defined(__x86_64__) || defined(__i386__))
  if (has_avx2()) return find_record_special_avx2(haystack, delimiter, quote);
#endif
  return find_record_special_scalar(haystack, delimiter, quote);
}

std::size_t count_byte(std::span<const char> haystack, char needle) noexcept {
#if XSV_USE_SIMD && (defined(__x86_64__) || defined(__i386__))
  if (has_avx2()) return count_byte_avx2(haystack, needle);
#endif
  return count_byte_scalar(haystack, needle);
}

}  // namespace xsv::simd
