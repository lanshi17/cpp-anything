#include "cppan/mcp.hpp"

#include <cstdio>
#include <iostream>
#include <print>
#include <sstream>

#include "cppan/json.hpp"
#include "cppan/skill_md.hpp"

namespace cppan::mcp {
namespace {

constexpr std::string_view kProtocol = "2025-06-18";

struct Tool {
  std::string_view name;
  std::string_view title;
  std::string_view description;
  std::string_view schema;
  bool read_only;
  /// Whether the tool runs the original's code or commands.
  bool runs_original;
};

const Tool kTools[] = {
    Tool{"analyze", "Analyze a repository", "Plan a port: the modules in a repository, their functions, the risks each one carries, and where to start.",
         R"JSON({"type":"object","properties":{"root":{"type":"string"},"lang":{"type":"string","enum":["python"]},"whole_app":{"type":"boolean"},"json":{"type":"boolean"}},"required":["root"]})JSON", true, false},
    Tool{"bridge", "Generate the C++ project for one unit", "Write the port skeleton, dispatch table, JSON driver, CMake project and profile header for one unit.",
         R"JSON({"type":"object","properties":{"root":{"type":"string"},"unit":{"type":"string"},"out":{"type":"string"},"toolchain":{"type":"string"},"version":{"type":"string"},"std":{"type":"string"},"force":{"type":"boolean"},"module_root":{"type":"string"}},"required":["root","unit"]})JSON", false, false},
    Tool{"refresh", "Regenerate the generated files", "Re-read the original's signatures and regenerate everything except src/port.cpp.",
         R"JSON({"type":"object","properties":{"project":{"type":"string"}},"required":["project"]})JSON", false, false},
    Tool{"record", "Record the original's behaviour", "Run the original on generated edge cases (and optionally its own test suite) and save what it returns, raises and writes.",
         R"JSON({"type":"object","properties":{"project":{"type":"string"},"count":{"type":"number"},"seed":{"type":"number"},"inputs":{"type":"string"},"float_tolerance":{"type":"number"},"from_tests":{"type":"boolean"},"test_command":{"type":"array","items":{"type":"string"}}},"required":["project"]})JSON", false, true},
    Tool{"build", "Build the C++ project", "Configure and build the generated project with CMake.",
         R"JSON({"type":"object","properties":{"project":{"type":"string"},"clean":{"type":"boolean"},"cxx":{"type":"string"},"jobs":{"type":"number"}},"required":["project"]})JSON", false, false},
    Tool{"diff", "Compare the port with the recording", "Replay every recorded case through the C++ port and compare; reports args, expected, got and why.",
         R"JSON({"type":"object","properties":{"project":{"type":"string"},"json":{"type":"boolean"},"strict_errors":{"type":"boolean"},"tolerance":{"type":"number"},"only":{"type":"string"},"max_report":{"type":"number"}},"required":["project"]})JSON", true, false},
    Tool{"profile", "Profile a workload", "Run a workload under the language's profiler and rank what to move: hot functions, callers, and batch advice.",
         R"JSON({"type":"object","properties":{"root":{"type":"string"},"command":{"type":"array","items":{"type":"string"}},"json":{"type":"boolean"},"top":{"type":"number"}},"required":["root","command"]})JSON", true, true},
    Tool{"bench", "Benchmark the port", "Time the recorded cases on the original and on the port, and say whether it is worth shipping.",
         R"JSON({"type":"object","properties":{"project":{"type":"string"},"repeat":{"type":"number"},"json":{"type":"boolean"}},"required":["project"]})JSON", true, true},
    Tool{"shadow", "Run the port beside the original", "Generate the drop-in shadow module: the original returns, the port runs second, disagreements are logged.",
         R"JSON({"type":"object","properties":{"project":{"type":"string"},"report":{"type":"boolean"},"check":{"type":"boolean"},"json":{"type":"boolean"}},"required":["project"]})JSON", false, false},
    Tool{"map", "Map the port", "Every unit, its project, cases and last diff, what is still only in Python, and which originals changed since they were recorded.",
         R"JSON({"type":"object","properties":{"repo":{"type":"string"},"crates":{"type":"array","items":{"type":"string"}},"write":{"type":"boolean"},"check":{"type":"boolean"},"json":{"type":"boolean"}},"required":["repo"]})JSON", true, false},
    Tool{"gate", "Check the definition of done", "Matrix, no stubs, error paths, build, dispatch and every recorded case; optional bench.",
         R"JSON({"type":"object","properties":{"project":{"type":"string"},"json":{"type":"boolean"},"clean":{"type":"boolean"},"bench":{"type":"boolean"}},"required":["project"]})JSON", false, true},
    Tool{"matrix_features", "What a compiler supports", "Supported, partial and missing features of one C++ standard for a compiler release, from cppreference's tables.",
         R"JSON({"type":"object","properties":{"toolchain":{"type":"string"},"version":{"type":"string"},"std":{"type":"string"},"kind":{"type":"string","enum":["all","core","library"]}},"required":["toolchain","version","std"]})JSON", true, false},
    Tool{"matrix_explain", "Explain one feature", "One feature by name or feature-test macro: first release per compiler, notes, paper.",
         R"JSON({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})JSON", true, false},
    Tool{"matrix_profile", "Generate profile.hpp", "The CPPAN_HAS_* header for a target toolchain, computed from the matrix.",
         R"JSON({"type":"object","properties":{"toolchain":{"type":"string"},"version":{"type":"string"},"std":{"type":"string"}},"required":["toolchain","version","std"]})JSON", true, false},
    Tool{"app_record", "Record a whole CLI", "Run the original CLI on its documented examples, your inputs and semantic traps, twice each, and store what it did.",
         R"JSON({"type":"object","properties":{"repo":{"type":"string"},"run":{"type":"array","items":{"type":"string"}},"inputs":{"type":"string"},"normalize":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"timeout":{"type":"number"},"traps":{"type":"boolean"},"float_tolerance":{"type":"number"},"json":{"type":"boolean"}},"required":["repo"]})JSON", false, true},
    Tool{"app_replay", "Replay a build against the app cases", "Run any build (the original, the front door, the final binary) against the recorded cases and report per-case mismatches.",
         R"JSON({"type":"object","properties":{"repo":{"type":"string"},"build":{"type":"string"},"timeout":{"type":"number"},"case":{"type":"string"},"json":{"type":"boolean"}},"required":["repo"]})JSON", true, true},
    Tool{"app_front_door", "Scaffold the C++ front door", "Write and build cpp/app: the binary that owns the entry point and falls back to the original for unwired commands.",
         R"JSON({"type":"object","properties":{"repo":{"type":"string"},"no_check":{"type":"boolean"},"json":{"type":"boolean"}},"required":["repo"]})JSON", false, true},
    Tool{"app_wire", "Wire a unit into the front door", "Route one command to a ported unit: adds the route, the glue stub, the kill switch and the library link.",
         R"JSON({"type":"object","properties":{"repo":{"type":"string"},"unit":{"type":"string"},"command":{"type":"string"},"project":{"type":"string"}},"required":["repo","unit","command"]})JSON", false, false},
    Tool{"app_lift", "Lift the app", "Refuse while any case still falls back; mark the app standalone when every case is answered by the front door.",
         R"JSON({"type":"object","properties":{"repo":{"type":"string"},"json":{"type":"boolean"}},"required":["repo"]})JSON", false, true},
    Tool{"setup", "Register with the coding agents", "Write the MCP server entry (and the skill where the agent reads skills) for the agents found on this machine.",
         R"JSON({"type":"object","properties":{"agent":{"type":"array","items":{"type":"string"}},"remove":{"type":"boolean"},"dry_run":{"type":"boolean"},"json":{"type":"boolean"}}})JSON", false, false},
    Tool{"doctor", "Check the tools", "cmake, a C++ compiler, python3, the matrix, the guides, this MCP server and the agent entries, with the fix for each.",
         R"JSON({"type":"object","properties":{"json":{"type":"boolean"}}})JSON", true, false},};

Json result_response(Json id, Json result) {
  return Json::object({
      {"jsonrpc", Json::string("2.0")},
      {"id", std::move(id)},
      {"result", std::move(result)},
  });
}

Json error_response(Json id, std::int64_t code, std::string_view message) {
  return Json::object({
      {"jsonrpc", Json::string("2.0")},
      {"id", std::move(id)},
      {"error", Json::object({{"code", Json::integer(code)},
                              {"message", Json::string(std::string(message))}})},
  });
}

void progress_note(const Json& token, std::int64_t progress, std::string_view message) {
  const Json note = Json::object({
      {"jsonrpc", Json::string("2.0")},
      {"method", Json::string("notifications/progress")},
      {"params", Json::object({
                     {"progressToken", token},
                     {"progress", Json::integer(progress)},
                     {"total", Json::integer(1)},
                     {"message", Json::string(std::string(message))},
                 })},
  });
  std::print("{}\n", dump(note));
  std::fflush(stdout);
}

Json tools_list() {
  Json::Array tools;
  for (const Tool& tool : kTools) {
    Json input_schema;
    try {
      input_schema = parse(tool.schema);
    } catch (const JsonError&) {
      input_schema = Json::object();
    }
    tools.push_back(Json::object({
        {"name", Json::string(std::string(tool.name))},
        {"title", Json::string(std::string(tool.title))},
        {"description", Json::string(std::string(tool.description))},
        {"inputSchema", std::move(input_schema)},
        {"annotations", Json::object({
                            {"title", Json::string(std::string(tool.title))},
                            {"readOnlyHint", Json::boolean(tool.read_only)},
                            {"destructiveHint", Json::boolean(false)},
                            {"idempotentHint", Json::boolean(!tool.runs_original)},
                            {"openWorldHint", Json::boolean(tool.runs_original)},
                        })},
    }));
  }
  return Json::object({{"tools", Json::array(std::move(tools))}});
}

Json prompts_list() {
  return Json::object({{"prompts", Json::array({
      Json::object({
          {"name", Json::string("port_unit")},
          {"title", Json::string("Port one unit to C++")},
          {"description", Json::string("Port one unit of a repository to C++ with cpp-anything, "
                                       "and prove it.")},
          {"arguments", Json::array({
              Json::object({{"name", Json::string("path")},
                            {"description", Json::string("Repository root.")},
                            {"required", Json::boolean(true)}}),
              Json::object({{"name", Json::string("unit")},
                            {"description", Json::string("Unit id from analyze; the best first "
                                                         "port when omitted.")},
                            {"required", Json::boolean(false)}}),
          })},
      }),
      Json::object({
          {"name", Json::string("port_whole_app")},
          {"title", Json::string("Port a whole app to C++")},
          {"description", Json::string("Record the app, put a C++ front door in front of it, "
                                       "and walk it to standalone.")},
          {"arguments", Json::array({
              Json::object({{"name", Json::string("path")},
                            {"description", Json::string("Repository root.")},
                            {"required", Json::boolean(true)}}),
          })},
      }),
  })}});
}

std::string skill_body() {
  std::string text(data::skill_md, data::skill_md_size);
  if (text.starts_with("---")) {
    const std::size_t end = text.find("\n---\n", 3);
    if (end != std::string::npos) text = text.substr(end + 5);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.erase(text.begin());
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  return text;
}

Result<Json> prompts_get(const Json& params) {
  const Json* name_field = params.find("name");
  const std::string name =
      name_field != nullptr && name_field->is_string() ? name_field->as_string() : "";
  Json arguments = Json::object();
  if (const Json* value = params.find("arguments")) arguments = *value;
  const Json* path_field = arguments.find("path");
  if (path_field == nullptr || !path_field->is_string()) {
    return std::unexpected("prompt `" + name + "` needs the `path` argument");
  }
  const std::string path = path_field->as_string();
  const std::string skill = skill_body();
  std::string title;
  std::string text;
  if (name == "port_unit") {
    title = "Port one unit to C++";
    const Json* unit = arguments.find("unit");
    std::string lead;
    if (unit != nullptr && unit->is_string()) {
      lead = "Port the unit `" + unit->as_string() + "` of the repository at " + path +
             " to C++ with cpp-anything. Call the port done only when gate passes.";
    } else {
      lead = "Port one unit of the repository at " + path +
             " to C++ with cpp-anything: run `analyze` on it first and take the best first port "
             "from `start_here`. Call the port done only when gate passes.";
    }
    text = lead + "\n\nFollow this guide:\n\n" + skill;
  } else if (name == "port_whole_app") {
    title = "Port a whole app to C++";
    text = "Rewrite the app at " + path +
           " as a C++ front door that answers every recorded case, keeping the original only as "
           "reference.\n\nWork in this order: `app record`, `app replay` (the original against its "
           "own cases), `app front-door`, then one unit at a time through the per-unit loop, "
           "`app wire` for each command it answers, and `app lift` when nothing falls back."
           "\n\nFollow this guide:\n\n" + skill;
  } else {
    return std::unexpected("unknown prompt `" + name + "`");
  }
  return Json::object({
      {"description", Json::string(title)},
      {"messages", Json::array({Json::object({
                       {"role", Json::string("user")},
                       {"content", Json::object({{"type", Json::string("text")},
                                                 {"text", Json::string(text)}})},
                   })})},
  });
}

std::optional<std::string> arg_str(const Json& arguments, std::string_view key) {
  const Json* value = arguments.find(key);
  if (value == nullptr || !value->is_string()) return std::nullopt;
  return value->as_string();
}

Result<std::string> required(const Json& arguments, std::string_view key) {
  if (auto value = arg_str(arguments, key); value.has_value()) return *value;
  return std::unexpected("missing `" + std::string(key) + "`");
}

void push_flag(std::vector<std::string>& argv, const Json& arguments, std::string_view flag,
               std::string_view key) {
  const Json* value = arguments.find(key);
  if (value == nullptr) return;
  if (value->is_bool() && value->as_bool()) {
    argv.push_back("--" + std::string(flag));
  } else if (value->is_string()) {
    argv.push_back("--" + std::string(flag));
    argv.push_back(value->as_string());
  } else if (value->is_integer()) {
    argv.push_back("--" + std::string(flag));
    argv.push_back(std::to_string(value->as_int()));
  } else if (value->is_real()) {
    argv.push_back("--" + std::string(flag));
    argv.push_back(std::format("{}", value->as_real()));
  }
}

void push_repeated(std::vector<std::string>& argv, const Json& arguments, std::string_view flag,
                   std::string_view key) {
  const Json* list = arguments.find(key);
  if (list == nullptr || !list->is_array()) return;
  for (const Json& item : list->as_array()) {
    if (!item.is_string()) continue;
    argv.push_back("--" + std::string(flag));
    argv.push_back(item.as_string());
  }
}

Result<std::vector<std::string>> build_argv(std::string_view name, const Json& arguments) {
  std::vector<std::string> argv;
  const auto add = [&](std::string_view flag, std::string_view key) {
    push_flag(argv, arguments, flag, key);
  };
  if (name == "analyze") {
    argv.push_back("analyze");
    auto root = required(arguments, "root");
    if (!root) return std::unexpected(root.error());
    argv.push_back(*root);
    add("lang", "lang");
    add("whole-app", "whole_app");
    argv.push_back("--json");
  } else if (name == "bridge") {
    argv.push_back("bridge");
    auto root = required(arguments, "root");
    if (!root) return std::unexpected(root.error());
    argv.push_back(*root);
    auto unit = required(arguments, "unit");
    if (!unit) return std::unexpected(unit.error());
    argv.push_back(*unit);
    for (const auto& [flag, key] :
         {std::pair<std::string_view, std::string_view>{"out", "out"},
          {"toolchain", "toolchain"},
          {"version", "version"},
          {"std", "std"},
          {"force", "force"},
          {"module-root", "module_root"}}) {
      add(flag, key);
    }
  } else if (name == "refresh") {
    argv.push_back("refresh");
    auto project = required(arguments, "project");
    if (!project) return std::unexpected(project.error());
    argv.push_back(*project);
  } else if (name == "record") {
    argv.push_back("record");
    auto project = required(arguments, "project");
    if (!project) return std::unexpected(project.error());
    argv.push_back(*project);
    for (const auto& [flag, key] :
         {std::pair<std::string_view, std::string_view>{"count", "count"},
          {"seed", "seed"},
          {"inputs", "inputs"},
          {"float-tolerance", "float_tolerance"},
          {"from-tests", "from_tests"}}) {
      add(flag, key);
    }
    std::vector<std::string> tests;
    if (const Json* list = arguments.find("test_command");
        list != nullptr && list->is_array()) {
      for (const Json& item : list->as_array()) {
        if (item.is_string()) tests.push_back(item.as_string());
      }
    }
    if (!tests.empty()) {
      argv.push_back("--");
      for (std::string& test : tests) argv.push_back(std::move(test));
    }
  } else if (name == "build") {
    argv.push_back("build");
    auto project = required(arguments, "project");
    if (!project) return std::unexpected(project.error());
    argv.push_back(*project);
    for (const auto& [flag, key] :
         {std::pair<std::string_view, std::string_view>{"clean", "clean"},
          {"cxx", "cxx"},
          {"jobs", "jobs"}}) {
      add(flag, key);
    }
  } else if (name == "diff") {
    argv.push_back("diff");
    auto project = required(arguments, "project");
    if (!project) return std::unexpected(project.error());
    argv.push_back(*project);
    for (const auto& [flag, key] :
         {std::pair<std::string_view, std::string_view>{"json", "json"},
          {"strict-errors", "strict_errors"},
          {"tolerance", "tolerance"},
          {"only", "only"},
          {"max-report", "max_report"}}) {
      add(flag, key);
    }
  } else if (name == "profile") {
    argv.push_back("profile");
    auto root = required(arguments, "root");
    if (!root) return std::unexpected(root.error());
    argv.push_back(*root);
    add("json", "json");
    add("top", "top");
    const Json* command = arguments.find("command");
    if (command == nullptr || !command->is_array()) return std::unexpected("missing `command`");
    std::vector<std::string> parts;
    for (const Json& item : command->as_array()) {
      if (item.is_string()) parts.push_back(item.as_string());
    }
    if (parts.empty()) return std::unexpected("missing `command`");
    argv.push_back("--");
    for (std::string& part : parts) argv.push_back(std::move(part));
  } else if (name == "bench") {
    argv.push_back("bench");
    auto project = required(arguments, "project");
    if (!project) return std::unexpected(project.error());
    argv.push_back(*project);
    add("repeat", "repeat");
    add("json", "json");
  } else if (name == "shadow") {
    argv.push_back("shadow");
    auto project = required(arguments, "project");
    if (!project) return std::unexpected(project.error());
    argv.push_back(*project);
    add("report", "report");
    add("check", "check");
    add("json", "json");
  } else if (name == "map") {
    argv.push_back("map");
    auto repo = required(arguments, "repo");
    if (!repo) return std::unexpected(repo.error());
    argv.push_back(*repo);
    push_repeated(argv, arguments, "crates", "crates");
    add("write", "write");
    add("check", "check");
    add("json", "json");
  } else if (name == "gate") {
    argv.push_back("gate");
    auto project = required(arguments, "project");
    if (!project) return std::unexpected(project.error());
    argv.push_back(*project);
    add("json", "json");
    add("clean", "clean");
    add("bench", "bench");
  } else if (name == "matrix_features") {
    argv.push_back("matrix");
    argv.push_back("features");
    for (const auto& [flag, key] :
         {std::pair<std::string_view, std::string_view>{"toolchain", "toolchain"},
          {"version", "version"},
          {"std", "std"},
          {"kind", "kind"}}) {
      add(flag, key);
    }
    argv.push_back("--json");
  } else if (name == "matrix_explain") {
    argv.push_back("matrix");
    argv.push_back("explain");
    auto query = required(arguments, "query");
    if (!query) return std::unexpected(query.error());
    argv.push_back(*query);
    argv.push_back("--json");
  } else if (name == "matrix_profile") {
    argv.push_back("matrix");
    argv.push_back("profile");
    for (const auto& [flag, key] :
         {std::pair<std::string_view, std::string_view>{"toolchain", "toolchain"},
          {"version", "version"},
          {"std", "std"}}) {
      add(flag, key);
    }
  } else if (name == "app_record") {
    argv.push_back("app");
    argv.push_back("record");
    auto repo = required(arguments, "repo");
    if (!repo) return std::unexpected(repo.error());
    argv.push_back(*repo);
    for (const auto& [flag, key] :
         {std::pair<std::string_view, std::string_view>{"inputs", "inputs"},
          {"name", "name"},
          {"timeout", "timeout"},
          {"float-tolerance", "float_tolerance"}}) {
      add(flag, key);
    }
    push_repeated(argv, arguments, "normalize", "normalize");
    push_repeated(argv, arguments, "fresh", "fresh");
    push_repeated(argv, arguments, "arg", "args");
    const Json* traps = arguments.find("traps");
    if (traps != nullptr && traps->is_bool() && !traps->as_bool()) {
      argv.push_back("--no-traps");
    }
    if (const Json* run = arguments.find("run"); run != nullptr && run->is_array()) {
      std::vector<std::string> parts;
      for (const Json& item : run->as_array()) {
        if (item.is_string()) parts.push_back(item.as_string());
      }
      if (!parts.empty()) {
        argv.push_back("--");
        for (std::string& part : parts) argv.push_back(std::move(part));
      }
    }
  } else if (name == "app_replay") {
    argv.push_back("app");
    argv.push_back("replay");
    auto repo = required(arguments, "repo");
    if (!repo) return std::unexpected(repo.error());
    argv.push_back(*repo);
    for (const auto& [flag, key] :
         {std::pair<std::string_view, std::string_view>{"build", "build"},
          {"timeout", "timeout"},
          {"case", "case"},
          {"json", "json"}}) {
      add(flag, key);
    }
  } else if (name == "app_front_door") {
    argv.push_back("app");
    argv.push_back("front-door");
    auto repo = required(arguments, "repo");
    if (!repo) return std::unexpected(repo.error());
    argv.push_back(*repo);
    add("no-check", "no_check");
    add("json", "json");
  } else if (name == "app_wire") {
    argv.push_back("app");
    argv.push_back("wire");
    auto repo = required(arguments, "repo");
    if (!repo) return std::unexpected(repo.error());
    argv.push_back(*repo);
    auto unit = required(arguments, "unit");
    if (!unit) return std::unexpected(unit.error());
    argv.push_back(*unit);
    add("command", "command");
    add("project", "project");
  } else if (name == "app_lift") {
    argv.push_back("app");
    argv.push_back("lift");
    auto repo = required(arguments, "repo");
    if (!repo) return std::unexpected(repo.error());
    argv.push_back(*repo);
    add("json", "json");
  } else if (name == "setup") {
    argv.push_back("setup");
    push_repeated(argv, arguments, "agent", "agent");
    add("remove", "remove");
    add("dry-run", "dry_run");
    add("json", "json");
  } else if (name == "doctor") {
    argv.push_back("doctor");
    argv.push_back("--json");
  } else {
    return std::unexpected("unknown tool `" + std::string(name) + "`");
  }
  return argv;
}

Json failure(std::string_view code, std::string_view message,
             const std::optional<std::string>& next) {
  return Json::object({
      {"code", Json::string(std::string(code))},
      {"message", Json::string(std::string(message))},
      {"retryable", Json::boolean(false)},
      {"next", next.has_value() ? Json::string(*next) : Json::null()},
  });
}

std::string classify(std::string_view text) {
  if (text.find("no recorded cases") != std::string_view::npos ||
      (text.find("app record") != std::string_view::npos &&
       text.find("no cases") != std::string_view::npos)) {
    return "no_recording";
  }
  if (text.find("changed since it was recorded") != std::string_view::npos) {
    return "stale_recording";
  }
  if (text.find("is not built yet") != std::string_view::npos ||
      text.find("build failed") != std::string_view::npos) {
    return "build_failed";
  }
  return "failed";
}

std::optional<std::string> next_hint(std::string_view code, const Json& arguments) {
  std::string path = "<project>";
  for (const std::string_view key : {"project", "repo", "root"}) {
    if (const Json* value = arguments.find(key); value != nullptr && value->is_string()) {
      path = value->as_string();
      break;
    }
  }
  if (code == "no_recording" || code == "stale_recording") return "cpp-anything record " + path;
  if (code == "build_failed") return "cpp-anything build " + path;
  return std::nullopt;
}

struct ToolError {
  std::string text;
  Json structured;
};

Result<std::string> call_tool(const Json& params) {
  const Json* name_field = params.find("name");
  if (name_field == nullptr || !name_field->is_string()) {
    return std::unexpected("tools/call needs a name");
  }
  const std::string name = name_field->as_string();
  Json arguments = Json::object();
  if (const Json* value = params.find("arguments")) arguments = *value;
  bool known = false;
  for (const Tool& tool : kTools) {
    if (tool.name == name) known = true;
  }
  if (!known) return std::unexpected("unknown tool `" + name + "`");
  auto argv = build_argv(name, arguments);
  if (!argv) return std::unexpected(argv.error());
  const auto executable = cppan::current_executable();
  std::vector<std::string> args = *argv;
  const Run run = run_command(executable.string(), args);
  if (run.ok) return run.out;
  std::string joined;
  for (const std::string& arg : args) {
    if (!joined.empty()) joined += ' ';
    joined += arg;
  }
  return std::unexpected("cpp-anything " + joined + " failed: " + run.failure() + "\n" + run.err);
}

}  // namespace

Result<int> serve() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    bool blank = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) blank = false;
    }
    if (blank) continue;
    Json request;
    try {
      request = parse(line);
    } catch (const JsonError& error) {
      std::print("{}\n", dump(error_response(Json::null(), -32700,
                                             "parse error: " + std::string(error.what()))));
      std::fflush(stdout);
      continue;
    }
    Json id = Json::null();
    if (const Json* value = request.find("id")) id = *value;
    const Json* method_field = request.find("method");
    const std::string method =
        method_field != nullptr && method_field->is_string() ? method_field->as_string() : "";
    Json params = Json::object();
    if (const Json* value = request.find("params")) params = *value;
    if (id.is_null()) continue;
    std::optional<Json> progress;
    if (const Json* meta = params.find("_meta"); meta != nullptr) {
      if (const Json* token = meta->find("progressToken")) progress = *token;
    }
    if (progress.has_value()) progress_note(*progress, 0, "running " + method);

    Json response;
    if (method == "initialize") {
      const Json* requested_field = params.find("protocolVersion");
      const std::string requested = requested_field != nullptr && requested_field->is_string()
                                        ? requested_field->as_string()
                                        : std::string(kProtocol);
      response = result_response(std::move(id), Json::object({
          {"protocolVersion", Json::string(requested)},
          {"capabilities", Json::object({{"tools", Json::object()},
                                         {"prompts", Json::object()}})},
          {"serverInfo", Json::object({{"name", Json::string("cpp-anything")},
                                       {"version", Json::string(CPPAN_VERSION)}})},
          {"instructions", Json::string(
              "Port one unit at a time: analyze, bridge, record, write src/port.cpp, diff, gate. "
              "`matrix` answers what the target toolchain supports; `app` records a whole CLI and "
              "puts a C++ front door in front of it. Never mark a port done before gate passes.")},
      }));
    } else if (method == "ping") {
      response = result_response(std::move(id), Json::object());
    } else if (method == "tools/list") {
      response = result_response(std::move(id), tools_list());
    } else if (method == "prompts/list") {
      response = result_response(std::move(id), prompts_list());
    } else if (method == "prompts/get") {
      auto result = prompts_get(params);
      if (result) {
        response = result_response(std::move(id), std::move(*result));
      } else {
        response = error_response(std::move(id), -32602, result.error());
      }
    } else if (method == "tools/call") {
      auto result = call_tool(params);
      if (result) {
        response = result_response(std::move(id), Json::object({
            {"content", Json::array({Json::object({{"type", Json::string("text")},
                                                   {"text", Json::string(*result)}})})},
            {"isError", Json::boolean(false)},
        }));
      } else {
        const std::string code = classify(result.error());
        const Json structured = failure(code, result.error(), next_hint(code, params));
        response = result_response(std::move(id), Json::object({
            {"content", Json::array({Json::object({{"type", Json::string("text")},
                                                   {"text", Json::string(result.error())}})})},
            {"structuredContent", structured},
            {"isError", Json::boolean(true)},
        }));
      }
    } else {
      response = error_response(std::move(id), -32601, "unknown method `" + method + "`");
    }
    if (progress.has_value()) progress_note(*progress, 1, "finished");
    std::print("{}\n", dump(response));
    std::fflush(stdout);
  }
  return 0;
}

}  // namespace cppan::mcp
