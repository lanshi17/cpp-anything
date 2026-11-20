#include "cppan/setup.hpp"

#include <algorithm>
#include <format>
#include <print>

#include "cppan/skill_md.hpp"

namespace cppan::setup {
namespace {

std::filesystem::path home_dir() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) home = std::getenv("USERPROFILE");
  if (home == nullptr || *home == '\0') return {};
  return home;
}

std::filesystem::path backup_path_for(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  return path.parent_path() / (name + "." + std::string(kServerKey) + ".bak");
}

Result<void> write_atomic(const std::filesystem::path& path, std::string_view text) {
  if (path.has_parent_path()) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return std::unexpected(path.parent_path().string() + ": " + error.message());
  }
  std::filesystem::path temp = path;
  temp.replace_extension("cpp-anything.tmp");
  if (auto written = write_file(temp, text); !written) return written;
  std::error_code error;
  if (std::filesystem::exists(path)) {
    const auto permissions = std::filesystem::status(path, error).permissions();
    if (!error) std::filesystem::permissions(temp, permissions, error);
  }
  std::filesystem::rename(temp, path, error);
  if (error) return std::unexpected(path.string() + ": " + error.message());
  return {};
}

const Json* json_get_impl(const Json& value, const std::vector<std::string_view>& path,
                          std::size_t index) {
  if (index == path.size()) return &value;
  const Json* next = value.find(path[index]);
  if (next == nullptr) return nullptr;
  return json_get_impl(*next, path, index + 1);
}

Json json_set_impl(Json value, const std::vector<std::string_view>& path, std::size_t index,
                   Json new_value) {
  if (index == path.size()) return new_value;
  if (!value.is_object()) value = Json::object();
  Json::Object pairs = value.as_object();
  for (auto& [name, item] : pairs) {
    if (name == path[index]) {
      item = json_set_impl(std::move(item), path, index + 1, std::move(new_value));
      return Json::object(std::move(pairs));
    }
  }
  Json nested = Json::object();
  nested = json_set_impl(std::move(nested), path, index + 1, std::move(new_value));
  pairs.emplace_back(std::string(path[index]), std::move(nested));
  return Json::object(std::move(pairs));
}

Json json_remove_impl(Json value, const std::vector<std::string_view>& path, std::size_t index) {
  if (!value.is_object() || index >= path.size()) return value;
  Json::Object pairs = value.as_object();
  if (index + 1 == path.size()) {
    std::erase_if(pairs, [&](const auto& pair) { return pair.first == path[index]; });
    return Json::object(std::move(pairs));
  }
  for (auto& [name, item] : pairs) {
    if (name == path[index]) {
      item = json_remove_impl(std::move(item), path, index + 1);
    }
  }
  return Json::object(std::move(pairs));
}

bool is_object_empty(const Json* value) {
  return value != nullptr && value->is_object() && value->as_object().empty();
}

