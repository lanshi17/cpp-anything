#pragma once

// Common vocabulary: fallible results that never throw across a module
// boundary, plus the feature switches that the ablation builds flip.
//
// Every XSV_USE_* macro can be overridden by the build system; the defaults
// keep a header-only test or a standalone compile of one translation unit
// valid.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#ifndef XSV_USE_PMR
#define XSV_USE_PMR 0
#endif
#ifndef XSV_USE_SIMD
#define XSV_USE_SIMD 0
#endif
#ifndef XSV_USE_TBB
#define XSV_USE_TBB 0
#endif
#ifndef XSV_USE_CUSTOM_HASH
#define XSV_USE_CUSTOM_HASH 0
#endif
#ifndef XSV_USE_MMAP
#define XSV_USE_MMAP 0
#endif
#ifndef XSV_USE_RE2
#define XSV_USE_RE2 0
#endif

namespace xsv {

struct Error {
  std::string message;
  int exit_code = 1;
};

template <class T>
using Result = std::expected<T, Error>;
using Status = std::expected<void, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(std::string message) {
  return std::unexpected(Error{std::move(message), 1});
}

[[nodiscard]] inline Status success() { return {}; }

inline constexpr std::size_t npos = static_cast<std::size_t>(-1);

}  // namespace xsv
