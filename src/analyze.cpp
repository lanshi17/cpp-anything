module;

module cppan.analyze;
import cppan.json;
import cppan.types;
import cppan.util;
import cppan.inventory;
import cppan.libs;
import cppan.python;
import std;

namespace cppan::analyze {
namespace {

std::string string_or(const Json* value, std::string_view key, std::string_view fallback) {
  if (value == nullptr) return std::string(fallback);
  const Json* field = value->find(key);
  if (field == nullptr || !field->is_string()) return std::string(fallback);
  return field->as_string();
}

std::vector<std::string> string_array(const Json& item, std::string_view key) {
  std::vector<std::string> out;
  const Json* value = item.find(key);
  if (value == nullptr || !value->is_array()) return out;
  for (const Json& element : value->as_array()) {
    if (element.is_string()) out.push_back(element.as_string());
  }
  return out;
}

std::string module_name(const std::filesystem::path& file, const std::filesystem::path& base) {
  std::error_code error;
  auto relative = std::filesystem::relative(file, base, error);
  if (error) relative = file;
  relative.replace_extension();
  std::vector<std::string> parts;
  for (const auto& part : relative) {
    const std::string text = part.string();
    if (text.empty() || text == ".") continue;
    parts.push_back(text);
  }
  if (!parts.empty() && parts.back() == "__init__") parts.pop_back();
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out += '.';
    out += part;
  }
  return out;
}

struct Unit {
  std::string module;
  /// What to pass to `bridge`: the file path for plugins, the dotted module for Python.
  std::string bridge_arg;
  std::uint32_t lines = 0;
  std::vector<Json> functions;
  std::vector<std::string> risks;
  std::vector<std::string> imports;
  bool entry_point = false;
  std::string status;
  std::string note;
  std::int64_t readiness = 0;
  std::int64_t priority = 0;
  std::size_t wave = 0;
};

bool is_ui_package(std::string_view package) {
  return package == "tkinter" || package == "PyQt5" || package == "PyQt6" ||
         package == "PySide2" || package == "PySide6" || package == "kivy" || package == "pygame";
}

bool is_server_package(std::string_view package) {
  return package == "flask" || package == "fastapi" || package == "django" ||
         package == "starlette" || package == "uvicorn" || package == "aiohttp" ||
         package == "bottle";
}

std::vector<Unit> units(const Json& doc) {
  std::vector<Unit> out;
  const Json* modules = doc.find("modules");
  if (modules == nullptr || !modules->is_array()) return out;
  for (const Json& module : modules->as_array()) {
    Unit unit;
    unit.imports = string_array(module, "imports");
    unit.risks = string_array(module, "risks");
    if (const Json* functions = module.find("functions");
        functions != nullptr && functions->is_array()) {
      unit.functions = functions->as_array();
    }
    if (const Json* lines = module.find("lines"); lines != nullptr && lines->is_integer()) {
      unit.lines = static_cast<std::uint32_t>(lines->as_int());
    }

    std::int64_t none_deps = 0;
    std::int64_t unknown_deps = 0;
    bool kept = false;
    std::vector<std::string> blockers;
    for (const std::string& import : unit.imports) {
      const std::string_view package = libs::reduce(import);
      const libs::Entry* entry = libs::find(package);
      if (entry != nullptr && entry->fit == libs::Fit::None) {
        ++none_deps;
        blockers.push_back("`" + std::string(package) + "` has no C++ equivalent in the table");
      } else if (entry != nullptr && entry->libs.empty() && entry->fit != libs::Fit::Partial) {
        kept = true;
      } else if (entry == nullptr) {
        ++unknown_deps;
      }
    }
    for (const std::string& import : unit.imports) {
      if (is_ui_package(libs::reduce(import))) kept = true;
    }

    if (kept) {
      unit.status = "keep";
      unit.note = "UI code stays in Python; port what it calls";
    } else if (none_deps > 0) {
      unit.status = "blocked";
      for (std::size_t index = 0; index < blockers.size(); ++index) {
        if (index > 0) unit.note += "; ";
        unit.note += blockers[index];
      }
    } else {
      unit.status = "ready";
    }
    if (none_deps == 0 && unknown_deps > 0) {
      const std::string suffix = std::format(
          "{} dependency(ies) not in the library table: check them by hand", unknown_deps);
      unit.note = unit.note.empty() ? suffix : unit.note + "; " + suffix;
    }

    const std::int64_t risk_count = static_cast<std::int64_t>(unit.risks.size());
    // Signed arithmetic on purpose: a small file has a negative penalty and thus
    // a higher readiness, exactly as the reference implementation scores it.
    const std::int64_t size_penalty = (static_cast<std::int64_t>(unit.lines) - 600) / 10;
    std::int64_t readiness =
        100 - 6 * risk_count - size_penalty - 12 * none_deps - 4 * unknown_deps;
    readiness = std::clamp<std::int64_t>(readiness, 0, 100);
    const std::int64_t value =
        std::min<std::int64_t>(static_cast<std::int64_t>(unit.functions.size()) * 8 +
                                   static_cast<std::int64_t>(unit.lines) / 25,
                               100);
    unit.readiness = readiness;
    unit.priority = (readiness + value) / 2;

    const Json* unit_name = module.find("unit");
    if (unit_name == nullptr || !unit_name->is_string()) unit_name = module.find("module");
    unit.bridge_arg = unit_name != nullptr && unit_name->is_string() ? unit_name->as_string() : "?";
    unit.module = string_or(&module, "module", "?");
    if (const Json* entry = module.find("entry_point");
        entry != nullptr && entry->is_bool()) {
      unit.entry_point = entry->as_bool();
    }
    out.push_back(std::move(unit));
  }

  // Waves follow imports: a module that imports another local module ports after it.
  std::vector<std::pair<std::string, std::vector<std::string>>> local;
  local.reserve(out.size());
  for (const Unit& unit : out) {
    std::string package;
    if (const std::size_t dot = unit.module.rfind('.'); dot != std::string::npos) {
      package = unit.module.substr(0, dot);
    }
    std::vector<std::string> deps;
    for (const std::string& import : unit.imports) {
      const bool direct = std::ranges::any_of(
          out, [&](const Unit& other) { return other.module == import; });
      const bool in_package =
          !package.empty() &&
          std::ranges::any_of(
              out, [&](const Unit& other) { return other.module == package + "." + import; });
      if (direct || in_package) deps.push_back(import);
    }
    local.emplace_back(unit.module, std::move(deps));
  }
  std::map<std::string, std::size_t> depths;
  for (std::size_t pass = 0; pass < local.size() + 1; ++pass) {
    for (const auto& [module, deps] : local) {
      std::optional<std::size_t> deepest;
      for (const std::string& dep : deps) {
        if (const auto found = depths.find(dep); found != depths.end()) {
          const std::size_t depth = found->second + 1;
          if (!deepest.has_value() || depth > *deepest) deepest = depth;
        }
      }
      std::size_t& entry = depths[module];
      entry = std::max(entry, deepest.value_or(0));
    }
  }
  for (Unit& unit : out) {
    if (const auto found = depths.find(unit.module); found != depths.end()) {
      unit.wave = found->second;
    }
  }
  // Within a wave: the best first port first.
  std::stable_sort(out.begin(), out.end(), [](const Unit& a, const Unit& b) {
    if (a.wave != b.wave) return a.wave < b.wave;
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.module < b.module;
  });
  return out;
}

struct DependencyRow {
  std::string package;
  std::string libs;
  std::string fit;
  std::string note;
  std::vector<std::string> users;
};

std::vector<DependencyRow> dependency_rows(const Json& doc, const std::vector<Unit>& unit_list) {
  std::map<std::string, std::vector<std::string>> seen;
  for (const Unit& unit : unit_list) {
    for (const std::string& import : unit.imports) {
      const std::string package(libs::reduce(import));
      if (package.empty() || package.starts_with('.')) continue;
      seen[package].push_back(unit.module);
    }
  }
  if (const Json* declared = doc.find("dependencies");
      declared != nullptr && declared->is_array()) {
    for (const Json& item : declared->as_array()) {
      if (item.is_string()) seen.try_emplace(item.as_string());
    }
  }
  const auto is_local = [&](std::string_view package) {
    return std::ranges::any_of(unit_list, [&](const Unit& unit) {
      return unit.module == package || unit.module.starts_with(std::string(package) + ".");
    });
  };

  std::vector<DependencyRow> rows;
  for (auto& [package, users] : seen) {
    if (is_local(package)) continue;
    std::ranges::sort(users);
    users.erase(std::unique(users.begin(), users.end()), users.end());
    DependencyRow row;
    row.package = package;
    if (const libs::Entry* entry = libs::find(package); entry != nullptr) {
      row.libs = entry->libs;
      row.fit = std::string(libs::fit_label(entry->fit));
      row.note = entry->note;
    } else {
      row.fit = "unknown";
    }
    row.users = users;
    rows.push_back(std::move(row));
  }
  std::stable_sort(rows.begin(), rows.end(), [](const DependencyRow& a, const DependencyRow& b) {
    const bool left_unknown = a.fit == "unknown";
    const bool right_unknown = b.fit == "unknown";
    if (left_unknown != right_unknown) return !left_unknown;
    return a.package < b.package;
  });
  return rows;
}

std::string_view app_shape(const std::vector<Unit>& unit_list) {
  const bool imports_server = std::ranges::any_of(unit_list, [](const Unit& unit) {
    return std::ranges::any_of(unit.imports, [](const std::string& import) {
      return is_server_package(libs::reduce(import));
    });
  });
  if (imports_server) return "server";
  if (std::ranges::any_of(unit_list, [](const Unit& unit) { return unit.entry_point; })) {
    return "CLI";
  }
  return "library";
}

std::string joined_modules(const std::vector<const Unit*>& units) {
  std::string out;
  for (std::size_t index = 0; index < units.size(); ++index) {
    if (index > 0) out += ", ";
    out += "`" + units[index]->module + "`";
  }
  return out;
}

}  // namespace

