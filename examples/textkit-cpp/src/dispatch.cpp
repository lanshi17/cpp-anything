// Generated dispatch: one lambda per ported function. Never edit by hand.
//
// `cpp-anything refresh` regenerates it when a signature changes.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/wire.hpp"
#include "textkit_words/port.hpp"

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
  registry.add("slug", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("slug", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "slug", 0);
    return cppan::encode_result(textkit_words::slug(arg0));
  });

  registry.add("truncate", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() < 1 || args.size() > 2) bad_arity("truncate", args.size(), 1, 2);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "truncate", 0);
    std::int64_t arg1 = 12;
    if (args.size() > 1) arg1 = cppan::decode<std::int64_t>(args.at(1), "truncate", 1);
    return cppan::encode_result(textkit_words::truncate(arg0, arg1));
  });

  registry.add("word_count", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("word_count", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "word_count", 0);
    return cppan::encode_result(textkit_words::word_count(arg0));
  });

  registry.add("digits_only", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("digits_only", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "digits_only", 0);
    return cppan::encode_result(textkit_words::digits_only(arg0));
  });

  registry.add("summarize", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() < 1 || args.size() > 2) bad_arity("summarize", args.size(), 1, 2);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "summarize", 0);
    std::int64_t arg1 = 4;
    if (args.size() > 1) arg1 = cppan::decode<std::int64_t>(args.at(1), "summarize", 1);
    return cppan::encode_result(textkit_words::summarize(arg0, arg1));
  });

  registry.add("require_positive", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("require_positive", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::int64_t>(args.at(0), "require_positive", 0);
    return cppan::encode_result(textkit_words::require_positive(arg0));
  });

}
