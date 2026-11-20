#include "cppan/doctor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ranges>
#include <format>
#include <print>

#include "cppan/docs.hpp"
#include "cppan/json.hpp"
#include "cppan/matrix.hpp"
#include "cppan/modules.hpp"
#include "cppan/python.hpp"
#include "cppan/setup.hpp"

namespace cppan::doctor {
namespace {

struct Check {
  std::string name;
  std::string status;
  bool required = false;
  std::string detail;
  std::optional<std::string> fix;
};

std::optional<std::string> first_version(std::string_view banner) {
  std::string current;
  for (const char ch : banner) {
    if ((ch >= '0' && ch <= '9') || (ch == '.' && !current.empty())) {
      current += ch;
    } else if (!current.empty()) {
      if (std::ranges::count(current, '.') >= 1) {
        while (!current.empty() && current.back() == '.') current.pop_back();
        return current;
      }
      current.clear();
    }
  }
  if (!current.empty()) {
    while (!current.empty() && current.back() == '.') current.pop_back();
    return current;
  }
  return std::nullopt;
}

std::string first_line_of(std::string_view text) {
  const std::size_t newline = text.find('\n');
  return std::string(text.substr(0, newline));
}

Check program_check(std::string_view name, std::string_view program,
                    std::optional<std::string_view> minimum, bool required,
                    const std::vector<std::string>& version_args) {
  const auto path = which_on_path(program);
  if (!path.has_value()) {
    return Check{std::string(name), "missing", required,
                 std::string(program) + " not found or did not run",
                 program == "cmake"
                     ? "apt install cmake · brew install cmake · winget install Kitware.CMake"
                     : "install the tool and put it on PATH"};
  }
  const Run run = run_command(path->string(), version_args);
  std::string banner;
  if (!run.out.empty()) banner = first_line_of(run.out);
  if (banner.empty() && !run.err.empty()) banner = first_line_of(run.err);
  if (minimum.has_value()) {
    if (const auto version = first_version(banner); version.has_value()) {
      if (!version_at_least(*version, *minimum)) {
        return Check{std::string(name), "outdated", required,
                     banner + "; cpp-anything needs " + std::string(*minimum) + " or newer",
                     "apt install cmake · brew upgrade cmake · winget upgrade Kitware.CMake"};
      }
    }
  }
  return Check{std::string(name), "ok", required,
               banner.empty() ? std::string(program) + " on PATH" : banner, std::nullopt};
}

Check mcp_check() {
  std::error_code error;
  const auto executable = std::filesystem::canonical("/proc/self/exe", error);
  const std::filesystem::path binary =
      error ? std::filesystem::path("cpp-anything") : executable;
  const Json request = Json::object({
      {"jsonrpc", Json::string("2.0")},
      {"id", Json::integer(1)},
      {"method", Json::string("initialize")},
      {"params", Json::object({{"clientInfo", Json::object({{"name",
                                                              Json::string(
                                                                  "cpp-anything-doctor")}})}})},
  });
  const Json list = Json::object({
      {"jsonrpc", Json::string("2.0")},
      {"id", Json::integer(2)},
      {"method", Json::string("tools/list")},
  });
  const std::string input = dump(request) + "\n" + dump(list) + "\n";
  const Run run = run_command_with_input(binary.string(), {"mcp"}, input);
  if (!run.ok && run.status == std::nullopt) {
    return Check{"mcp server", "missing", true,
                 "`" + binary.string() + " mcp` could not start",
                 "run `cpp-anything mcp` by hand and look at what it prints"};
  }
  std::optional<std::size_t> tools;
  for (const std::string_view line : split_lines(run.out)) {
    try {
      const Json doc = parse(line);
      const Json* result = doc.find("result");
      if (result == nullptr) continue;
      const Json* list_field = result->find("tools");
      if (list_field != nullptr && list_field->is_array()) {
        tools = list_field->as_array().size();
      }
    } catch (const JsonError&) {
      continue;
    }
  }
  if (tools.has_value() && *tools > 0) {
    return Check{"mcp server", "ok", true,
                 "`" + binary.string() + " mcp` answered with " +
                     std::to_string(*tools) + " tools",
                 std::nullopt};
  }
  return Check{"mcp server", "missing", true,
               "`" + binary.string() + " mcp` did not list tools: " +
                   truncate_text(run.out, 160),
               "run `cpp-anything mcp` by hand and look at what it prints"};
}

Check agent_check(setup::Agent agent, const std::filesystem::path& home,
                  const std::filesystem::path& binary) {
  const std::filesystem::path config = setup::config_file(agent, home);
  const std::string executable = binary.string();
  const auto text = read_file(config);
  if (agent == setup::Agent::Codex) {
    std::string status;
    std::string detail;
    if (!text.has_value()) {
      status = "missing";
      detail = "no `" + std::string(setup::kServerKey) + "` server in " + config.string();
    } else {
      const std::string header = "[mcp_servers." + std::string(setup::kServerKey) + "]";
      std::string_view section;
      const std::size_t at = text->find(header);
      if (at == std::string::npos) {
        status = "missing";
        detail = "no `" + std::string(setup::kServerKey) + "` server in " + config.string();
      } else {
        section = std::string_view(*text).substr(at + header.size());
        const std::size_t next = section.find("\n[");
        if (next != std::string_view::npos) section = section.substr(0, next);
        const std::string command = "command = \"" + executable + "\"";
        if (section.find(command) != std::string_view::npos &&
            section.find("\"mcp\"") != std::string_view::npos) {
          status = "ok";
          detail = config.string() + " runs this binary's `mcp`";
        } else {
          status = "outdated";
          detail = config.string() + " runs a different command";
        }
      }
    }
    return Check{"agents", status, true,
                 std::string(setup::title(agent)) + ": " + detail, "cpp-anything setup"};
  }
  std::optional<Json> entry;
  if (text.has_value()) {
    try {
      const Json doc = parse(*text);
      std::vector<std::string_view> path;
      if (agent == setup::Agent::OpenCode) {
        if (setup::json_get(doc, {"mcp", "servers", setup::kServerKey}) != nullptr) {
          path = {"mcp", "servers", setup::kServerKey};
        } else {
          path = {"mcp", setup::kServerKey};
        }
      } else {
        path = {"mcpServers", setup::kServerKey};
      }
      if (const Json* found = setup::json_get(doc, path); found != nullptr) entry = *found;
    } catch (const JsonError&) {
      entry = std::nullopt;
    }
  }
  std::string status;
  std::string detail;
  if (!entry.has_value()) {
    status = "missing";
    detail = "no `" + std::string(setup::kServerKey) + "` server in " + config.string();
  } else {
    bool command_ok = false;
    if (agent == setup::Agent::OpenCode) {
      if (const Json* command = entry->find("command");
          command != nullptr && command->is_array()) {
        const auto& items = command->as_array();
        command_ok = items.size() >= 2 && items[0].is_string() &&
                     items[0].as_string() == executable && items[1].is_string() &&
                     items[1].as_string() == "mcp";
      }
    } else {
      const Json* command = entry->find("command");
      const Json* args = entry->find("args");
      bool args_ok = false;
      if (args != nullptr && args->is_array() && args->as_array().size() == 1) {
        const Json& first = args->as_array()[0];
        args_ok = first.is_string() && first.as_string() == "mcp";
      }
      command_ok = command != nullptr && command->is_string() &&
                   command->as_string() == executable && args_ok;
    }
    if (command_ok) {
      status = "ok";
      detail = config.string() + " runs this binary's `mcp`";
    } else {
      status = "outdated";
      detail = config.string() + " runs a different command";
    }
  }
  return Check{"agents", status, true, std::string(setup::title(agent)) + ": " + detail,
               "cpp-anything setup"};
}

}  // namespace

Result<int> command(const cli::Parsed& args) {
  std::vector<Check> checks;
  checks.push_back(program_check("cmake", "cmake", "3.28.0", true, {"--version"}));
  {
    std::optional<std::string> found;
    for (const std::string_view name : {"c++", "g++", "clang++", "cl"}) {
      if (which_on_path(name).has_value()) {
        found = std::string(name) + " on PATH";
        break;
      }
    }
    checks.push_back(Check{"c++ compiler", found.has_value() ? "ok" : "missing", true,
                           found.value_or("no c++, g++, clang++ or cl on PATH"),
                           "apt install g++ · xcode-select --install · install Visual Studio "
                           "Build Tools"});
  }
  {
    const auto ninja = modules::ninja();
    checks.push_back(Check{"ninja", ninja.has_value() ? "ok" : "missing", false,
                           ninja.has_value()
                               ? ninja->string()
                               : "only needed to build projects generated with --modules "
                                 "(gcc/clang)",
                           "python3 -m pip install --user ninja · apt install ninja-build · "
                           "brew install ninja"});
  }
  {
    const auto python = python::available();
    checks.push_back(Check{
        "python3", python.has_value() ? "ok" : "missing", true,
        python.has_value() ? python::interpreter() + " (" + *python + ")"
                           : python::interpreter() + " not found",
        "set CPPAN_PYTHON=/path/to/python3 if it is installed under another name"});
  }
  const Matrix& matrix = Matrix::get();
  checks.push_back(Check{
      "matrix", matrix.features.size() > 100 ? "ok" : "outdated", true,
      std::format("{} features over {} standards, generated {}", matrix.features.size(),
                  matrix.standards.size(), matrix.generated_at),
      "cpp-anything matrix sync, then rebuild"});
  {
    const std::filesystem::path guides_path = "docs/guides";
    const bool in_source = std::filesystem::is_directory(guides_path);
    std::vector<std::string> drifted;
    if (in_source) {
      auto result = docs::check(guides_path);
      if (result) drifted = *result;
    }
    std::string detail;
    if (!in_source) {
      detail = "no docs/guides in the current directory (installed binary): skipped";
    } else if (drifted.empty()) {
      detail = "docs/guides matches data/matrix.json";
    } else {
      detail = std::format("{} drifted: ", drifted.size());
      for (std::size_t index = 0; index < drifted.size(); ++index) {
        if (index > 0) detail += ", ";
        detail += drifted[index];
      }
    }
    checks.push_back(Check{"guides", drifted.empty() ? "ok" : "outdated", in_source, detail,
                           "cpp-anything matrix docs"});
  }
  checks.push_back(mcp_check());

  std::filesystem::path home;
  if (const char* value = std::getenv("HOME"); value != nullptr) {
    home = value;
  } else if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr) {
    home = profile;
  }
  std::error_code error;
  const auto executable = std::filesystem::canonical("/proc/self/exe", error);
  const std::filesystem::path binary =
      error ? std::filesystem::path("cpp-anything") : executable;
  bool any_agent = false;
  if (!home.empty()) {
    for (const setup::Agent agent : setup::kAllAgents) {
      if (!setup::present(agent, home)) continue;
      any_agent = true;
      checks.push_back(agent_check(agent, home, binary));
    }
  }
  if (!any_agent) {
    checks.push_back(Check{
        "agents", "ok", false,
        "no coding agent found (Claude Code, Codex, Cursor, Gemini CLI, OpenCode); the CLI "
        "works without one",
        "cpp-anything setup --agent claude"});
  }