Result<Resolved> resolve(const std::filesystem::path& repo_path, std::string_view unit,
                         const std::optional<std::filesystem::path>& module_root) {
  std::error_code error;
  const std::filesystem::path repo = std::filesystem::canonical(repo_path, error);
  if (error) return std::unexpected(repo_path.string() + ": " + error.message());

  std::filesystem::path base = repo;
  if (module_root.has_value()) {
    const std::filesystem::path root =
        module_root->is_absolute() ? *module_root : repo / *module_root;
    base = std::filesystem::canonical(root, error);
    if (error) return std::unexpected(root.string() + ": " + error.message());
  }

  const std::filesystem::path direct =
      std::filesystem::path(unit).is_absolute() ? std::filesystem::path(unit) : repo / unit;
  std::filesystem::path file;
  std::string module;
  if (std::filesystem::is_regular_file(direct, error)) {
    file = std::filesystem::canonical(direct, error);
    if (error) return std::unexpected(direct.string() + ": " + error.message());
    module = module_name(file, base);
  } else {
    std::string joined;
    for (const char ch : unit) {
      if (ch == '.') {
        joined += '/';
      } else {
        joined += ch;
      }
    }
    const std::filesystem::path module_file = (base / joined).replace_extension(".py");
    const std::filesystem::path package_file = base / joined / "__init__.py";
    if (std::filesystem::is_regular_file(module_file, error)) {
      file = module_file;
      module = std::string(unit);
    } else if (std::filesystem::is_regular_file(package_file, error)) {
      file = package_file;
      module = std::string(unit);
    } else {
      return std::unexpected(std::format(
          "cannot find unit \"{}\": looked for {} and {}\n  pass the path to a .py file, a dotted "
          "module, or --module-root if the package root is not the repository root",
          unit, module_file.string(), package_file.string()));
    }
  }
  return Resolved{file, module, base};
}

