// Generated inputs, shared by the language plugins that record out-of-process.
//
// Deterministic, biased towards edges, and inside int64: a value the wire cannot
// carry is a risk the analyzer reports, not a case a signature can never satisfy.
#pragma once

#include <vector>

#include "cppan/inventory.hpp"
#include "cppan/json.hpp"

namespace cppan::corpora {

/// `{"id","fn","args"}` calls, one per generated argument combination.
[[nodiscard]] std::vector<Json> calls(const std::vector<Function>& functions, std::size_t count);

}  // namespace cppan::corpora
