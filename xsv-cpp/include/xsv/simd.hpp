#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "xsv/common.hpp"

namespace xsv::simd {

[[nodiscard]] bool has_avx2() noexcept;

// Returns the index of the first occurrence of needle, or npos.
[[nodiscard]] std::size_t find_byte(std::span<const char> haystack,
                                    char needle) noexcept;

// Returns the index of the first delimiter, quote, CR or LF.
[[nodiscard]] std::size_t find_record_special(std::span<const char> haystack,
                                              char delimiter,
                                              char quote) noexcept;

// Returns the index of the first delimiter, CR or LF. This is the unquoted
// field scanner; quotes are ordinary bytes there.
[[nodiscard]] std::size_t find_field_end(std::span<const char> haystack,
                                         char delimiter) noexcept;

[[nodiscard]] std::size_t count_byte(std::span<const char> haystack,
                                     char needle) noexcept;

}  // namespace xsv::simd
