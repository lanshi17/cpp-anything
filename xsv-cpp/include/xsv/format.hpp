#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "xsv/common.hpp"

namespace xsv {

// Number formatting is part of the observable contract: stats writes numbers
// that must agree with Rust's Display implementation, not with the C++
// default float formatting (which switches to scientific notation).
[[nodiscard]] std::string format_u64(std::uint64_t value);
[[nodiscard]] std::string format_i64(std::int64_t value);
[[nodiscard]] std::string format_f64(double value);

// Rust's FromStr behaviour: optional sign, no surrounding whitespace, the
// whole string must be consumed. Fast_float handles the significant digits;
// these wrappers reject what Rust rejects ("+", "1.", ".e1", trailing bytes).
[[nodiscard]] Result<std::int64_t> parse_i64(std::string_view text);
[[nodiscard]] Result<double> parse_f64(std::string_view text);

}  // namespace xsv