  bool ok = true;
  for (const Check& check : checks) {
    if (check.required && check.status != "ok") ok = false;
  }
  if (args.flag("json")) {
    Json::Array entries;
    for (const Check& check : checks) {
      entries.push_back(Json::object({
          {"name", Json::string(check.name)},
          {"status", Json::string(check.status)},
          {"required", Json::boolean(check.required)},
          {"detail", Json::string(check.detail)},
          {"fix", check.fix.has_value() ? Json::string(*check.fix) : Json::null()},
      }));
    }
    std::print("{}\n", dump(Json::object({
                               {"schema", Json::string("cpp-anything/doctor@1")},
                               {"ok", Json::boolean(ok)},
                               {"checks", Json::array(std::move(entries))},
                           }),
                            true));
  } else {
    std::size_t width = 10;
    for (const Check& check : checks) width = std::max(width, check.name.size());
    std::print("cpp-anything doctor\n\n");
    for (const Check& check : checks) {
      const bool optional = !check.required && check.status != "ok";
      std::print("  {:<9} {}  {}{}\n", check.status, pad_right(check.name, width + 2),
                 check.detail, optional ? " (optional)" : "");
      if (check.status != "ok" && check.fix.has_value()) {
        std::print("  {:<9} {}  fix: {}\n", "", pad_right("", width + 2), *check.fix);
      }
    }
    std::print(
        "\n{}\n",
        ok ? "Everything required is in place. Language runtimes for other source languages are "
             "optional: each matters only for the languages you port."
           : "Something required is missing: cmake, a C++ compiler, python3, the matrix, the "
             "guides or the MCP server. Run the fix commands above.");
  }
  return ok ? 0 : 1;
}

}  // namespace cppan::doctor
