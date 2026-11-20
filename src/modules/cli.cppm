export module cppan.cli;
import std;
import cppan.util;
export import cppan.args;

export {
// The command line. No argument-parser library: the surface is small enough to
// own, and `--help` stays in one place next to the code that implements it.



namespace cppan::cli {




/// Runs one command; the value is the process exit code.
[[nodiscard]] Result<int> run(const std::vector<std::string>& argv);

/// The help text, also printed when `--help` appears after a command.
[[nodiscard]] std::string_view help_text();

}  // namespace cppan::cli
}
