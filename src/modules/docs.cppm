export module cppan.docs;
import std;
import cppan.util;

export {
// `matrix docs`: the guides, rendered from the matrix.
//
// Every guide is generated: the numbers come from `data/matrix.json`, the prose
// that is not a number is a short curated note with the vendor page it comes from.
// `matrix docs --check` compares the rendered text with the files on disk, so a
// stale guide fails the build instead of lying quietly.



namespace cppan::docs {

[[nodiscard]] std::vector<std::pair<std::string, std::string>> all();
[[nodiscard]] Result<std::size_t> write(const std::filesystem::path& root);
[[nodiscard]] Result<std::vector<std::string>> check(const std::filesystem::path& root);

}  // namespace cppan::docs
}
