export module cppan.langc;
import std;
import cppan.inventory;
import cppan.json;
import cppan.project;
import cppan.types;
import cppan.util;

export {
// The C plugin: units, functions, a recording harness, and the corpora that make
// a C port checkable.
//
// C is where a C++ port should be almost mechanical, so the harness is strict: it
// links the original with renamed symbols, forks per case (a segfault is a case,
// not a lost run), and writes the same wire format Python recordings use.



namespace cppan::langc {

enum class Lang : std::uint8_t {
  C,
  /// C++ written in a C style: the port's job is to make it modern C++20/23.
  Cpp,
};

[[nodiscard]] std::string_view id(Lang lang);
[[nodiscard]] std::string_view label(Lang lang);
/// `extern "C" {` / `}` for C; nothing for C++.
[[nodiscard]] std::pair<std::string_view, std::string_view> linkage(Lang lang);
[[nodiscard]] std::string_view cmake_languages(Lang lang);
[[nodiscard]] std::optional<Lang> parse_lang(std::string_view name);

struct CFunction {
  std::string name;
  std::uint32_t line = 0;
  Ty ret;
  std::string ret_c;
  std::vector<std::pair<Ty, std::string>> params;
  /// (C type, name)
  std::vector<std::pair<std::string, std::string>> params_c;
};

struct Analysis {
  std::vector<CFunction> functions;
  std::vector<std::pair<std::string, std::string>> skipped;
  std::uint32_t lines = 0;
};

[[nodiscard]] Result<Analysis> analyze_file(const std::filesystem::path& path, Lang lang);
[[nodiscard]] Result<Unit> unit(const std::filesystem::path& repo,
                                std::string_view relative_or_path, Lang lang);
[[nodiscard]] Result<Json> analyze_repo(const std::filesystem::path& repo, Lang lang);

/// Calls with generated arguments, before the harness has run them.
[[nodiscard]] std::vector<Json> calls(const Unit& unit, std::size_t count);
[[nodiscard]] std::string harness_source(const std::vector<CFunction>& functions, Lang lang);
[[nodiscard]] Result<std::filesystem::path> build_harness(const Project& project,
                                                          const Unit& unit, Lang lang);

struct Recorded {
  std::size_t cases = 0;
  std::size_t crashes = 0;
};

/// Records the original: generated calls in, the harness's results out.
[[nodiscard]] Result<Recorded> record(const Project& project, const Unit& unit, std::size_t count,
                                      Lang lang);
[[nodiscard]] Result<BenchMeasurement> bench_original(const Project& project, const Unit& unit,
                                                      std::size_t repeat, Lang lang);

}  // namespace cppan::langc
}
