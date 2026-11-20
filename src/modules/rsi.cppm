export module cppan.rsi;
import std;
import cppan.args;
import cppan.util;

export {
// `rsi`: cpp-anything checking itself with its own loop. Every step either proves a
// shipped artefact still works or says exactly what drifted.


namespace cppan::rsi {

[[nodiscard]] Result<int> command(const cli::Parsed& args);

}  // namespace cppan::rsi
}