Result<std::pair<std::string, std::optional<std::string>>> edit_json(
    const std::filesystem::path& path, const std::filesystem::path& executable, bool remove,
    std::optional<std::string_view> flavor) {
  std::optional<std::string> existing;
  std::error_code error;
  if (std::filesystem::exists(path)) {
    const auto text = read_file(path);
    if (!text.has_value()) return std::unexpected(path.string() + ": cannot read");
    existing = *text;
  }
  Json root;
  if (existing.has_value() && !existing->empty()) {
    bool blank = true;
    for (const char ch : *existing) {
      if (!std::isspace(static_cast<unsigned char>(ch))) blank = false;
    }
    if (!blank) {
      try {
        root = parse(*existing);
      } catch (const JsonError&) {
        return std::unexpected(path.string() +
                               " is not valid JSON; fix it or add the server by hand");
      }
    } else {
      root = Json::object();
    }
  } else {
    root = Json::object();
  }
  if (!root.is_object()) {
    return std::unexpected(path.string() + " is not a JSON object; add the server by hand");
  }

  const bool opencode = flavor.has_value() && *flavor == "opencode";
  const auto entry_path = [&]() -> std::vector<std::string_view> {
    if (opencode) {
      if (json_get(root, {"mcp", "servers", kServerKey}) != nullptr) {
        return {"mcp", "servers", kServerKey};
      }
      return {"mcp", kServerKey};
    }
    return {"mcpServers", kServerKey};
  };

  const bool had = json_get(root, entry_path()) != nullptr;

  if (remove) {
    if (!had) return std::pair{std::string("absent"), std::optional<std::string>()};
    if (opencode) {
      root = json_remove(std::move(root), {"mcp", "servers", kServerKey});
      root = json_remove(std::move(root), {"mcp", kServerKey});
      if (is_object_empty(json_get(root, {"mcp", "servers"}))) {
        root = json_remove(std::move(root), {"mcp", "servers"});
      }
      if (is_object_empty(json_get(root, {"mcp"}))) {
        root = json_remove(std::move(root), {"mcp"});
      }
    } else {
      root = json_remove(std::move(root), {"mcpServers", kServerKey});
    }
    return std::pair{std::string("removed"), std::optional<std::string>(dump(root, true) + "\n")};
  }

  const std::string executable_text = executable.string();
  bool unchanged = false;
  if (opencode) {
    const Json* entry = json_get(root, {"mcp", "servers", kServerKey});
    if (entry == nullptr) entry = json_get(root, {"mcp", kServerKey});
    if (entry != nullptr && entry->find("url") == nullptr && entry->find("disabled") == nullptr) {
      const Json* command = entry->find("command");
      if (command != nullptr && command->is_array() && command->as_array().size() == 2) {
        const auto& items = command->as_array();
        unchanged = items[0].is_string() && items[0].as_string() == executable_text &&
                    items[1].is_string() && items[1].as_string() == "mcp";
      }
    }
  } else {
    const Json* entry = json_get(root, {"mcpServers", kServerKey});
    if (entry != nullptr && entry->find("url") == nullptr) {
      const Json* command = entry->find("command");
      const Json* args = entry->find("args");
      bool args_ok = false;
      if (args != nullptr && args->is_array() && args->as_array().size() == 1) {
        const Json& first = args->as_array()[0];
        args_ok = first.is_string() && first.as_string() == "mcp";
      }
      unchanged = command != nullptr && command->is_string() &&
                  command->as_string() == executable_text && args_ok;
    }
  }
  if (unchanged) return std::pair{std::string("unchanged"), std::optional<std::string>()};

  const std::string action = had ? "updated" : "added";
  Json::Object entry;
  if (opencode) {
    entry.emplace_back("type", Json::string("local"));
    entry.emplace_back("command",
                       Json::array({Json::string(executable_text), Json::string("mcp")}));
  } else {
    entry.emplace_back("command", Json::string(executable_text));
    entry.emplace_back("args", Json::array({Json::string("mcp")}));
    entry.emplace_back("type", Json::string("stdio"));
    if (flavor.has_value() && *flavor == "claude") {
      entry.emplace_back("env", Json::object());
    }
  }
  // Keep everything a user had on our entry except the parts that would break it.
  const Json* previous = opencode ? json_get(root, {"mcp", "servers", kServerKey}) : nullptr;
  if (previous == nullptr && opencode) previous = json_get(root, {"mcp", kServerKey});
  if (previous == nullptr && !opencode) previous = json_get(root, {"mcpServers", kServerKey});
  if (previous != nullptr && previous->is_object()) {
    for (const auto& [key, value] : previous->as_object()) {
      if (key == "command" || key == "args" || key == "url" || key == "disabled" ||
          key == "enabled" || key == "type" || key == "env") {
        continue;
      }
      entry.emplace_back(key, value);
    }
  }
  const Json object = Json::object(entry);

  if (opencode) {
    bool servers_object = false;
    if (const Json* servers = json_get(root, {"mcp", "servers"});
        servers != nullptr && servers->is_object()) {
      Json::Object pairs = servers->as_object();
      std::erase_if(pairs, [](const auto& pair) { return pair.first == kServerKey; });
      pairs.emplace_back(std::string(kServerKey), object);
      root = json_set(std::move(root), {"mcp", "servers"}, Json::object(std::move(pairs)));
      servers_object = true;
    }
    if (!servers_object) {
      Json servers = Json::object();
      servers = json_set(std::move(servers), {kServerKey}, object);
      root = json_set(std::move(root), {"mcp"}, Json::object({{"servers", servers}}));
    }
  } else {
    root = json_set(std::move(root), {"mcpServers", kServerKey}, object);
  }
  return std::pair{action, std::optional<std::string>(dump(root, true) + "\n")};
}

