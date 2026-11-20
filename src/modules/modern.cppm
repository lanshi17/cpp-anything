export module cppan.modern;
import std;

export {
// What to modernize: the C idioms a C-style C++ unit uses, and the C++20/23
// facility that replaces each one.
//
// This is the point of the `cpp` language in cpp-anything: the port is not just
// behaviour-preserving, it has to be *modern* — and which facilities are available
// is answered by the same matrix as everything else (`CPPAN_HAS_*`).



namespace cppan::modern {

struct Opportunity {
  std::string function;
  std::uint32_t line = 0;
  std::string idiom;
  std::string replacement;
  /// Feature-test macro the replacement needs, when it needs one.
  std::optional<std::string> feature;

  [[nodiscard]] std::string describe() const;
};

[[nodiscard]] struct FunctionStart {
  std::uint32_t line = 0;
  std::string name;
};

[[nodiscard]] std::vector<Opportunity> scan(std::string_view source,
                                            const std::vector<FunctionStart>& functions);

}  // namespace cppan::modern
}