Result<Json> analyze(const std::filesystem::path& repo) {
  if (!python::available().has_value()) {
    return std::unexpected(std::format(
        "{} is not on PATH: analyze reads Python with the project's own interpreter (set "
        "CPPAN_PYTHON to choose one)",
        python::interpreter()));
  }
  return python::analyze(repo);
}

std::string render_plan(const Json& doc, const std::filesystem::path& repo, bool whole_app) {
  const std::vector<Unit> unit_list = units(doc);
  const std::vector<DependencyRow> dependencies = dependency_rows(doc, unit_list);
  std::string out = std::format("# Port plan for {}\n\n", repo.string());

  std::size_t total_functions = 0;
  for (const Unit& unit : unit_list) total_functions += unit.functions.size();
  const std::string language = string_or(&doc, "language", "source");
  const bool whole_app_only = language == "php" || language == "csharp";

  if (whole_app_only) {
    out += std::format(
        "{} {} unit(s), {} function(s), {} dependency(ies). {} is whole-app only in this tool: "
        "the loop records and fronts the app (`app record`, `app front-door`, `app lift`), and "
        "there is no per-unit bridge or recorder.\n\n",
        unit_list.size(), language, total_functions, dependencies.size(), language);
  } else {
    out += std::format(
        "{} {} unit(s), {} function(s), {} dependency(ies). Port one unit at a time; `bridge`, "
        "`record`, `diff` and `gate` are per unit.\n\n",
        unit_list.size(), language, total_functions, dependencies.size());
  }

  if (whole_app) {
    out += "## What the app is\n\n";
    out += std::format("- shape: **{}**\n", app_shape(unit_list));
    std::vector<const Unit*> entry;
    for (const Unit& unit : unit_list) {
      if (unit.entry_point) entry.push_back(&unit);
    }
    if (!entry.empty()) out += "- entry points: " + joined_modules(entry) + "\n";
    std::vector<const Unit*> servers;
    for (const Unit& unit : unit_list) {
      if (std::ranges::any_of(unit.imports, [](const std::string& import) {
            return is_server_package(libs::reduce(import));
          })) {
        servers.push_back(&unit);
      }
    }
    if (!servers.empty()) out += "- servers: " + joined_modules(servers) + "\n";
    const std::size_t kept = static_cast<std::size_t>(std::ranges::count_if(
        unit_list, [](const Unit& unit) { return unit.status == "keep"; }));
    const std::size_t blocked = static_cast<std::size_t>(std::ranges::count_if(
        unit_list, [](const Unit& unit) { return unit.status == "blocked"; }));
    const std::size_t ready = static_cast<std::size_t>(std::ranges::count_if(
        unit_list, [](const Unit& unit) { return unit.status == "ready"; }));
    out += std::format("- {} ready, {} kept in Python, {} blocked\n", ready, kept, blocked);
    out +=
        "\nFor a whole app, `app record`, `app replay` and `app front-door` come first: the front "
        "door ships with every command answered by the original until a unit is wired.\n";
  }

  out += "\n## Units in port order\n\n";
  out +=
      "| # | wave | unit | lines | functions | readiness | priority | status |\n| ---: | ---: "
      "| --- | ---: | ---: | ---: | ---: | --- |\n";
  for (std::size_t index = 0; index < unit_list.size(); ++index) {
    const Unit& unit = unit_list[index];
    out += std::format("| {} | {} | `{}` | {} | {} | {} | {} | {} |\n", index + 1, unit.wave,
                       unit.module, unit.lines, unit.functions.size(), unit.readiness,
                       unit.priority, unit.status);
  }

  const auto first = std::ranges::find_if(
      unit_list, [](const Unit& unit) { return unit.status == "ready"; });
  if (first != unit_list.end()) {
    std::string lang_flag;
    const std::string language_name = string_or(&doc, "language", "");
    for (const std::string_view native : {"c", "cpp", "ts", "js", "go", "ruby", "java"}) {
      if (language_name == native) lang_flag = " --lang " + std::string(native);
    }
    if (whole_app_only) {
      out +=
          "\nStart here: `cpp-anything app record <repo> -- <command>` then `cpp-anything app "
          "front-door <repo>`: the app stays whole and the C++ front door takes it over command "
          "by command.\n";
    } else {
      out += std::format(
          "\nStart here: `cpp-anything bridge {} {}{}` then `cpp-anything record <project>`.\n",
          repo.string(), first->bridge_arg, lang_flag);
    }
    if (first->entry_point) {
      out +=
          "\nThis unit is an entry point: port it together with what it calls, and wire the "
          "command into `cpp/app` when it passes gate.\n";
    }
  } else if (std::ranges::all_of(unit_list,
                                 [](const Unit& unit) { return unit.status == "keep"; })) {
    out += "\nEvery unit is UI code that stays in Python: nothing to port here.\n";
  } else {
    out += "\nNothing is ready: every unit is blocked. See the dependencies table.\n";
  }

  if (whole_app) {
    out +=
        "\n## Dependencies\n\n| package | C++ library | fit | used by |\n| --- | --- | --- | --- "
        "|\n";
    for (const DependencyRow& row : dependencies) {
      std::string users = "declared only";
      if (!row.users.empty()) {
        users.clear();
        for (std::size_t index = 0; index < row.users.size() && index < 3; ++index) {
          if (index > 0) users += ", ";
          users += row.users[index];
        }
      }
      out += std::format("| `{}` | {} | {} | {} |\n", row.package,
                         row.libs.empty() ? "—" : "`" + row.libs + "`", row.fit, users);
    }
    std::vector<const DependencyRow*> notes;
    for (const DependencyRow& row : dependencies) {
      if (!row.note.empty()) notes.push_back(&row);
    }
    if (!notes.empty()) {
      out += "\nNotes on the mappings that are not drop-in:\n\n";
      for (const DependencyRow* row : notes) {
        out += std::format("- `{}` ({}): {}\n", row->package, row->fit, row->note);
      }
    }
  }

  out += "\n## Risks per module\n\n";
  for (const Unit& unit : unit_list) {
    if (unit.risks.empty() && unit.note.empty()) continue;
    out += std::format("### `{}`\n\n", unit.module);
    if (!unit.note.empty()) out += std::format("- status: {}\n", unit.note);
    for (const std::string& risk : unit.risks) out += std::format("- {}\n", risk);
    out += '\n';
  }

  out += "\n## Decisions to confirm before porting\n\n";
  out +=
      "1. **Target toolchain and standard**: `cpp-anything matrix features --toolchain gcc "
      "--version 13 --std c++23` says what it can express; the default is GCC 13 with C++23.\n";
  out +=
      "2. **Which unit first**: the order above is by dependency wave, then readiness, then "
      "value. Porting a lower-priority unit first is fine if the user says so.\n";
  if (whole_app) {
    out +=
        "3. **The app's shape**: a CLI or server keeps its entry point in `cpp/app`; a library "
        "becomes the front door's public API. Confirm which one this is.\n";
    out +=
        "4. **Dependencies marked `none`**: their units are blocked. Decide per package: keep "
        "that unit in Python, hand-roll the missing piece, or change the app.\n";
    out +=
        "5. **Dependencies marked `unknown`**: not in `data/libs.json`, so nobody has judged the "
        "fit. Check each before porting a unit that uses it.\n";
  } else {
    out += "3. **Badged units**: blocked and keep units are decisions for the user, not the agent.\n";
  }

  out += "\n## What the numbers mean\n\n";
  out +=
      "- **readiness** = 100 - 6 x risks - size over 600 lines - 12 x each `none` dependency - 4 "
      "x each unknown dependency, clamped to 0..100.\n";
  out +=
      "- **priority** = the mean of readiness and value, where value = 8 x functions + lines/25 "
      "(capped at 100).\n";
  out +=
      "- **wave** = 1 + the deepest wave among the local modules this one imports: port "
      "dependencies first.\n";
  out +=
      "- **risks** are heuristics from the analyzer: unicode offsets, int parsing, regex "
      "engines, float formatting, integer width, dynamic dispatch. Each one is a place the port "
      "can differ while still compiling.\n";
  out +=
      "- **status**: `ready` ports now; `keep` is UI code that stays in Python; `blocked` has a "
      "dependency with no C++ equivalent.\n";
  return out;
}

