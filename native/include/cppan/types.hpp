// The type model: Python annotations -> C++20/23 signatures.
//
// One dialect, `Ty`, is the contract between the analyzer, the code generator and
// the differ. It maps onto C++ the way a person would write it now: `string_view`
// in, `std::string` out, `std::span` for read-only sequences,
// `std::expected<T, PortError>` for anything that can fail.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/json.hpp"

namespace cppan {

struct Ty {
  enum class Kind : std::uint8_t {
    Str,
    Int,
    Float,
    Bool,
    None,
    List,
    Dict,
    Tuple,
    Optional,
    Any,
  };

  Kind kind = Kind::Any;
  /// List/Dict/Optional: exactly one; Tuple: one per element.
  std::vector<Ty> items;

  /// Parses the string form the analyzer emits: `str`, `list[int]`,
  /// `dict[str, float]`, `tuple[int, str]`, `str | None`, `Optional[str]`.
  [[nodiscard]] static Ty parse(std::string_view text);

  [[nodiscard]] bool operator==(const Ty& other) const = default;

  /// Parameter type in the port's header. `top_level` lets the outer list be a
  /// `std::span` while nested ones stay owning vectors.
  [[nodiscard]] std::string cpp_param(bool top_level) const;
  /// The owning type for a value of this type.
  [[nodiscard]] std::string cpp_value() const;
  /// Type the dispatch decodes into: sequences need storage a span can point at.
  [[nodiscard]] std::string cpp_storage() const;
  /// Return type inside `std::expected<_, PortError>`.
  [[nodiscard]] std::string cpp_return() const;
  /// How the analyzer labels the type in a report.
  [[nodiscard]] std::string label() const;
  /// Headers a generated file needs for this type.
  void headers(std::vector<std::string>& out) const;
  /// A C++ expression for a Python default value.
  [[nodiscard]] std::optional<std::string> cpp_default(const Json& value) const;
};

[[nodiscard]] std::string cpp_string_literal(std::string_view text);
[[nodiscard]] std::vector<std::string_view> split_top_level(std::string_view text,
                                                            char separator);

}  // namespace cppan
