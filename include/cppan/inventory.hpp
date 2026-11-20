// What the probe found in a unit: functions, types, defaults, risks.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cppan/json.hpp"
#include "cppan/types.hpp"
#include "cppan/util.hpp"

namespace cppan {

struct Param {
  std::string name;
  std::string annotation;
  Ty ty;
  std::optional<Json> default_value;
  bool has_default = false;
  bool keyword_only = false;
};

struct Function {
  std::string name;
  std::uint32_t line = 0;
  std::vector<Param> params;
  Ty returns;
  std::string returns_annotation;
  std::string doc;
  std::vector<std::string> risks;
  /// The original raises somewhere in this function: a port must too, and a
  /// recorded failing case must exist.
  bool raises = false;

  [[nodiscard]] static Result<Function> parse(const Json& item);
  [[nodiscard]] Json to_json() const;

  /// `[[nodiscard]] std::expected<std::string, PortError> slug(std::string_view text);`
  [[nodiscard]] std::string cpp_declaration() const;
  /// The definition's opening line: same types, no default arguments.
  [[nodiscard]] std::string cpp_definition() const;
  /// The declaration, without the semicolon.
  [[nodiscard]] std::string cpp_signature() const;
  /// `std::string slug(std::string_view text)`: the reader-facing signature.
  [[nodiscard]] std::string plain_signature() const;
  /// `slug(text: str) -> str`, the Python side of the same function.
  [[nodiscard]] std::string signature_line() const;

 private:
  [[nodiscard]] std::string cpp_signature_with(bool with_defaults) const;
};

struct Unit {
  std::string unit;
  std::string module;
  std::filesystem::path file;
  std::optional<std::filesystem::path> package_root;
  std::uint32_t lines = 0;
  std::vector<Function> functions;
  std::vector<std::pair<std::string, std::string>> skipped;
  std::vector<std::string> risks;
  std::vector<std::string> imports;

  [[nodiscard]] static Result<Unit> parse(const Json& doc);
};

}  // namespace cppan
