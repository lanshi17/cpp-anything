module;

module cppan.portmap;
import cppan.json;
import cppan.types;
import cppan.util;
import cppan.inventory;
import cppan.project;
import cppan.python;
import cppan.recording;
import std;

namespace cppan::portmap {
namespace {

std::vector<std::filesystem::path> walk(const std::filesystem::path& directory,
                                        std::size_t depth) {
  std::vector<std::filesystem::path> out;
  if (depth > 4) return out;
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) return out;
  for (const auto& entry : iterator) {
    if (!entry.is_directory(error)) continue;
    const std::string name = entry.path().filename().string();
    if (name == "build" || name == "node_modules" || name == "target" || name == "vendor" ||
        name == ".git" || name == "__pycache__" || name == ".venv" || name == "venv" ||
        name.starts_with('.')) {
      continue;
    }
    out.push_back(entry.path());
    for (auto& nested : walk(entry.path(), depth + 1)) out.push_back(std::move(nested));
  }
  return out;
}

}  // namespace

Result<Map> build(const std::filesystem::path& repo,
                  const std::vector<std::filesystem::path>& extra) {
  std::error_code error;
  const auto root = std::filesystem::canonical(repo, error);
  if (error) return std::unexpected(repo.string() + ": " + error.message());
  Map map;
  map.root = root;
  std::vector<std::filesystem::path> candidates = walk(root, 0);
  for (const auto& directory : extra) {
    for (auto& found : walk(directory, 0)) candidates.push_back(std::move(found));
  }
  std::ranges::sort(candidates);
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  for (const auto& entry : candidates) {
    const std::filesystem::path project_file = entry / kMetaDir / kMetaFile;
    if (!std::filesystem::is_regular_file(project_file)) continue;
    auto project = Project::load(entry);
    if (!project) continue;
    Port port;
    port.unit = project->unit;
    const auto relative = std::filesystem::relative(entry, root, error);
    port.dir = (error ? entry : relative).string();
    port.module = project->module;
    port.language = project->source_lang;
    for (const Function& function : project->functions) port.functions.push_back(function.name);
    if (const auto text = read_file(project->dir / "cases.jsonl"); text.has_value()) {
      for (const std::string_view line : split_lines(*text)) {
        bool empty = true;
        for (const char ch : line) {
          if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
        }
        if (!empty) ++port.cases;
      }
    }
    const std::filesystem::path history = project->dir / kMetaDir / "diff-history.jsonl";
    if (const auto text = read_file(history); text.has_value()) {
      std::optional<Json> last;
      for (const std::string_view line : split_lines(*text)) {
        bool empty = true;
        for (const char ch : line) {
          if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
        }
        if (empty) continue;
        try {
          last = parse(line);
        } catch (const JsonError&) {
          continue;
        }
      }
      if (last.has_value()) {
        const Json* passed = last->find("passed");
        const Json* total = last->find("total");
        if (passed != nullptr && passed->is_integer() && total != nullptr &&
            total->is_integer()) {
          port.last_diff = std::pair{passed->as_int(), total->as_int()};
        }
      }
    }
    port.changed_upstream = recording::stale(*project);
    map.ports.push_back(std::move(port));
  }
  std::ranges::sort(map.ports, [](const Port& a, const Port& b) {
    if (a.unit != b.unit) return a.unit < b.unit;
    return a.dir < b.dir;
  });

  if (!python::available().has_value()) {
    map.analyze_error = "python3 is not available: cannot list the units still in Python";
  } else {
    auto doc = python::analyze(root);
    if (!doc) {
      map.analyze_error = doc.error();
    } else {
      const Json* modules = doc->find("modules");
      if (modules != nullptr && modules->is_array()) {
        for (const Json& module : modules->as_array()) {
          const Json* name_field = module.find("module");
          const std::string name =
              name_field != nullptr && name_field->is_string() ? name_field->as_string() : "?";
          bool known = false;
          for (const Port& port : map.ports) {
            if (port.module == name) known = true;
          }
          if (known) continue;
          const Json* lines = module.find("lines");
          const Json* functions = module.find("functions");
          const Json* risks = module.find("risks");
          map.not_ported.push_back(Json::object({
              {"unit", Json::string(name)},
              {"lines", Json::integer(lines != nullptr && lines->is_integer() ? lines->as_int() : 0)},
              {"exports", Json::integer(functions != nullptr && functions->is_array()
                                            ? static_cast<std::int64_t>(functions->as_array().size())
                                            : 0)},
              {"risks", Json::integer(risks != nullptr && risks->is_array()
                                          ? static_cast<std::int64_t>(risks->as_array().size())
                                          : 0)},
          }));
        }
      }
    }
  }

  const std::filesystem::path glue_dir = root / "cpp" / "app" / "src";
  std::filesystem::directory_iterator glue_iterator(glue_dir, error);
  if (!error) {
    for (const auto& entry : glue_iterator) {
      const std::string name = entry.path().filename().string();
      if (!name.starts_with("glue_") || !name.ends_with(".cpp")) continue;
      const std::string text = read_file(entry.path()).value_or(std::string());
      const auto relative = std::filesystem::relative(entry.path(), root, error);
      map.glue.push_back(
          Glue{(error ? entry.path() : relative).string(),
               text.find("return false") != std::string::npos});
    }
  }
  std::ranges::sort(map.glue, [](const Glue& a, const Glue& b) { return a.file < b.file; });
  return map;
}