Result<std::pair<std::string, std::optional<std::string>>> edit_toml(
    const std::filesystem::path& path, const std::filesystem::path& executable, bool remove) {
  std::optional<std::string> existing;
  std::error_code error;
  if (std::filesystem::exists(path)) {
    const auto text = read_file(path);
    if (!text.has_value()) return std::unexpected(path.string() + ": cannot read");
    existing = *text;
  }
  const std::string text = existing.value_or(std::string());
  const std::string header = "[mcp_servers." + std::string(kServerKey) + "]";
  const auto lines = split_lines(text);
  std::optional<std::size_t> start;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    std::string_view trimmed = lines[index];
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
      trimmed.remove_suffix(1);
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
      trimmed.remove_prefix(1);
    }
    if (trimmed == header) {
      start = index;
      break;
    }
  }
  std::size_t end = lines.size();
  if (start.has_value()) {
    for (std::size_t index = *start + 1; index < lines.size(); ++index) {
      std::string_view trimmed = lines[index];
      while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.remove_prefix(1);
      }
      if (trimmed.starts_with('[')) {
        end = index;
        break;
      }
    }
  }
  bool had = start.has_value();
  if (!had) {
    const std::string needle = "mcp_servers." + std::string(kServerKey) + " =";
    for (const std::string_view line : lines) {
      if (line.find(needle) != std::string_view::npos) had = true;
    }
  }
  if (remove) {
    if (!had) return std::pair{std::string("absent"), std::optional<std::string>()};
    std::vector<std::string> out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      if (start.has_value() && index >= *start && index < end) continue;
      out.emplace_back(lines[index]);
    }
    std::string result;
    for (std::size_t index = 0; index < out.size(); ++index) {
      if (index > 0) result += '\n';
      result += out[index];
    }
    if (!result.empty() && !result.ends_with('\n')) result += '\n';
    return std::pair{std::string("removed"), std::optional<std::string>(result)};
  }

  const std::string executable_text = executable.string();
  bool unchanged = false;
  if (start.has_value()) {
    bool has_url = false;
    bool args_ok = false;
    bool command_ok = false;
    for (std::size_t index = *start + 1; index < end; ++index) {
      std::string_view line = lines[index];
      while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
        line.remove_prefix(1);
      }
      if (line.starts_with("url")) has_url = true;
      if (line.starts_with("args") && line.find("\"mcp\"") != std::string_view::npos) {
        args_ok = true;
      }
      if (line.starts_with("command") &&
          line.find(executable_text) != std::string_view::npos) {
        command_ok = true;
      }
    }
    unchanged = command_ok && !has_url && args_ok;
  }
  if (unchanged) return std::pair{std::string("unchanged"), std::optional<std::string>()};

  const std::string action = had ? "updated" : "added";
  std::string block = header + "\ncommand = " + quote_debug(executable_text) +
                      "\nargs = [\"mcp\"]";
  std::vector<std::string> out;
  if (start.has_value()) {
    for (std::size_t index = 0; index < *start; ++index) out.emplace_back(lines[index]);
    out.push_back(block);
    for (std::size_t index = end; index < lines.size(); ++index) out.emplace_back(lines[index]);
  } else {
    for (const std::string_view line : lines) out.emplace_back(line);
    if (!out.empty() && !out.back().empty()) out.emplace_back();
    out.push_back(block);
  }
  std::string result;
  for (std::size_t index = 0; index < out.size(); ++index) {
    if (index > 0) result += '\n';
    result += out[index];
  }
  if (!result.ends_with('\n')) result += '\n';
  return std::pair{action, std::optional<std::string>(result)};
}

Result<std::pair<std::string, std::string>> install_skill(const std::filesystem::path& dir,
                                                          bool remove, bool dry_run) {
  const std::filesystem::path skill = dir / "SKILL.md";
  const std::string body(data::skill_md, data::skill_md_size);
  const auto existing = read_file(skill);
  if (remove) {
    if (!existing.has_value()) return std::pair{std::string("absent"), skill.string()};
    if (!dry_run) {
      std::error_code error;
      std::filesystem::remove(skill, error);
      std::filesystem::remove(dir, error);
    }
    return std::pair{std::string("removed"), skill.string()};
  }
  std::string action;
  if (!existing.has_value()) {
    action = "added";
  } else if (*existing == body) {
    action = "unchanged";
  } else {
    action = "updated";
  }
  if (action != "unchanged" && !dry_run) {
    if (auto written = write_file(skill, body); !written) {
      return std::unexpected(written.error());
    }
  }
  return std::pair{action, skill.string()};
}

