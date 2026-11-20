export module cppan.setup;
import std;
import cppan.args;
import cppan.json;
import cppan.util;

export {
// `setup`: register cpp-anything with the coding agents on this machine.
//
// Writes only its own entries, keeps every other setting byte-for-byte where it
// can, backs up a file before changing it, and is safe to run again. `--remove`
// takes the entries out again.



namespace cppan::setup {

inline constexpr std::string_view kServerKey = "cpp-anything";

enum class Agent : std::uint8_t { Claude, Codex, Cursor, Gemini, OpenCode };

inline constexpr Agent kAllAgents[] = {Agent::Claude, Agent::Codex, Agent::Cursor, Agent::Gemini,
                                       Agent::OpenCode};

[[nodiscard]] std::string_view slug(Agent agent);
[[nodiscard]] std::string_view title(Agent agent);
[[nodiscard]] std::string_view marker_dir(Agent agent);
[[nodiscard]] std::string_view command_name(Agent agent);
[[nodiscard]] std::filesystem::path config_file(Agent agent, const std::filesystem::path& home);
[[nodiscard]] std::optional<std::filesystem::path> skill_dir(Agent agent,
                                                             const std::filesystem::path& home);
[[nodiscard]] bool present(Agent agent, const std::filesystem::path& home);

struct Change {
  std::string agent;
  std::string file;
  std::string action;
  std::optional<std::string> backup;
};

[[nodiscard]] Result<int> command(const cli::Parsed& args);

/// Tiny JSON tree helpers (objects only, order preserved).
[[nodiscard]] const Json* json_get(const Json& value, const std::vector<std::string_view>& path);
[[nodiscard]] Json json_set(Json value, const std::vector<std::string_view>& path, Json new_value);
[[nodiscard]] Json json_remove(Json value, const std::vector<std::string_view>& path);

}  // namespace cppan::setup
}
