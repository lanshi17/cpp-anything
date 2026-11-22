#pragma once

#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace test {

class Failure : public std::exception {
 public:
  Failure(const char* file, int line, std::string message)
      : message_(std::string(file) + ":" + std::to_string(line) + ": " +
                 std::move(message)) {}

  [[nodiscard]] const char* what() const noexcept override {
    return message_.c_str();
  }

 private:
  std::string message_;
};

struct Case {
  const char* name;
  void (*run)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

inline std::string repr(std::string_view value) {
  return "\"" + std::string(value) + "\"";
}
inline std::string repr(const std::string& value) { return repr(std::string_view(value)); }
inline std::string repr(const char* value) { return repr(std::string_view(value)); }
inline std::string repr(bool value) { return value ? "true" : "false"; }
template <class T>
std::string repr(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}
template <class T>
std::string repr(const std::vector<T>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ", ";
    out += repr(values[i]);
  }
  out += "]";
  return out;
}

}  // namespace test

#define TEST(name)                                             \
  static void name();                                          \
  static ::test::Registrar registrar_##name(#name, &name);     \
  static void name()

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      throw ::test::Failure(__FILE__, __LINE__, "CHECK(" #condition ")");   \
    }                                                                       \
  } while (false)

#define CHECK_EQ(actual, expected)                                          \
  do {                                                                      \
    const auto& actual_value = (actual);                                    \
    const auto& expected_value = (expected);                                \
    if (!(actual_value == expected_value)) {                                \
      throw ::test::Failure(                                                \
          __FILE__, __LINE__,                                               \
          std::string("expected ") + ::test::repr(actual_value) +           \
              " == " + ::test::repr(expected_value));                       \
    }                                                                       \
  } while (false)

#define CHECK_THROWS(expression)                              \
  do {                                                        \
    bool threw = false;                                       \
    try {                                                     \
      (void)(expression);                                     \
    } catch (const std::exception&) {                         \
      threw = true;                                           \
    }                                                         \
    if (!threw) {                                             \
      throw ::test::Failure(__FILE__, __LINE__,               \
                            "expected exception: " #expression); \
    }                                                         \
  } while (false)
