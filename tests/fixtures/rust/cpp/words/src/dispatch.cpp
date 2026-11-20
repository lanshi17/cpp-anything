// Generated dispatch: one lambda per ported function. Never edit by hand.
//
// `cpp-anything refresh` regenerates it when a signature changes.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/wire.hpp"
#include "words/port.hpp"

namespace {

template <typename... Args>
[[noreturn]] void bad_arity(const char* fn, std::size_t got, std::size_t lowest,
                            std::size_t highest) {
  std::string expected = std::to_string(lowest);
  if (highest != lowest) expected += " to " + std::to_string(highest);
  throw cppan::PortError{"TypeError", std::string(fn) + "() takes " + expected +
                                            " arguments (" + std::to_string(got) + " given)"};
}

}  // namespace

void cppan_register(cppan::Registry& registry) {
  registry.add("word_count", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("word_count", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "word_count", 0);
    return cppan::encode_result(words::word_count(arg0));
  });

  registry.add("digits_only", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("digits_only", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "digits_only", 0);
    return cppan::encode_result(words::digits_only(arg0));
  });

  registry.add("repeat_count", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 2) bad_arity("repeat_count", args.size(), 2, 2);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "repeat_count", 0);
    const auto arg1 = cppan::decode<std::string_view>(args.at(1), "repeat_count", 1);
    return cppan::encode_result(words::repeat_count(arg0, arg1));
  });

  registry.add("average_word_length", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("average_word_length", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "average_word_length", 0);
    return cppan::encode_result(words::average_word_length(arg0));
  });

  registry.add("slugify", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("slugify", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "slugify", 0);
    return cppan::encode_result(words::slugify(arg0));
  });

  registry.add("is_palindrome", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("is_palindrome", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "is_palindrome", 0);
    return cppan::encode_result(words::is_palindrome(arg0));
  });

  registry.add("initials", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("initials", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "initials", 0);
    return cppan::encode_result(words::initials(arg0));
  });

}
