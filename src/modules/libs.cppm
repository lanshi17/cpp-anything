export module cppan.libs;
import std;

export {
// The library table: Python package -> C++ library, with an honest fit.
//
// Hand-curated on purpose: a guessed mapping is worse than an admitted unknown.
// `fit = none` means there is no equivalent worth handing to a port, and that
// blocks the unit instead of silently hand-rolling one.


namespace cppan::libs {

enum class Fit { Good, Partial, None, Kept };

[[nodiscard]] std::string_view fit_label(Fit fit);

struct Entry {
  std::vector<std::string> names;
  std::string libs;
  Fit fit = Fit::Partial;
  std::string note;
};

[[nodiscard]] const Entry* find(std::string_view package);
/// `@scope/name/sub` -> `@scope/name`, `pkg.module` -> `pkg`: the part a table
/// entry can name.
[[nodiscard]] std::string_view reduce(std::string_view import);

}  // namespace cppan::libs
}
