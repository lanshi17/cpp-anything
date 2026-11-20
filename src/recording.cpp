module;

module cppan.recording;
import cppan.types;
import cppan.json;
import cppan.util;
import std;

namespace cppan::recording {

std::optional<std::string> stale(const Project& project) {
  const std::filesystem::path path = project.dir / kMetaDir / "recorded-from.json";
  const auto text = read_file(path);
  if (!text.has_value()) return std::nullopt;
  Json doc;
  try {
    doc = parse(*text);
  } catch (const JsonError&) {
    return std::nullopt;
  }
  const Json* stored = doc.find("source_hash");
  if (stored == nullptr || !stored->is_string()) return std::nullopt;
  const std::string current = hash64(read_file(project.source_file).value_or(std::string()));
  if (stored->as_string() == current) return std::nullopt;
  return std::format(
      "{} changed since it was recorded: re-run `cpp-anything record {}` so the port is checked "
      "against what the original does now",
      short_path(project.source_file), project.dir.string());
}

}  // namespace cppan::recording
