// Whether a recording still describes the source it was made from.
#pragma once

#include <optional>
#include <string>

#include "cppan/project.hpp"

namespace cppan::recording {

/// The saved hash is taken by `record`; when it differs, the cases describe code
/// that no longer exists and `diff` passing would mean nothing.
[[nodiscard]] std::optional<std::string> stale(const Project& project);

}  // namespace cppan::recording
