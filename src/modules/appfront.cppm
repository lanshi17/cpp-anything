export module cppan.appfront;
import std;
import cppan.appcase;
import cppan.util;

export {
// The C++ front door: a binary that owns the CLI's entry point, answers what has
// been wired to a port, and falls back to the original for everything else.
//
// Fallback is explicit and auditable: every case says whether it ran the port or
// the original, and `app lift` refuses while any case still falls back.



namespace cppan::appfront {

struct Route {
  std::string command;
  /// HTTP route: `Method /path/{param}` (or `*`), for servers.
  std::optional<std::string> method;
  std::optional<std::string> path;
  std::string unit;
  std::string project;
};

/// Writes the front door for `config`; the value is the number of files written.
[[nodiscard]] Result<std::size_t> write(const std::filesystem::path& app_dir,
                                        const appcase::Config& config);

[[nodiscard]] std::size_t route_count(const std::filesystem::path& app_dir);

/// `app wire <repo> <unit> --route "GET /path"`: registers native HTTP routes and
/// writes the unit's handler stub. Requests the handler declines still proxy.
[[nodiscard]] Result<std::filesystem::path> wire_http(
    const std::filesystem::path& app_dir, const std::filesystem::path& repo,
    const std::string& unit, const std::vector<std::string>& texts,
    std::optional<std::filesystem::path> project);

[[nodiscard]] Result<std::filesystem::path> wire(const std::filesystem::path& app_dir,
                                                 const std::filesystem::path& repo,
                                                 const std::string& unit,
                                                 const std::string& command,
                                                 std::optional<std::filesystem::path> project);

/// `app front-door --library`: a C++ library that re-exports every bridged unit's
/// port, with no entry point and no fallback.
[[nodiscard]] Result<std::size_t> write_library(const std::filesystem::path& repo,
                                                const std::filesystem::path& app_dir);

[[nodiscard]] Result<std::filesystem::path> build(const std::filesystem::path& app_dir);

[[nodiscard]] Result<std::filesystem::path> build_library(const std::filesystem::path& app_dir);

}  // namespace cppan::appfront
}
