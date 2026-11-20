// Generated dispatch: one lambda per ported function. Never edit by hand.
//
// `cpp-anything refresh` regenerates it when a signature changes.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cppan/wire.hpp"
#include "textpkg/port.hpp"

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
  registry.add("WordCount", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("WordCount", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "WordCount", 0);
    return cppan::encode_result(textpkg::WordCount(arg0));
  });

  registry.add("Slash", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("Slash", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "Slash", 0);
    return cppan::encode_result(textpkg::Slash(arg0));
  });

  registry.add("CountBetween", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 2) bad_arity("CountBetween", args.size(), 2, 2);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "CountBetween", 0);
    const auto arg1 = cppan::decode<std::string_view>(args.at(1), "CountBetween", 1);
    return cppan::encode_result(textpkg::CountBetween(arg0, arg1));
  });

  registry.add("Greater", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 2) bad_arity("Greater", args.size(), 2, 2);
    const auto arg0 = cppan::decode<std::int64_t>(args.at(0), "Greater", 0);
    const auto arg1 = cppan::decode<std::int64_t>(args.at(1), "Greater", 1);
    return cppan::encode_result(textpkg::Greater(arg0, arg1));
  });

  registry.add("MustParse", [](const std::vector<cppan::Json>& args) -> cppan::Json {
    if (args.size() != 1) bad_arity("MustParse", args.size(), 1, 1);
    const auto arg0 = cppan::decode<std::string_view>(args.at(0), "MustParse", 0);
    return cppan::encode_result(textpkg::MustParse(arg0));
  });

}
