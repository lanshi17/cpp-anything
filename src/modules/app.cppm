export module cppan.app;
import std;
import cppan.args;
import cppan.util;

export {
// `app`: record a CLI black-box, replay any build against it, and put a C++ front
// door in front of the original so the app can ship before every unit is ported.


namespace cppan::app {

/// Runs `app <subcommand>`; the value is the process exit code.
[[nodiscard]] Result<int> command(const cli::Parsed& args);

}  // namespace cppan::app
}
