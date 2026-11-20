export module cppan.analyze;
import std;
import cppan.json;
import cppan.util;

export {
// `analyze`: what is in the repository, what porting each unit costs, and where to
// start. `--whole-app` adds the app's shape, every dependency with its C++ library,
// the port order and the decisions the user has to make.



namespace cppan::analyze {

struct Resolved {
  std::filesystem::path file;
  std::string module;
  std::optional<std::filesystem::path> package_root;
};

[[nodiscard]] Result<Resolved> resolve(const std::filesystem::path& repo, std::string_view unit,
                                       const std::optional<std::filesystem::path>& module_root);

/// The Python probe's analysis, or the plugin's, depending on the language.
[[nodiscard]] Result<Json> analyze(const std::filesystem::path& repo);

[[nodiscard]] Json to_json(const Json& doc, const std::filesystem::path& repo, bool whole_app);
[[nodiscard]] std::string render_plan(const Json& doc, const std::filesystem::path& repo,
                                      bool whole_app);
[[nodiscard]] std::string summarize(const Json& doc);
[[nodiscard]] Result<void> write_plan(const std::filesystem::path& path, std::string_view text);

}  // namespace cppan::analyze
}
