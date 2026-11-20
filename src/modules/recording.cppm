export module cppan.recording;
import std;
import cppan.project;

export {
// Whether a recording still describes the source it was made from.



namespace cppan::recording {

/// The saved hash is taken by `record`; when it differs, the cases describe code
/// that no longer exists and `diff` passing would mean nothing.
[[nodiscard]] std::optional<std::string> stale(const Project& project);

}  // namespace cppan::recording
}
