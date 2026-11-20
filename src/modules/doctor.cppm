export module cppan.doctor;
import std;
import cppan.args;
import cppan.util;

export {
// `doctor`: the tools a port needs, whether they are there, and how to install them.


namespace cppan::doctor {

[[nodiscard]] Result<int> command(const cli::Parsed& args);

}  // namespace cppan::doctor
}