Result<std::vector<Change>> wire_agent(Agent agent, const std::filesystem::path& home,
                                       const std::filesystem::path& executable, bool remove,
                                       bool dry_run) {
  const std::filesystem::path config = config_file(agent, home);
  const std::string file = config.string();
  Result<std::pair<std::string, std::optional<std::string>>> edited =
      std::unexpected("unknown agent");
  switch (agent) {
    case Agent::Codex: edited = edit_toml(config, executable, remove); break;
    case Agent::OpenCode: edited = edit_json(config, executable, remove, "opencode"); break;
    case Agent::Cursor:
    case Agent::Gemini: edited = edit_json(config, executable, remove, std::nullopt); break;
    case Agent::Claude: edited = edit_json(config, executable, remove, "claude"); break;
  }
  if (!edited) return std::unexpected(edited.error());
  auto [action, changed_text] = std::move(*edited);
  std::optional<std::string> backup;
  std::error_code error;
  if (!dry_run && action != "unchanged" && action != "absent") {
    if (changed_text.has_value()) {
      if (std::filesystem::exists(config)) {
        const auto backup_path = backup_path_for(config);
        std::filesystem::copy_file(config, backup_path,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) return std::unexpected(backup_path.string() + ": " + error.message());
        backup = backup_path.string();
      }
      if (auto written = write_atomic(config, *changed_text); !written) {
        return std::unexpected(written.error());
      }
    }
  }
  std::vector<Change> changes;
  changes.push_back(Change{std::string(slug(agent)), file, action, backup});
  if (const auto dir = skill_dir(agent, home); dir.has_value()) {
    auto skill = install_skill(*dir, remove, dry_run);
    if (!skill) return std::unexpected(skill.error());
    if (skill->first != "unchanged") {
      changes.push_back(Change{std::string(slug(agent)), skill->second, skill->first,
                               std::nullopt});
    }
  }
  return changes;
}

}  // namespace

std::string_view slug(Agent agent) {
  switch (agent) {
    case Agent::Claude: return "claude";
    case Agent::Codex: return "codex";
    case Agent::Cursor: return "cursor";
    case Agent::Gemini: return "gemini";
    case Agent::OpenCode: return "opencode";
  }
  return "claude";
}

std::string_view title(Agent agent) {
  switch (agent) {
    case Agent::Claude: return "Claude Code";
    case Agent::Codex: return "Codex";
    case Agent::Cursor: return "Cursor";
    case Agent::Gemini: return "Gemini CLI";
    case Agent::OpenCode: return "OpenCode";
  }
  return "Claude Code";
}

std::string_view marker_dir(Agent agent) {
  switch (agent) {
    case Agent::Claude: return ".claude";
    case Agent::Codex: return ".codex";
    case Agent::Cursor: return ".cursor";
    case Agent::Gemini: return ".gemini";
    case Agent::OpenCode: return ".config/opencode";
  }
  return ".claude";
}

std::string_view command_name(Agent agent) {
  switch (agent) {
    case Agent::Claude: return "claude";
    case Agent::Codex: return "codex";
    case Agent::Cursor: return "cursor";
    case Agent::Gemini: return "gemini";
    case Agent::OpenCode: return "opencode";
  }
  return "claude";
}

std::filesystem::path config_file(Agent agent, const std::filesystem::path& home) {
  switch (agent) {
    case Agent::Claude: return home / ".claude.json";
    case Agent::Codex: return home / ".codex" / "config.toml";
    case Agent::Cursor: return home / ".cursor" / "mcp.json";
    case Agent::Gemini: return home / ".gemini" / "settings.json";
    case Agent::OpenCode: return home / ".config" / "opencode" / "opencode.json";
  }
  return home / ".claude.json";
}

