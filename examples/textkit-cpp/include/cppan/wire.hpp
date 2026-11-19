// cppan::wire — the contract between the recorded cases and the C++ port.
//
// One case is one JSON object per line:
//   {"id":"slug:3","fn":"slug","args":["Hi"],"expect":{"ok":"hi"}}
// The driver reads cases, calls the port, and writes one result per line:
//   {"id":"slug:3","result":{"ok":"hi"}}
// A port signals a failure by returning std::unexpected(PortError{type, message}),
// where `type` is the original's exception class name ("ValueError", "TypeError").
// The diff compares types, because messages are rarely a contract.
#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "cppan/json.hpp"
#include "cppan/profile.hpp"

#if CPPAN_HAS_EXPECTED
#include <expected>
#endif

namespace cppan {

struct PortError {
  std::string type;
  std::string message;

  [[nodiscard]] std::string what() const { return type + ": " + message; }
};

// --------------------------------------------------------------------------
// Result<T> — std::expected on C++23 targets, the same shape on C++20 ones.
//
// The wire format needs a value-or-error type. `std::expected` is C++23; a port
// targeting C++20 gets this small equivalent instead, and the generated code is
// identical either way because it says `cppan::Result<T>`.
// --------------------------------------------------------------------------

#if CPPAN_HAS_EXPECTED

template <typename T>
using Result = std::expected<T, PortError>;

template <typename T>
[[nodiscard]] auto unexpected(T&& error) {
  return std::unexpected<std::decay_t<T>>(std::forward<T>(error));
}

#else

struct Unexpected {
  PortError error;
};

template <typename T>
[[nodiscard]] Unexpected unexpected(T&& error) {
  return Unexpected{PortError{std::forward<T>(error)}};
}

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)), has_value_(true) {}
  Result(Unexpected failure) : error_(std::move(failure.error)), has_value_(false) {}
  Result(PortError error) : error_(std::move(error)), has_value_(false) {}

  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value_; }

  [[nodiscard]] T& value() & { return value_; }
  [[nodiscard]] const T& value() const& { return value_; }
  [[nodiscard]] T&& value() && { return std::move(value_); }
  [[nodiscard]] T* operator->() { return &value_; }
  [[nodiscard]] const T* operator->() const { return &value_; }
  [[nodiscard]] T& operator*() { return value_; }
  [[nodiscard]] const T& operator*() const { return value_; }
  [[nodiscard]] const PortError& error() const { return error_; }

 private:
  T value_{};
  PortError error_{};
  bool has_value_ = false;
};

#endif

using Fn = std::function<Json(const std::vector<Json>&)>;

class Registry {
 public:
  void add(std::string name, Fn fn) { functions_.emplace_back(std::move(name), std::move(fn)); }

