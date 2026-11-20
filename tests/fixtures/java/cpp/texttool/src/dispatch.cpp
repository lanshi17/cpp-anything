// Generated dispatch: one lambda per ported function. Never edit by hand.
//
// `cpp-anything refresh` regenerates it when a signature changes.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/wire.hpp"
#include "texttool/port.hpp"

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
  registry.add("wordCount", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("wordCount", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "wordCount", 0);
    return cppan::encode_result(texttool::wordCount(arg0));
  });

  registry.add("slug", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("slug", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "slug", 0);
    return cppan::encode_result(texttool::slug(arg0));
  });

  registry.add("compareLengths", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 2) bad_arity("compareLengths", args.size(), 2, 2);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "compareLengths", 0);
    const auto arg1 = cppan::decode<std::string_view>(args.at(1), "compareLengths", 1);
    return cppan::encode_result(texttool::compareLengths(arg0, arg1));
  });

  registry.add("compareInts", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 2) bad_arity("compareInts", args.size(), 2, 2);
    const auto arg0 = cppan::decode<std::int64_t>(args.at(0), "compareInts", 0);
    const auto arg1 = cppan::decode<std::int64_t>(args.at(1), "compareInts", 1);
    return cppan::encode_result(texttool::compareInts(arg0, arg1));
  });

}