std::optional<std::filesystem::path> skill_dir(Agent agent,
                                                const std::filesystem::path& home) {
  switch (agent) {
    case Agent::Claude: return home / ".claude" / "skills" / std::string(kServerKey);
    case Agent::Codex: return home / ".codex" / "skills" / std::string(kServerKey);
    case Agent::OpenCode:
      return home / ".config" / "opencode" / "skills" / std::string(kServerKey);
    case Agent::Cursor:
    case Agent::Gemini: return std::nullopt;
  }
  return std::nullopt;
}

bool present(Agent agent, const std::filesystem::path& home) {
  return std::filesystem::exists(home / marker_dir(agent)) ||
         which_on_path(command_name(agent)).has_value();
}

const Json* json_get(const Json& value, const std::vector<std::string_view>& path) {
  return json_get_impl(value, path, 0);
}

Json json_set(Json value, const std::vector<std::string_view>& path, Json new_value) {
  return json_set_impl(std::move(value), path, 0, std::move(new_value));
}

Json json_remove(Json value, const std::vector<std::string_view>& path) {
  return json_remove_impl(std::move(value), path, 0);
}

Result<int> command(const cli::Parsed& args) {
  const auto home = home_dir();
  if (home.empty()) return std::unexpected("no home directory (HOME is not set)");
  std::error_code error;
  const auto executable = std::filesystem::canonical(cppan::current_executable(), error);
  const std::filesystem::path binary =
      error ? cppan::current_executable() : executable;

  std::vector<Agent> requested;
  if (args.many("agent").empty()) {
    for (const Agent agent : kAllAgents) {
      if (present(agent, home)) requested.push_back(agent);
    }
  } else {
    for (const std::string& name : args.many("agent")) {
      std::optional<Agent> found;
      for (const Agent agent : kAllAgents) {
        if (slug(agent) == name) found = agent;
      }
      if (!found.has_value()) {
        return std::unexpected("unknown agent " + quote_debug(name) +
                               ": claude|codex|cursor|gemini|opencode");
      }
      requested.push_back(*found);
    }
  }
  if (requested.empty()) {
    return std::unexpected(
        "no coding agent found (looked for ~/.claude, ~/.codex, ~/.cursor, ~/.gemini, "
        "~/.config/opencode and their commands); name one with "
        "--agent claude|codex|cursor|gemini|opencode");
  }
  const bool remove = args.flag("remove");
  const bool dry_run = args.flag("dry-run");
  std::vector<Change> changes;
  for (const Agent agent : requested) {
    auto wired = wire_agent(agent, home, binary, remove, dry_run);
    if (!wired) return std::unexpected(wired.error());
    for (Change& change : *wired) changes.push_back(std::move(change));
  }

  if (args.flag("json")) {
    Json::Array agents;
    for (const Agent agent : requested) agents.push_back(Json::string(std::string(slug(agent))));
    Json::Array changes_json;
    for (const Change& change : changes) {
      Json::Object fields;
      fields.emplace_back("agent", Json::string(change.agent));
      fields.emplace_back("file", Json::string(change.file));
      fields.emplace_back("action", Json::string(change.action));
      if (change.backup.has_value()) {
        fields.emplace_back("backup", Json::string(*change.backup));
      }
      changes_json.push_back(Json::object(std::move(fields)));
    }
    std::print("{}\n", dump(Json::object({
                               {"command", Json::string(binary.string())},
                               {"agents", Json::array(std::move(agents))},
                               {"changes", Json::array(std::move(changes_json))},
                               {"dry_run", Json::boolean(dry_run)},
                           }),
                            true));
  } else {
    std::string names;
    for (const Agent agent : requested) {
      if (!names.empty()) names += ", ";
      names += title(agent);
    }
    std::print("{} {} for {}\n", dry_run ? "Would set up" : "Set up", kServerKey, names);
    for (const Change& change : changes) {
      std::print("  {:<10} {:<9} {}\n", change.agent, change.action, change.file);
      if (change.backup.has_value()) {
        std::print("  {:<10} {:<9} {}\n", "", "backup", *change.backup);
      }
    }
    bool touched = false;
    for (const Change& change : changes) {
      if (change.action != "removed" && change.action != "absent") touched = true;
    }
    if (!dry_run && touched && !remove) {
      std::print(
          "\nThe agents run `{} mcp`. Restart them, then ask, for example:\n  \"Port this repo's "
          "hottest unit to C++ with cpp-anything and stop when gate passes.\"\n",
          binary.string());
    }
  }
  return 0;
}

}  // namespace cppan::setup