  [[nodiscard]] const Fn* find(std::string_view name) const {
    for (const auto& [candidate, fn] : functions_) {
      if (candidate == name) return &fn;
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<std::string_view> names() const {
    std::vector<std::string_view> out;
    out.reserve(functions_.size());
    for (const auto& [name, unused] : functions_) out.push_back(name);
    return out;
  }

  [[nodiscard]] std::size_t size() const { return functions_.size(); }

 private:
  std::vector<std::pair<std::string, Fn>> functions_;
};

// The port may throw instead of returning std::unexpected when it mirrors an
// exception-throwing original; the driver turns both shapes into the same result.
[[nodiscard]] inline Json error_json(const PortError& error) {
  Json::Object fields;
  fields.emplace_back("type", Json::string(error.type));
  fields.emplace_back("msg", Json::string(error.message));
  Json::Object out;
  out.emplace_back("err", Json::object(std::move(fields)));
  return Json::object(std::move(out));
}

[[nodiscard]] inline Json error_json(std::string type, std::string message) {
  return error_json(PortError{std::move(type), std::move(message)});
}

[[nodiscard]] inline Json ok_json(Json value) {
  Json::Object out;
  out.emplace_back("ok", std::move(value));
  return Json::object(std::move(out));
}

// --------------------------------------------------------------------------
// decode: JSON -> the port's parameter type
// --------------------------------------------------------------------------

namespace detail {

[[noreturn]] inline void type_error(std::string_view fn, std::size_t index,
                                    const std::string& wanted, const Json& got) {
  throw PortError{"TypeError", "argument " + std::to_string(index) + " of " +
                                   std::string(fn) + " must be " + wanted + ", got " +
                                   repr(got, 60)};
}

template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
struct is_vector : std::false_type {};
template <typename T, typename Alloc>
struct is_vector<std::vector<T, Alloc>> : std::true_type {};

template <typename T>
struct is_map : std::false_type {};
template <typename T, typename Compare, typename Alloc>
struct is_map<std::map<std::string, T, Compare, Alloc>> : std::true_type {};

template <typename T>
struct is_tuple : std::false_type {};
template <typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type {};

template <typename T>
struct is_pair : std::false_type {};
template <typename A, typename B>
struct is_pair<std::pair<A, B>> : std::true_type {};

}  // namespace detail

template <typename T>
T decode(const Json& value, std::string_view fn, std::size_t index);

namespace detail {

template <typename Tuple, std::size_t... Indices>
Tuple decode_tuple(const Json& items, std::string_view fn, std::size_t index,
                   std::index_sequence<Indices...>) {
  if (items.size() != sizeof...(Indices)) {
    type_error(fn, index, "a tuple of " + std::to_string(sizeof...(Indices)) + " elements", items);
  }
  return Tuple{decode<std::tuple_element_t<Indices, Tuple>>(items.at(Indices), fn, index)...};
}

}  // namespace detail

template <typename T>
T decode(const Json& value, std::string_view fn, std::size_t index) {
  if constexpr (std::same_as<T, Json>) {
    return value;
  } else if constexpr (std::same_as<T, bool>) {
    if (!value.is_bool()) detail::type_error(fn, index, "a boolean", value);
    return value.as_bool();
  } else if constexpr (std::same_as<T, std::int64_t>) {
    if (!value.is_integer()) detail::type_error(fn, index, "an integer", value);
    return value.as_int();
  } else if constexpr (std::same_as<T, double>) {
    if (!value.is_number()) detail::type_error(fn, index, "a number", value);
    return value.as_number();
  } else if constexpr (std::same_as<T, std::string_view>) {
    if (!value.is_string()) detail::type_error(fn, index, "a string", value);
    return value.as_string();
  } else if constexpr (std::same_as<T, std::string>) {
    if (!value.is_string()) detail::type_error(fn, index, "a string", value);
    return value.as_string();
  } else if constexpr (detail::is_optional<T>::value) {
    if (value.is_null()) return std::nullopt;
    return T{decode<typename T::value_type>(value, fn, index)};
  } else if constexpr (detail::is_vector<T>::value) {
    // A tuple recorded by Python arrives as {"$tuple":[...]}; a list as [...].
    const Json* items = &value;
    if (const Json* tagged = value.find("$tuple"); tagged != nullptr && value.is_object()) {
      items = tagged;
    } else if (value.is_object() && value.find("$set") != nullptr) {
      items = value.find("$set");
    }
    if (!items->is_array()) detail::type_error(fn, index, "a list", value);
    T out;
    out.reserve(items->size());
    for (const auto& item : items->as_array()) {
      out.push_back(decode<typename T::value_type>(item, fn, index));
    }
    return out;
  } else if constexpr (detail::is_map<T>::value) {
    if (!value.is_object()) detail::type_error(fn, index, "an object", value);
    T out;
    for (const auto& [key, item] : value.as_object()) {
      out.emplace(key, decode<typename T::mapped_type>(item, fn, index));
    }
    return out;
  } else if constexpr (detail::is_tuple<T>::value) {
    const Json* items = value.find("$tuple");
    if (items == nullptr || !items->is_array()) detail::type_error(fn, index, "a tuple", value);
    return detail::decode_tuple<T>(*items, fn, index, std::make_index_sequence<std::tuple_size_v<T>>{});
  } else {
    static_assert(sizeof(T) == 0, "cppan::wire has no decoder for this parameter type");
  }
}

// --------------------------------------------------------------------------
// encode: the port's return type -> JSON
// --------------------------------------------------------------------------

template <typename T>
Json encode(const T& value);

template <typename T>
Json encode(const T& value) {
  if constexpr (std::same_as<T, Json>) {
    return value;
  } else if constexpr (std::same_as<T, bool>) {
    return Json::boolean(value);
  } else if constexpr (std::same_as<T, std::int64_t> || std::same_as<T, int> ||
                       std::same_as<T, long> || std::same_as<T, long long> ||
                       std::same_as<T, unsigned> || std::same_as<T, unsigned long> ||
                       std::same_as<T, unsigned long long> || std::same_as<T, short>) {
    return Json::integer(static_cast<std::int64_t>(value));
  } else if constexpr (std::same_as<T, double> || std::same_as<T, float>) {
    return Json::real(static_cast<double>(value));
  } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
    return Json::string(std::string(value));
  } else if constexpr (std::same_as<T, const char*>) {
    return Json::string(std::string(value));
  } else if constexpr (detail::is_optional<T>::value) {
    if (!value.has_value()) return Json::null();
    return encode<typename T::value_type>(*value);
  } else if constexpr (detail::is_vector<T>::value) {
    Json::Array out;
    out.reserve(value.size());
    for (const auto& item : value) out.push_back(encode(item));
    return Json::array(std::move(out));
  } else if constexpr (detail::is_map<T>::value) {
    Json::Object out;
    for (const auto& [key, item] : value) out.emplace_back(key, encode(item));
    return Json::object(std::move(out));
  } else if constexpr (detail::is_tuple<T>::value) {
    Json::Array items;
    std::apply([&](const auto&... part) { (items.push_back(encode(part)), ...); }, value);
    Json::Object out;
    out.emplace_back("$tuple", Json::array(std::move(items)));
    return Json::object(std::move(out));
  } else if constexpr (detail::is_pair<T>::value) {
    Json::Array items;
    items.push_back(encode(value.first));
    items.push_back(encode(value.second));
    Json::Object out;
    out.emplace_back("$tuple", Json::array(std::move(items)));
    return Json::object(std::move(out));
  } else if constexpr (std::is_enum_v<T>) {
    return Json::integer(static_cast<std::int64_t>(value));
  } else {
    static_assert(sizeof(T) == 0, "cppan::wire has no encoder for this return type");
  }
}

// A port returns `cppan::Result<T>`; the driver unwraps it so a signature change
// stays one line per function.
template <typename T>
[[nodiscard]] Json encode_result(const Result<T>& result) {
  if (!result.has_value()) return error_json(result.error());
  return ok_json(encode(*result));
}

}  // namespace cppan
