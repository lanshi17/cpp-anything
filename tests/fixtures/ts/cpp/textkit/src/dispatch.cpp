// Generated dispatch: one lambda per ported function. Never edit by hand.
//
// `cpp-anything refresh` regenerates it when a signature changes.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/wire.hpp"
#include "textkit/port.hpp"

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
  registry.add("squish", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("squish", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "squish", 0);
    return cppan::encode_result(textkit::squish(arg0));
  });

  registry.add("parseCount", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("parseCount", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "parseCount", 0);
    return cppan::encode_result(textkit::parseCount(arg0));
  });

  registry.add("commonPrefix", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 2) bad_arity("commonPrefix", args.size(), 2, 2);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "commonPrefix", 0);
    const auto arg1 = cppan::decode<std::string_view>(args.at(1), "commonPrefix", 1);
    return cppan::encode_result(textkit::commonPrefix(arg0, arg1));
  });

  registry.add("compare", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 2) bad_arity("compare", args.size(), 2, 2);
    const auto arg0 = cppan::decode<double>(args.at(0), "compare", 0);
    const auto arg1 = cppan::decode<double>(args.at(1), "compare", 1);
    return cppan::encode_result(textkit::compare(arg0, arg1));
  });

}