Json to_json(const Json& doc, const std::filesystem::path& repo, bool whole_app) {
  Json::Object fields;
  fields.emplace_back("schema", Json::string("cpp-anything/analyze@1"));
  fields.emplace_back("root", Json::string(repo.string()));
  const Json* modules = doc.find("modules");
  fields.emplace_back("modules", modules != nullptr ? *modules : Json::array());
  const Json* dependencies = doc.find("dependencies");
  fields.emplace_back("dependencies", dependencies != nullptr ? *dependencies : Json::array());
  if (whole_app) fields.emplace_back("whole_app", Json::boolean(true));
  return Json::object(std::move(fields));
}

std::string summarize(const Json& doc) {
  const Json* modules = doc.find("modules");
  if (modules == nullptr || !modules->is_array()) return "0 modules, 0 functions";
  std::size_t functions = 0;
  for (const Json& module : modules->as_array()) {
    const Json* list = module.find("functions");
    if (list != nullptr && list->is_array()) functions += list->as_array().size();
  }
  return std::format("{} modules, {} functions", modules->as_array().size(), functions);
}

Result<void> write_plan(const std::filesystem::path& path, std::string_view text) {
  auto written = write_file(path, text);
  if (!written) return std::unexpected(path.string() + ": " + written.error());
  return {};
}

}  // namespace cppan::analyze