std::size_t Map::changed() const {
  std::size_t count = 0;
  for (const Port& port : ports) {
    if (port.changed_upstream.has_value()) ++count;
  }
  return count;
}

std::string Map::markdown() const {
  std::string out = std::format("# Port map: {}\n\n", root.filename().string());
  std::string sent;
  if (changed() > 0) {
    sent = std::format(
        "; **{} changed upstream** since they were recorded: re-record them and run `diff`.",
        changed());
  } else {
    sent = "; no original changed since it was recorded.";
  }
  out += std::format("{} unit(s) have a C++ port{}{}.\n\n", ports.size(), sent,
                     not_ported.empty()
                         ? std::string()
                         : std::format(", {} still only in Python", not_ported.size()));
  out +=
      "## Ports\n\n| Unit | Project | In C++ | Cases | Last diff | Original |\n| --- | --- | --- "
      "| ---: | --- | --- |\n";
  for (const Port& port : ports) {
    std::string functions;
    for (std::size_t index = 0; index < port.functions.size(); ++index) {
      if (index > 0) functions += ", ";
      functions += "`" + port.functions[index] + "`";
    }
    if (functions.empty()) functions = "-";
    std::string last_diff = "not run";
    if (port.last_diff.has_value()) {
      last_diff = std::format("{}/{}", port.last_diff->first, port.last_diff->second);
    }
    std::string original = "unchanged";
    if (port.changed_upstream.has_value()) {
      original = "**changed**: " + truncate_text(*port.changed_upstream, 80);
    }
    out += std::format("| `{}` | `{}` | {} | {} | {} | {} |\n", port.unit, port.dir, functions,
                       port.cases, last_diff, original);
  }
  if (!glue.empty()) {
    out += "\n## Whole app\n\n| Glue | Runs |\n| --- | --- |\n";
    for (const Glue& entry : glue) {
      out += std::format("| `{}` | {} |\n", entry.file,
                         entry.falls_back ? "C++, handing some cases to the original"
                                          : "C++ only");
    }
  }
  if (!not_ported.empty()) {
    out +=
        "\n## Not ported yet\n\n| Unit | Lines | Exports | Risks |\n| --- | ---: | ---: | ---: |\n";
    for (const Json& unit : not_ported) {
      const Json* name = unit.find("unit");
      const Json* lines = unit.find("lines");
      const Json* exports = unit.find("exports");
      const Json* risks = unit.find("risks");
      out += std::format("| `{}` | {} | {} | {} |\n",
                         name != nullptr && name->is_string() ? name->as_string() : "?",
                         lines != nullptr && lines->is_integer() ? lines->as_int() : 0,
                         exports != nullptr && exports->is_integer() ? exports->as_int() : 0,
                         risks != nullptr && risks->is_integer() ? risks->as_int() : 0);
    }
  }
  if (analyze_error.has_value()) {
    out += std::format("\nThe rest of the repository could not be analyzed: {}\n",
                       *analyze_error);
  }
  out +=
      "\nRegenerate with `cpp-anything map --write`. Commit this file: when the original "
      "changes, the next `map` says so.\n";
  return out;
}

Json Map::to_json() const {
  Json::Array ports_json;
  for (const Port& port : ports) {
    Json::Array functions;
    for (const std::string& name : port.functions) functions.push_back(Json::string(name));
    Json::Array kept;
    for (const std::string& name : port.kept) kept.push_back(Json::string(name));
    Json::Object fields;
    fields.emplace_back("unit", Json::string(port.unit));
    fields.emplace_back("project", Json::string(port.dir));
    fields.emplace_back("module", Json::string(port.module));
    fields.emplace_back("language", Json::string(port.language));
    fields.emplace_back("in_cpp", Json::array(std::move(functions)));
    fields.emplace_back("kept_in_original", Json::array(std::move(kept)));
    fields.emplace_back("cases", Json::integer(static_cast<std::int64_t>(port.cases)));
    if (port.last_diff.has_value()) {
      fields.emplace_back("last_diff", Json::array({Json::integer(port.last_diff->first),
                                                    Json::integer(port.last_diff->second)}));
    }
    if (port.changed_upstream.has_value()) {
      fields.emplace_back("changed_upstream", Json::string(*port.changed_upstream));
    }
    ports_json.push_back(Json::object(std::move(fields)));
  }
  Json::Array glue_json;
  for (const Glue& entry : glue) {
    glue_json.push_back(Json::object({
        {"file", Json::string(entry.file)},
        {"falls_back", Json::boolean(entry.falls_back)},
    }));
  }
  return Json::object({
      {"schema", Json::string("cpp-anything/port-map@1")},
      {"root", Json::string(root.string())},
      {"ports", Json::array(std::move(ports_json))},
      {"not_ported", Json::array(not_ported)},
      {"glue", Json::array(std::move(glue_json))},
      {"analyze_error", analyze_error.has_value() ? Json::string(*analyze_error)
                                                   : Json::null()},
  });
}

}  // namespace cppan::portmap
