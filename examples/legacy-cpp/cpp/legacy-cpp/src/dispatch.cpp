// Generated dispatch: one lambda per ported function. Never edit by hand.
//
// `cpp-anything refresh` regenerates it when a signature changes.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/wire.hpp"
#include "legacy/port.hpp"

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
    return cppan::encode_result(legacy::word_count(arg0));
  });

  registry.add("digits_only", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("digits_only", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "digits_only", 0);
    return cppan::encode_result(legacy::digits_only(arg0));
  });

  registry.add("average_word_length", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("average_word_length", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "average_word_length", 0);
    return cppan::encode_result(legacy::average_word_length(arg0));
  });

  registry.add("sum_ascii", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("sum_ascii", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "sum_ascii", 0);
    return cppan::encode_result(legacy::sum_ascii(arg0));
  });

  registry.add("trim_copy", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("trim_copy", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "trim_copy", 0);
    return cppan::encode_result(legacy::trim_copy(arg0));
  });

  registry.add("repeat_join", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 2) bad_arity("repeat_join", args.size(), 2, 2);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "repeat_join", 0);
    const auto arg1 = cppan::decode<std::int64_t>(args.at(1), "repeat_join", 1);
    return cppan::encode_result(legacy::repeat_join(arg0, arg1));
  });

  registry.add("print_words", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("print_words", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "print_words", 0);
    return cppan::encode_result(legacy::print_words(arg0));
  });

}
