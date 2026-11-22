module;
#include <cctype>
#include <cstdio>

module cppan.cli;
import cppan.args;
import cppan.types;
import cppan.util;
import cppan.inventory;
import cppan.analyze;
import cppan.app;
import cppan.bench;
import cppan.build;
import cppan.codegen;
import cppan.diff;
import cppan.docs;
import cppan.doctor;
import cppan.gate;
import cppan.json;
import cppan.langc;
import cppan.langs;
import cppan.matrix;
import cppan.mcp;
import cppan.modules;
import cppan.project;
import cppan.portmap;
import cppan.profile;
import cppan.python;
import cppan.rsi;
import cppan.setup;
import cppan.shadow;
import std;

namespace cppan::cli {
namespace {

std::string replace_all_copy(std::string text, std::string_view needle, std::string_view with) {
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), with);
    at += with.size();
  }
  return text;
}

std::string debug_quote(std::string_view text) {
  std::string out = "\"";
  for (const char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += ch; break;
    }
  }
  out += '"';
  return out;
}

Json feature_json(const Feature& feature) {
  return Json::object({
      {"id", Json::string(feature.id)},
      {"name", Json::string(feature.name)},
      {"kind", Json::string(std::string(kind_label(feature.kind)))},
      {"std", Json::string(feature.standard)},
      {"macro", Json::string(feature.macro_id)},
      {"paper", Json::string(feature.paper)},
  });
}

std::string latest_release_of(const Matrix& matrix, std::string_view toolchain) {
  std::optional<std::string> latest;
  for (const Feature& feature : matrix.features) {
    const auto release = feature.first_release(toolchain);
    if (!release.has_value()) continue;
    if (!latest.has_value() || version_cmp(*release, *latest) >= 0) latest = release;
  }
  return latest.value_or("15");
}

Result<Target> target_from(const Parsed& args, const Matrix& matrix) {
  const std::string requested = args.value("toolchain").value_or("gcc");
  const auto toolchain = matrix.resolve_toolchain(requested);
  if (!toolchain.has_value()) {
    return std::unexpected("unknown toolchain " + debug_quote(requested));
  }
  return Target{*toolchain, args.value("version").value_or("13"),
                args.value("std").value_or("c++23")};
}

Result<int> matrix_features(const Parsed& args) {
  const Matrix& matrix = Matrix::get();
  const std::string requested = args.value("toolchain").value_or("gcc");
  const auto resolved = matrix.resolve_toolchain(requested);
  if (!resolved.has_value()) {
    return std::unexpected("unknown toolchain " + debug_quote(requested));
  }
  const std::string toolchain = *resolved;
  const std::string version =
      args.value("version").value_or(latest_release_of(matrix, toolchain));
  const std::string standard = args.value("std").value_or("c++23");
  const std::string kind = args.value("kind").value_or("all");

  const Audit audit = matrix.audit(toolchain, version, standard);
  const auto keep = [&](std::string_view candidate) {
    return kind == "all" || candidate == kind;
  };
  const auto features_json = [&](const std::vector<const Feature*>& list) {
    Json::Array items;
    for (const Feature* feature : list) {
      if (keep(kind_label(feature->kind))) items.push_back(feature_json(*feature));
    }
    return Json::array(std::move(items));
  };

  if (args.flag("json")) {
    Json doc = Json::object({
        {"schema", Json::string("cpp-anything/matrix-features@1")},
        {"toolchain", Json::string(toolchain)},
        {"toolchain_label", Json::string(matrix.toolchain_label(toolchain))},
        {"version", Json::string(version)},
        {"std", Json::string(standard)},
        {"supported", features_json(audit.supported)},
        {"partial", features_json(audit.partial)},
        {"missing", features_json(audit.missing)},
    });
    std::print("{}\n", dump(doc, true));
    return 0;
  }

  const std::size_t total = audit.supported.size() + audit.partial.size() + audit.missing.size();
  const std::size_t core = static_cast<std::size_t>(std::ranges::count_if(
      audit.supported, [](const Feature* feature) { return feature->kind == Kind::Core; }));
  const std::size_t library = audit.supported.size() - core;
  std::print("{} {} · {} — {}/{} features supported ({} core, {} library), {} partial, {} missing\n",
             matrix.toolchain_label(toolchain), version, standard, audit.supported.size(), total,
             core, library, audit.partial.size(), audit.missing.size());

  std::vector<const Feature*> rows;
  for (const Feature* feature : audit.supported) {
    if (keep(kind_label(feature->kind))) rows.push_back(feature);
  }
  std::ranges::sort(rows, [](const Feature* a, const Feature* b) {
    if (kind_label(a->kind) != kind_label(b->kind)) return kind_label(a->kind) < kind_label(b->kind);
    return a->name < b->name;
  });
  std::print("\nsupported ({}):\n", rows.size());
  for (const Feature* feature : rows) {
    const std::string name = truncate_text(feature->name, 60);
    std::string macro_name;
    for (const char ch : feature->macro_id) {
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
      macro_name += ch;
    }
    std::print("  {} {}\n", pad_right(name, 60), macro_name);
  }

  std::vector<const Feature*> missing;
  for (const Feature* feature : audit.missing) {
    if (keep(kind_label(feature->kind))) missing.push_back(feature);
  }
  std::ranges::sort(missing, [](const Feature* a, const Feature* b) {
    if (kind_label(a->kind) != kind_label(b->kind)) return kind_label(a->kind) < kind_label(b->kind);
    return a->name < b->name;
  });
  if (!missing.empty()) {
    std::print("\nnot in this release ({}):\n", missing.size());
    for (std::size_t index = 0; index < missing.size() && index < 25; ++index) {
      const Feature* feature = missing[index];
      std::print("  {} {}\n", pad_right(truncate_text(feature->name, 60), 60),
                 kind_label(feature->kind));
    }
    if (missing.size() > 25) {
      std::print("  … {} more (--json lists all)\n", missing.size() - 25);
    }
  }
  return 0;
}

Result<int> matrix_explain(const Parsed& args) {
  const Matrix& matrix = Matrix::get();
  if (args.positional.size() < 2) {
    return std::unexpected(
        "matrix explain needs a feature name or macro, e.g. `matrix explain std::expected`");
  }
  const std::string& query = args.positional[1];
  const std::vector<const Feature*> found = matrix.lookup(query);
  if (found.empty()) {
    return std::unexpected("nothing in the matrix matches " + debug_quote(query));
  }

  if (args.flag("json")) {
    Json::Array items;
    for (const Feature* feature : found) {
      Json::Object support;
      for (const auto& [toolchain, entry] : feature->support) {
        Json::Object fields;
        fields.emplace_back("status", Json::string(std::string(status_label(entry.status))));
        fields.emplace_back("min", entry.min.has_value() ? Json::string(*entry.min) : Json::null());
        Json::Array notes;
        for (const std::string& note : entry.notes) notes.push_back(Json::string(note));
        fields.emplace_back("notes", Json::array(std::move(notes)));
        support.emplace_back(toolchain, Json::object(std::move(fields)));
      }
      Json::Object fields = feature_json(*feature).as_object();
      fields.emplace_back("support", Json::object(std::move(support)));
      items.push_back(Json::object(std::move(fields)));
    }
    std::print("{}\n", dump(Json::array(std::move(items)), true));
    return 0;
  }

  std::size_t shown = 0;
  for (const Feature* feature : found) {
    if (shown++ >= 6) break;
    std::print("{} — {} {}, {}\n", feature->name, kind_label(feature->kind), feature->standard,
               feature->paper.empty() ? std::string() : std::format("({})", feature->paper));
    if (!feature->macro_id.empty()) std::print("  macro   {}\n", feature->macro_id);
    for (const auto& [toolchain, entry] : feature->support) {
      if (toolchain != "gcc" && toolchain != "clang" && toolchain != "msvc" &&
          toolchain != "aclang") {
        continue;
      }
      std::string value;
      if (entry.status == Status::Yes) {
        value = entry.min.value_or("yes");
      } else if (entry.status == Status::Partial) {
        value = entry.min.has_value() ? *entry.min + " (partial)" : "? (partial)";
      } else {
        value = "—";
      }
      std::string line = "  " + pad_right(toolchain, 7) + " " + value;
      if (toolchain == "msvc") {
        if (entry.min.has_value()) {
          if (const auto title = matrix.msvc_titles.find(*entry.min);
              title != matrix.msvc_titles.end()) {
            line += "  (" + title->second + ")";
          }
        }
      }
      std::print("{}\n", line);
      for (const std::string& note : entry.notes) std::print("          note: {}\n", note);
    }
    std::print("\n");
  }
  if (found.size() > 6) {
    std::print("… {} more matches; narrow the query\n", found.size() - 6);
  }
  return 0;
}

Result<int> matrix_profile(const Parsed& args) {
  const Matrix& matrix = Matrix::get();
  auto target = target_from(args, matrix);
  if (!target) return std::unexpected(target.error());
  const std::string profile =
      render_profile(target->toolchain, target->version, target->standard);
  if (const auto path = args.value("out"); path.has_value()) {
    if (auto written = write_file(*path, profile); !written) return std::unexpected(written.error());
    std::print("wrote {}\n", *path);
  } else {
    std::print("{}", profile);
  }
  return 0;
}

Result<int> matrix_docs(const Parsed& args) {
  const std::filesystem::path root = args.value("out").value_or("docs/guides");
  if (args.flag("check")) {
    auto drifted = docs::check(root);
    if (!drifted) return std::unexpected(drifted.error());
    if (drifted->empty()) {
      std::print("{}: up to date\n", root.string());
      return 0;
    }
    std::print("{}: {} file(s) drifted:\n", root.string(), drifted->size());
    for (const std::string& path : *drifted) std::print("  {}\n", path);
    std::print("\nrun `cpp-anything matrix docs` to regenerate them\n");
    return 1;
  }
  auto count = docs::write(root);
  if (!count) return std::unexpected(count.error());
  std::print("wrote {} guides under {}\n", *count, root.string());
  return 0;
}

Result<int> matrix_check() {
  const Matrix& matrix = Matrix::get();
  auto drifted = docs::check("docs/guides");
  if (!drifted) return std::unexpected(drifted.error());
  if (drifted->empty()) {
    std::print("guides match data/matrix.json ({} features)\n", matrix.features.size());
    return 0;
  }
  std::print("{} guide(s) drifted: [", drifted->size());
  for (std::size_t index = 0; index < drifted->size(); ++index) {
    if (index > 0) std::print(", ");
    std::print("\"{}\"", (*drifted)[index]);
  }
  std::print("]\n");
  return 1;
}

Result<std::vector<Batch>> parse_batches(const Project& project,
                                        const std::vector<std::string>& values) {
  std::vector<Batch> batches;
  for (const std::string& value : values) {
    const std::size_t colon = value.find(':');
    if (colon == std::string::npos) {
      return std::unexpected("--batch \"" + value +
                             "\" needs `function:param`, e.g. `--batch slug:text`");
    }
    const std::string function = value.substr(0, colon);
    const std::string param = value.substr(colon + 1);
    const auto spec = std::ranges::find_if(project.functions, [&](const Function& candidate) {
      return candidate.name == function;
    });
    if (spec == project.functions.end()) {
      std::string names;
      for (const Function& candidate : project.functions) {
        if (!names.empty()) names += ", ";
        names += candidate.name;
      }
      return std::unexpected("--batch \"" + value + "\": " + project.module +
                             " has no function \"" + function + "\"; it has " + names);
    }
    const bool has_param = std::ranges::any_of(
        spec->params, [&](const Param& candidate) { return candidate.name == param; });
    if (!has_param) {
      std::string names;
      for (const Param& candidate : spec->params) {
        if (!names.empty()) names += ", ";
        names += candidate.name;
      }
      return std::unexpected("--batch \"" + value + "\": " + function +
                             " has no parameter \"" + param + "\"; it has " + names);
    }
    const bool seen = std::ranges::any_of(batches, [&](const Batch& batch) {
      return batch.function == function && batch.param == param;
    });
    if (!seen) batches.push_back(Batch{function, param});
  }
  return batches;
}

void apply_kept(Unit& unit, const std::vector<std::string>& specs, langs::Lang lang) {
  for (const std::string& spec : specs) {
    const std::size_t colon = spec.find(':');
    std::string name;
    std::string reason;
    if (colon == std::string::npos) {
      name = spec;
    } else {
      name = spec.substr(0, colon);
      reason = spec.substr(colon + 1);
    }
    const auto trim = [](std::string& text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
      }
    };
    trim(name);
    trim(reason);
    const std::size_t before = unit.functions.size();
    std::erase_if(unit.functions, [&](const Function& function) { return function.name == name; });
    if (unit.functions.size() == before) continue;
    std::string note = "kept in " + std::string(langs::label(lang));
    if (!reason.empty()) note += ": " + reason;
    unit.skipped.emplace_back(name, note);
  }
}

bool mentions_type(std::string_view annotation, std::string_view name) {
  std::string token;
  for (const char ch : annotation) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
      token += ch;
    } else {
      if (token == name) return true;
      token.clear();
    }
  }
  return token == name;
}

void apply_as_json(Unit& unit, const std::vector<std::string>& specs) {
  for (const std::string& spec : specs) {
    std::string name = spec;
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
      name.erase(name.begin());
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
      name.pop_back();
    }
    if (name.empty()) continue;
    for (Function& function : unit.functions) {
      for (Param& param : function.params) {
        if (mentions_type(param.annotation, name)) {
          param.annotation = "Any";
          param.ty = Ty{};
        }
      }
      if (mentions_type(function.returns_annotation, name)) {
        function.returns_annotation = "Any";
        function.returns = Ty{};
      }
    }
  }
}

Result<Project> project_arg(const Parsed& args) {
  if (args.positional.empty()) return std::unexpected("this command needs a project directory");
  return Project::load(args.positional[0]);
}

Result<int> bridge_command(const Parsed& args) {
  if (args.positional.empty()) {
    return std::unexpected("bridge needs a repository path: `bridge <repo> <unit>`");
  }
  if (args.positional.size() < 2) {
    return std::unexpected(
        "bridge needs a unit: a .py file or a dotted module (`bridge . textkit.words`)");
  }
  const std::filesystem::path repo = args.positional[0];
  const std::string& unit_name = args.positional[1];
  const Matrix& matrix = Matrix::get();
  auto target = target_from(args, matrix);
  if (!target) return std::unexpected(target.error());

  std::optional<langc::Lang> native;
  if (const auto lang = args.value("lang"); lang.has_value()) {
    native = langc::parse_lang(*lang);
  }
  std::optional<langs::Lang> plugin;
  if (const auto lang = args.value("lang"); lang.has_value()) {
    plugin = langs::parse(*lang);
  } else {
    plugin = langs::infer(repo, unit_name);
  }
  if (native.has_value() && plugin.has_value()) return std::unexpected("ambiguous language");
  if (plugin.has_value() && langs::whole_app_only(*plugin)) {
    return std::unexpected(std::format(
        "{} is whole-app only: `analyze --lang {}` plans the app, and `app record` / `app "
        "front-door` take over its CLI or server; there is no per-unit bridge because no "
        "fixture-verified recorder exists for it",
        langs::label(*plugin), langs::id(*plugin)));
  }

  analyze::Resolved resolved;
  if (native.has_value() || plugin.has_value()) {
    resolved.file = std::filesystem::path(unit_name).is_absolute()
                        ? std::filesystem::path(unit_name)
                        : repo / unit_name;
    resolved.module = unit_name;
    resolved.package_root = repo;
  } else {
    auto result = analyze::resolve(repo, unit_name,
                                   args.value("module-root").has_value()
                                       ? std::optional<std::filesystem::path>(
                                             *args.value("module-root"))
                                       : std::nullopt);
    if (!result) return std::unexpected(result.error());
    resolved = std::move(*result);
  }

  // C++20 or C++23, and a toolchain the matrix knows about. `std::expected` is
  // preferred, not required: C++20 targets get cppan::Result, same interface.
  std::uint32_t std_number = 0;
  for (std::size_t index = target->standard.starts_with("c++") ? 3 : 0;
       index < target->standard.size(); ++index) {
    const char ch = target->standard[index];
    if (ch < '0' || ch > '9') break;
    std_number = std_number * 10 + static_cast<std::uint32_t>(ch - '0');
  }
  if (std_number < 20) {
    return std::unexpected(std::format(
        "cpp-anything targets C++20 and C++23, not {}: pass `--std c++20` or `--std c++23`",
        target->standard));
  }
  if (!matrix.resolve_toolchain(target->toolchain).has_value()) {
    return std::unexpected(
        "unknown toolchain " + debug_quote(target->toolchain) +
        ": `cpp-anything matrix features --toolchain …` lists the ones the matrix knows (gcc, "
        "clang, msvc, apple-clang, …)");
  }
  const MacroStatus expected = matrix.macro_available("__cpp_lib_expected", target->toolchain,
                                                      target->version, target->standard);
  if (!expected.available()) {
    std::print("note: std::expected is not available on {} {} with {}; the generated code uses "
               "cppan::Result (same interface) instead\n",
               target->toolchain, target->version, target->standard);
  }

  Result<Unit> unit_doc = std::unexpected("no unit");
  if (native.has_value()) {
    unit_doc = langc::unit(repo, unit_name, *native);
  } else if (plugin.has_value()) {
    unit_doc = langs::inventory(repo, unit_name, *plugin);
  } else {
    unit_doc = python::inventory(resolved.file, resolved.module, resolved.package_root);
  }
  if (!unit_doc) return std::unexpected(unit_doc.error());
  if (unit_doc->functions.empty()) {
    return std::unexpected(std::format(
        "{} has no ported-able functions: the analyzer needs plain module-level functions with a "
        "name not starting with '_' (decorated, async and *args functions are reported as "
        "skipped)",
        resolved.file.string()));
  }

  const std::string default_dir =
      (repo / "cpp" / replace_all_copy(sanitize_identifier(unit_doc->unit), "_", "-")).string();
  const std::filesystem::path dir(args.value("out").value_or(default_dir));
  auto created = prepare_dir(dir, args.flag("force"));
  if (!created) return std::unexpected(created.error());
  std::error_code error;
  const auto dir_absolute = std::filesystem::canonical(dir, error);
  if (error) return std::unexpected(dir.string() + ": " + error.message());
  Project project = Project::from_unit(*unit_doc, *target, dir_absolute);

  const std::string module_mode = args.value("modules").value_or("auto");
  if (module_mode != "auto" && module_mode != "on" && module_mode != "off") {
    return std::unexpected("--modules \"" + module_mode + "\": use auto, on or off");
  }
  auto modules = modules::resolve(module_mode, project.target, matrix);
  if (!modules) return std::unexpected(modules.error());
  project.modules = *modules;
  if (native.has_value()) project.source_lang = std::string(langc::id(*native));
  if (!plugin.has_value()) {
    const auto check_flag = [&](std::string_view name, const std::string& value) -> Result<void> {
      if (!value.empty()) {
        return std::unexpected(std::format(
            "--{} controls a TypeScript/JavaScript (or plugin-language) binding; this unit is "
            "read as Python",
            name));
      }
      return {};
    };
    std::string keep;
    for (const std::string& value : args.many("keep")) keep += (keep.empty() ? "" : " ") + value;
    std::string as_json;
    for (const std::string& value : args.many("as-json")) {
      as_json += (as_json.empty() ? "" : " ") + value;
    }
    std::string node_args;
    for (const std::string& value : args.many("node-arg")) {
      node_args += (node_args.empty() ? "" : " ") + value;
    }
    for (const auto& [name, value] :
         {std::pair<std::string, std::string>{"keep", keep},
          {"as-json", as_json},
          {"kind", args.value("kind").value_or("")},
          {"node-arg", node_args},
          {"resolve-from", args.value("resolve-from").value_or("")},
          {"module", args.value("module").value_or("")}}) {
      if (auto checked = check_flag(name, value); !checked) return std::unexpected(checked.error());
    }
  }
  if (plugin.has_value()) {
    if (!langs::is_ts_family(*plugin)) {
      for (const auto& [name, value] :
           {std::pair<std::string, std::string>{"node-arg", [&] {
              std::string joined;
              for (const std::string& arg : args.many("node-arg")) {
                joined += (joined.empty() ? "" : " ") + arg;
              }
              return joined;
            }()},
            {"resolve-from", args.value("resolve-from").value_or("")},
            {"module", args.value("module").value_or("")}}) {
        if (!value.empty()) {
          return std::unexpected(std::format(
              "--{} drives Node when the original is TypeScript/JavaScript; {} is recorded with "
              "its own runtime",
              name, langs::label(*plugin)));
        }
      }
    }
    project.source_lang = std::string(langs::id(*plugin));
    apply_kept(*unit_doc, args.many("keep"), *plugin);
    apply_as_json(*unit_doc, args.many("as-json"));
    project.functions = unit_doc->functions;
    project.skipped = unit_doc->skipped;
    project.node_args = args.many("node-arg");
    if (auto value = args.value("resolve-from"); value.has_value()) {
      project.resolve_from = *value;
    }
    if (auto value = args.value("module"); value.has_value()) project.source_module = *value;
    if (const auto kind = args.value("kind"); kind.has_value()) {
      if (*kind == "napi" && langs::is_ts_family(*plugin)) {
        // Node-API is the only binding kind that exists.
      } else if (*kind == "wasm") {
        return std::unexpected(
            "--kind wasm builds a WebAssembly module for a Rust port; the C++ target is a native "
            "library, so only `--kind napi` exists");
      } else {
        return std::unexpected("--kind " + debug_quote(*kind) +
                               ": the bindings are Node-API (`--kind napi`); WASM has no C++ "
                               "target here");
      }
    }
  }
  auto batches = parse_batches(project, args.many("batch"));
  if (!batches) return std::unexpected(batches.error());
  project.batches = std::move(*batches);

  auto written = codegen::write_project(project, args.flag("force"));
  if (!written) return std::unexpected(written.error());
  std::print("cpp-anything bridge: {} → {}\n  {} files written ({}){}\n", resolved.module,
             dir_absolute.string(), written->size(), project.functions.size(),
             *created ? ", existing directory reused" : "");
  for (const Function& function : project.functions) {
    std::print("  · {}\n", function.signature_line());
  }
  if (project.modules) {
    std::print("  · C++20 modules: src/port.cppm (`import {};`); building it needs Ninja\n",
               codegen::namespace_of(project));
  } else if (module_mode == "auto") {
    std::print("  · headers: `--modules on` would generate a C++20 module unit when the "
               "toolchain supports it\n");
  }
  if (!project.skipped.empty()) {
    std::print("\n  skipped:\n");
    for (const auto& [name, reason] : project.skipped) {
      std::print("    · {} ({})\n", name, reason);
    }
  }
  std::print("\nnext:\n  cpp-anything record {}\n  # write src/port.cpp\n  cpp-anything build "
             "{}\n  cpp-anything diff {}\n  cpp-anything gate {}\n",
             dir_absolute.string(), dir_absolute.string(), dir_absolute.string(),
             dir_absolute.string());
  return 0;
}

std::size_t default_jobs() {
  const unsigned hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 4 : hardware;
}

std::string library_name() {
#if defined(_WIN32)
  return "cppan_shadow.dll";
#elif defined(__APPLE__)
  return "libcppan_shadow.dylib";
#else
  return "libcppan_shadow.so";
#endif
}

Result<int> shadow_command(const Parsed& args) {
  auto project = project_arg(args);
  if (!project) return std::unexpected(project.error());
  if (args.flag("report") || args.flag("check")) {
    auto report = shadow::read_report(*project);
    if (!report) return std::unexpected(report.error());
    if (args.flag("json")) {
      std::print("{}\n", dump(shadow::to_json(*report, *project), true));
    } else {
      std::print("{}", shadow::render(*report, *project));
    }
    return args.flag("check") && !report->clean() ? 1 : 0;
  }
  auto refreshed = codegen::refresh(*project);
  if (!refreshed) return std::unexpected(refreshed.error());
  std::optional<std::string> cxx;
  if (const auto value = args.value("cxx"); value.has_value()) cxx = *value;
  auto outcome = build::build_project(*project, false, cxx,
                                      args.number("jobs").value_or(default_jobs()));
  if (!outcome) return std::unexpected(outcome.error());
  std::print("cpp-anything shadow: {} shadowed by {}\n", project->module, project->unit);
  std::print("  library: {}\n",
             short_path(outcome->driver.parent_path() / library_name()));
  const std::string ns = codegen::namespace_of(*project);
  std::string binding;
  if (const auto lang = langs::parse(project->source_lang); lang.has_value()) {
    switch (*lang) {
      case langs::Lang::Ruby: binding = "shadow/" + ns + "_shadow.rb"; break;
      case langs::Lang::Go: binding = "shadow/" + ns + "_shadow.go"; break;
      case langs::Lang::Rust: binding = "shadow/rust"; break;
      case langs::Lang::Java: binding = "shadow/CppanShadow.java"; break;
      case langs::Lang::Ts:
      case langs::Lang::Js: binding = "shadow/" + ns + "_shadow.mjs"; break;
      default: binding = "shadow/" + ns + "_shadow.py"; break;
    }
  } else {
    binding = "shadow/" + ns + "_shadow.py";
  }
  std::print("  module:  {}\n", short_path(project->dir / binding));
  if (const auto lang = langs::parse(project->source_lang); lang.has_value()) {
    const std::filesystem::path dir = project->dir;
    switch (*lang) {
      case langs::Lang::Ruby:
        std::print("\nrequire it where the original was required:\n  require_relative \"{}\"\n",
                   (dir / "shadow" / (ns + "_shadow")).string());
        break;
      case langs::Lang::Go:
        std::print(
            "\nimport it where the original package was imported:\n  // module: {} (cgo, links "
            "build/libcppan_shadow.so)\n",
            (dir / "shadow").string());
        break;
      case langs::Lang::Rust:
        std::print(
            "\nadd it where the original crate was a dependency:\n  {} = {{ path = \"{}\" }}\n",
            ns + "_shadow", (dir / "shadow" / "rust").string());
        break;
      case langs::Lang::Java:
        std::print("\nload it where the original class was used:\n  "
                   "System.loadLibrary(\"cppan_shadow_jni\") via {}\n",
                   (dir / "shadow" / "CppanShadow.java").string());
        break;
      case langs::Lang::Ts:
      case langs::Lang::Js:
        std::print("\nimport it where the original module was imported:\n  import * as shadow "
                   "from \"{}\"\n",
                   (dir / "shadow" / (ns + "_shadow.mjs")).string());
        break;
      default:
        std::print("\nimport it where the original was imported:\n  sys.path.insert(0, \"{}\")\n "
                   " import {}_shadow as {}\n",
                   (dir / "shadow").string(), ns, ns);
        break;
    }
  } else {
    std::print("\nimport it where the original was imported:\n  sys.path.insert(0, \"{}\")\n  "
               "import {}_shadow as {}\n",
               (project->dir / "shadow").string(), ns, ns);
  }
  std::print("\ncallers keep the original's results; disagreements go to shadow/{}.shadow.jsonl\n "
             " cpp-anything shadow {} --report\n",
             ns, project->dir.string());
  return 0;
}

Result<int> map_command(const Parsed& args) {
  const std::filesystem::path repo =
      args.positional.empty() ? std::filesystem::path(".") : std::filesystem::path(args.positional[0]);
  std::vector<std::filesystem::path> extra;
  for (const std::string& value : args.many("crates")) extra.emplace_back(value);
  auto map = portmap::build(repo, extra);
  if (!map) return std::unexpected(map.error());
  if (args.flag("json")) {
    std::print("{}\n", dump(map->to_json(), true));
  } else {
    std::print("{}", map->markdown());
  }
  if (args.flag("write")) {
    const std::filesystem::path path = map->root / portmap::kMapFile;
    auto written = write_file(path, map->markdown());
    if (!written) return std::unexpected(written.error());
    std::print("Wrote {}\n", short_path(path));
  }
  if (args.flag("check") && map->changed() > 0) return 1;
  return 0;
}

Result<int> bench_command(const Parsed& args) {
  auto project = project_arg(args);
  if (!project) return std::unexpected(project.error());
  const auto driver = build::driver_path(*project);
  if (!std::filesystem::exists(driver)) {
    return std::unexpected(short_path(driver) + " is not built yet: run `cpp-anything build " +
                           project->dir.string() + "` first");
  }
  const std::size_t repeat = args.number("repeat").value_or(1000);
  const std::size_t samples =
      std::clamp<std::size_t>(args.number("samples").value_or(5), 1, 50);
  const auto emit = [&](const bench::BenchReport& report) -> Result<int> {
    if (args.flag("json")) {
      std::print("{}\n", dump(bench::to_json(report, *project), true));
    } else {
      std::print("{}", bench::render(report, *project));
    }
    return 0;
  };
  if (const auto lang = langc::parse_lang(project->source_lang); lang.has_value()) {
    const std::filesystem::path repo =
        project->package_root.value_or(project->source_file.parent_path());
    auto unit = langc::unit(repo, project->source_file.string(), *lang);
    if (!unit) return std::unexpected(unit.error());
    auto report = bench::run_samples(
        *project, driver, repeat, samples, std::string(langc::label(*lang)),
        [&](std::size_t calls) {
          auto value = langc::bench_original(*project, *unit, calls, *lang);
          if (!value) return Result<BenchMeasurement>(std::unexpected(value.error()));
          return Result<BenchMeasurement>(*value);
        });
    if (!report) return std::unexpected(report.error());
    return emit(*report);
  }
  if (const auto lang = langs::parse(project->source_lang); lang.has_value()) {
    auto report = bench::run_samples(
        *project, driver, repeat, samples, std::string(langs::label(*lang)),
        [&](std::size_t calls) {
          auto value = langs::bench_original(*project, calls);
          if (!value) return Result<BenchMeasurement>(std::unexpected(value.error()));
          return Result<BenchMeasurement>(*value);
        });
    if (!report) return std::unexpected(report.error());
    return emit(*report);
  }
  auto report = bench::run(*project, driver, repeat, samples);
  if (!report) return std::unexpected(report.error());
  return emit(*report);
}

Result<int> gate_command(const Parsed& args) {
  auto project = project_arg(args);
  if (!project) return std::unexpected(project.error());
  gate::Options options;
  options.clean = args.flag("clean");
  options.cxx = args.value("cxx");
  options.jobs = args.number("jobs").value_or(default_jobs());
  if (args.flag("bench")) {
    options.relevance =
        std::pair<std::size_t, std::size_t>{args.number("repeat").value_or(500), 1};
    options.bench_samples =
        std::clamp<std::size_t>(args.number("samples").value_or(3), 1, 20);
  }
  auto report = gate::run(*project, options);
  if (!report) return std::unexpected(report.error());
  if (args.flag("json")) {
    std::print("{}\n", dump(gate::to_json(*report, *project), true));
  } else {
    std::print("{}", gate::render(*report, *project));
  }
  return report->passed() ? 0 : 1;
}

Result<int> record_command(const Parsed& args) {
  auto project = project_arg(args);
  if (!project) return std::unexpected(project.error());
  const std::size_t cases = args.number("count").value_or(args.number("cases").value_or(64));
  const std::uint64_t seed = args.number("seed").value_or(20260819);
  const auto inputs = args.value("inputs").has_value()
                          ? std::optional<std::filesystem::path>(*args.value("inputs"))
                          : std::nullopt;
  std::size_t count = 0;
  if (const auto lang = langc::parse_lang(project->source_lang); lang.has_value()) {
    const std::filesystem::path repo =
        project->package_root.value_or(project->source_file.parent_path());
    auto unit = langc::unit(repo, project->source_file.string(), *lang);
    if (!unit) return std::unexpected(unit.error());
    auto recorded = langc::record(*project, *unit, cases, *lang);
    if (!recorded) return std::unexpected(recorded.error());
    if (recorded->crashes > 0) {
      std::print("  {} case(s) crashed the original: recorded as crashes, and any port behaviour "
                 "is accepted there (undefined behaviour)\n",
                 recorded->crashes);
    }
    count = recorded->cases;
    if (const auto tolerance = args.value("float-tolerance"); tolerance.has_value()) {
      try {
        project->tolerance = std::stod(*tolerance);
      } catch (const std::exception&) {
        return std::unexpected("--float-tolerance \"" + *tolerance + "\" is not a number");
      }
    }
    Recorded entry;
    entry.cases = count;
    entry.at = now_iso8601();
    project->recorded = entry;
    auto saved = project->save();
    if (!saved) return std::unexpected(saved.error());
    std::print("cpp-anything record: {} cases for {} → {}/cases.jsonl\n", count, project->module,
               project->dir.string());
    std::print("  next: write src/port.cpp, then `cpp-anything diff {}`\n", project->dir.string());
    return 0;
  }
  if (const auto lang = langs::parse(project->source_lang); lang.has_value()) {
    if (langs::whole_app_only(*lang)) {
      return std::unexpected(std::format(
          "{} is whole-app only: record the app (`app record -- ...`) instead of a unit",
          langs::label(*lang)));
    }
    if (args.flag("from-tests")) {
      return std::unexpected(std::format(
          "--from-tests runs a Python test suite; {} tests arrive with the language plugin",
          langs::label(*lang)));
    }
    langs::RecordOptions options;
    if (inputs.has_value()) options.inputs = *inputs;
    if (const auto value = args.value("module"); value.has_value()) {
      options.module = std::filesystem::path(*value);
    } else if (project->source_module.has_value()) {
      options.module = std::filesystem::path(*project->source_module);
    }
    if (const auto value = args.value("resolve-from"); value.has_value()) {
      options.resolve_from = std::filesystem::path(*value);
    } else if (project->resolve_from.has_value()) {
      options.resolve_from = std::filesystem::path(*project->resolve_from);
    }
    options.node_args =
        args.many("node-arg").empty() ? project->node_args : args.many("node-arg");
    auto recorded = langs::record(*project, cases, options);
    if (!recorded) return std::unexpected(recorded.error());
    count = *recorded;
  } else {
    auto recorded = python::record(*project, cases, seed, inputs);
    if (!recorded) return std::unexpected(recorded.error());
    count = *recorded;
  }
  if (const auto tolerance = args.value("float-tolerance"); tolerance.has_value()) {
    try {
      project->tolerance = std::stod(*tolerance);
    } catch (const std::exception&) {
      return std::unexpected("--float-tolerance \"" + *tolerance + "\" is not a number");
    }
  }
  std::optional<python::FromTests> suite;
  if (args.flag("from-tests")) {
    if (args.trailing.empty()) {
      return std::unexpected(
          "--from-tests needs the test command after `--`, e.g. `record . --from-tests -- pytest "
          "-q`");
    }
    auto result = python::record_from_tests(*project, args.trailing);
    if (!result) return std::unexpected(result.error());
    suite = std::move(*result);
  }
  Recorded entry;
  entry.cases = count;
  entry.at = now_iso8601();
  project->recorded = entry;
  auto saved = project->save();
  if (!saved) return std::unexpected(saved.error());
  std::print("cpp-anything record: {} cases for {} → {}/cases.jsonl\n", count, project->module,
             project->dir.string());
  if (suite.has_value()) {
    std::string command;
    for (const std::string& part : suite->command) {
      if (!command.empty()) command += ' ';
      command += part;
    }
    std::print("  from tests ({}): {} call(s) captured, {} new case(s), suite {}\n", command,
               suite->calls, suite->added,
               suite->suite_ok ? "passed" : "failed (" + suite->suite_error + ")");
    if (suite->coverage.has_value()) {
      std::print("  coverage: {} of {} source lines reached by the suite\n",
                 suite->coverage->first, suite->coverage->second);
    }
    if (!suite->suite_ok) {
      std::print("  the cases the suite did reach were kept; fix the suite to capture the rest\n");
    }
  }
  if (project->tolerance > 0.0) {
    std::print("  float tolerance: {} (saved with the project; diff and gate use it)\n",
               project->tolerance);
  }
  std::print("  next: write src/port.cpp, then `cpp-anything diff {}`\n", project->dir.string());
  return 0;
}

Result<int> build_command(const Parsed& args) {
  auto project = project_arg(args);
  if (!project) return std::unexpected(project.error());
  std::optional<std::string> cxx;
  if (const auto value = args.value("cxx"); value.has_value()) cxx = *value;
  const std::size_t jobs = args.number("jobs").value_or(default_jobs());
  auto outcome = build::build_project(*project, args.flag("clean"), cxx, jobs);
  if (!outcome) return std::unexpected(outcome.error());
  std::print("cpp-anything build: {} ({})\n  {} warning(s)\n  next: `cpp-anything diff {}`\n",
             project->module, short_path(outcome->driver), outcome->warnings,
             project->dir.string());
  return 0;
}

Result<int> diff_command(const Parsed& args) {
  auto project = project_arg(args);
  if (!project) return std::unexpected(project.error());
  const auto driver = build::driver_path(*project);
  if (!std::filesystem::exists(driver)) {
    return std::unexpected(short_path(driver) + " is not built yet: run `cpp-anything build " +
                           project->dir.string() + "` first");
  }
  diff::Options options;
  if (const auto tolerance = args.value("tolerance"); tolerance.has_value()) {
    try {
      options.tolerance = std::stod(*tolerance);
    } catch (const std::exception&) {
      options.tolerance = 0.0;
    }
  }
  options.strict_errors = args.flag("strict-errors");
  options.only = args.value("only");
  options.max_report = args.number("max-report").value_or(8);
  auto report = diff::run(*project, driver, options);
  if (!report) return std::unexpected(report.error());
  if (args.flag("json")) {
    std::print("{}\n", dump(diff::to_json(*report, *project), true));
  } else {
    std::print("{}", diff::render(*report, *project, options));
  }
  return report->passed() ? 0 : 1;
}

Result<int> refresh_command(const Parsed& args) {
  auto project = project_arg(args);
  if (!project) return std::unexpected(project.error());
  Result<Unit> unit = std::unexpected("no unit");
  if (const auto native_lang = langc::parse_lang(project->source_lang);
      native_lang.has_value()) {
    const std::filesystem::path repo =
        project->package_root.value_or(project->source_file.parent_path());
    unit = langc::unit(repo, project->source_file.string(), *native_lang);
  } else if (const auto plugin_lang = langs::parse(project->source_lang);
             plugin_lang.has_value()) {
    const std::filesystem::path repo = project->package_root.value_or(project->dir);
    unit = langs::inventory(repo, project->source_file.string(), *plugin_lang);
  } else {
    unit = python::inventory(project->source_file, project->module, project->package_root);
  }
  if (!unit) return std::unexpected(unit.error());
  project->functions = unit->functions;
  project->skipped = unit->skipped;
  project->risks = unit->risks;
  project->imports = unit->imports;
  project->lines = unit->lines;
  auto batches = parse_batches(*project, args.many("batch"));
  if (!batches) return std::unexpected(batches.error());
  for (const Batch& batch : *batches) {
    const bool seen = std::ranges::any_of(project->batches, [&](const Batch& existing) {
      return existing.function == batch.function && existing.param == batch.param;
    });
    if (!seen) project->batches.push_back(batch);
  }
  auto written = codegen::refresh(*project);
  if (!written) return std::unexpected(written.error());
  auto saved = project->save();
  if (!saved) return std::unexpected(saved.error());
  std::print("cpp-anything refresh: {} function(s), {} file(s) regenerated, src/port.cpp "
             "untouched\n",
             project->functions.size(), written->size());
  for (const Function& function : project->functions) {
    std::print("  · {}\n", function.signature_line());
  }
  return 0;
}

Result<int> analyze_command(const Parsed& args) {
  if (args.positional.empty()) return std::unexpected("analyze needs a repository path");
  const std::filesystem::path repo = args.positional[0];
  const bool whole_app = args.flag("whole-app");
  Result<Json> doc = std::unexpected("no language");
  bool handled = false;
  if (const auto lang = args.value("lang"); lang.has_value()) {
    if (*lang == "python" || *lang == "py") {
      doc = analyze::analyze(repo);
      handled = true;
    } else if (const auto native = langc::parse_lang(*lang); native.has_value()) {
      doc = langc::analyze_repo(repo, *native);
      handled = true;
    } else if (const auto plugin = langs::parse(*lang); plugin.has_value()) {
      doc = langs::analyze_repo(repo, *plugin);
      handled = true;
    }
    if (!handled) {
      return std::unexpected("unknown language " + debug_quote(*lang) +
                             ": try python, c, cpp, ts, js, go, rust, ruby, java");
    }
  } else if (const auto plugin = langs::detect(repo); plugin.has_value()) {
    doc = langs::analyze_repo(repo, *plugin);
  } else {
    doc = analyze::analyze(repo);
  }
  if (!doc) return std::unexpected(doc.error());
  const Json json = analyze::to_json(*doc, repo, whole_app);
  if (args.flag("json")) {
    std::print("{}\n", dump(json, true));
    return 0;
  }
  const std::string plan = analyze::render_plan(*doc, repo, whole_app);
  if (const auto out = args.value("out"); out.has_value()) {
    auto written = analyze::write_plan(*out, plan);
    if (!written) return std::unexpected(written.error());
    std::print("wrote {} ({})\n", *out, analyze::summarize(*doc));
    return 0;
  }
  std::print("{}", plan);
  return 0;
}

Result<int> matrix_command(const Parsed& args) {
  const std::string sub = args.positional.empty() ? "features" : args.positional[0];
  if (sub == "features" || sub == "select" || sub == "support") return matrix_features(args);
  if (sub == "explain") return matrix_explain(args);
  if (sub == "profile") return matrix_profile(args);
  if (sub == "docs") return matrix_docs(args);
  if (sub == "check") return matrix_check();
  if (sub == "sync") {
    // The data lives in the checkout and the binary embeds it, so syncing is a
    // two-step operation: refresh data/matrix.json, re-render the guides, then
    // rebuild to embed the new matrix.
    const auto root = repo_root();
    if (!root.has_value()) {
      return std::unexpected(
          "matrix sync needs the cpp-anything checkout: no data/matrix.json found above the "
          "current directory");
    }
    const std::filesystem::path script = *root / "scripts" / "sync_matrix.py";
    if (!std::filesystem::exists(script)) {
      return std::unexpected(script.string() + " is missing");
    }
    std::vector<std::string> argv = {script.string()};
    if (args.flag("offline")) argv.push_back("--offline");
    const Run run = run_command(python::interpreter(), argv, *root);
    std::print("{}", run.out);
    if (!run.ok) {
      return std::unexpected("sync failed: " + run.failure() + "\n" + indent_text(run.err, 2));
    }
    auto guides = docs::write(*root / "docs" / "guides");
    if (!guides) return std::unexpected(guides.error());
    std::print("re-rendered {} guides under docs/guides\n", *guides);
    std::print("rebuild to embed the new matrix: `cmake --build build` (or `cmake --install "
               "build`)\n");
    return 0;
  }
  return std::unexpected("unknown matrix command " + debug_quote(sub) +
                         "; try features, explain, profile, docs, check");
}

}  // namespace

Result<int> run(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    std::print("{}", help_text());
    return 0;
  }
  const std::string& command = argv[0];
  const std::vector<std::string> rest(argv.begin() + 1, argv.end());
  auto parsed = parse_args(rest);
  if (!parsed) return std::unexpected(parsed.error());
  if (parsed->flag("help")) {
    std::print("{}", help_text());
    return 0;
  }
  if (command == "matrix") return matrix_command(*parsed);
  if (command == "app") return app::command(*parsed);
  if (command == "analyze") return analyze_command(*parsed);
  if (command == "bridge") return bridge_command(*parsed);
  if (command == "refresh") return refresh_command(*parsed);
  if (command == "record") return record_command(*parsed);
  if (command == "build") return build_command(*parsed);
  if (command == "diff") return diff_command(*parsed);
  if (command == "bench") return bench_command(*parsed);
  if (command == "shadow") return shadow_command(*parsed);
  if (command == "map") return map_command(*parsed);
  if (command == "setup") return setup::command(*parsed);
  if (command == "doctor") return doctor::command(*parsed);
  if (command == "rsi") return rsi::command(*parsed);
  if (command == "profile") return profile::command(*parsed);
  if (command == "mcp") return mcp::serve();
  if (command == "gate") return gate_command(*parsed);
  if (command == "help" || command == "--help") {
    std::print("{}", help_text());
    return 0;
  }
  return std::unexpected("unknown command " + debug_quote(command) +
                         "; run `cpp-anything help`");
}

std::string_view help_text() {
  return R"(cpp-anything — rewrite your app in C++ (C++20/23) and prove it still behaves the same

  matrix    what compilers and standards support, from cppreference's tables
  profile   what the workload actually costs, and which function to move first
  app       record a whole CLI, replay any build against it, and put a C++ front door in front
  analyze   the units in a repository and what porting each one costs
  bridge    generate the C++ project for one unit (port skeleton + driver)
  record    run the original on generated edge cases (and optionally its test suite) and save what it returns
  build     configure and build the C++ project with CMake
  diff      replay every recorded case through the port and compare
  bench     the original's per-call cost against the port's
  gate      build + diff + matrix + dispatch checks; done means this passes
  shadow    generate a drop-in module that runs the original and the port side by side
  refresh   regenerate the generated files after a signature change (--batch adds a batch class)
  map       every unit, its port, its cases and last diff, and which originals changed
  setup     register the MCP server (and skill) with Claude Code, Codex, Cursor, Gemini CLI, OpenCode
  rsi       verify cpp-anything itself with its own loop
  doctor    check the tools a port needs
  mcp       run as an MCP server on stdio

Run `cpp-anything <command> --help` for one command's options.

  matrix features [--toolchain gcc] [--version 13] [--std c++23] [--kind core|library] [--json]
  matrix explain <feature|macro> [--json]
  matrix profile  [--toolchain gcc] [--version 13] [--std c++23] [-o FILE]
  matrix docs     [--out DIR] [--check]
  matrix sync     [--offline]
  profile <repo> [--json] [--top N] -- node app.mjs | ruby bench.rb | go test -bench . | java Main | python3 bench.py
  app record  <repo> [--inputs FILE] [--normalize RULE] [--no-traps] [--timeout S] [-- python3 cli.py]
  app replay  <repo> [--build CMD] [--case NAME] [--json]
  app front-door <repo> [--no-check] [--library]
  app wire    <repo> <unit> --command NAME          (CLI: one command per unit)
  app wire    <repo> <unit> --route "GET /path"     (server: repeatable; {param}, *)
  app lift    <repo>
  analyze <repo>  [-o plan.md] [--json] [--whole-app] [--lang python|c|cpp|ts|js|go|rust|ruby|java|php|csharp]
  bridge  <repo> <unit> [--lang ts|js|go|rust|ruby|java|c|cpp] [--out DIR] [--toolchain gcc]
                        [--version 13] [--std c++23] [--modules auto|on|off] [--module-root DIR]
                        [--keep name:reason]… [--as-json T]… [--kind napi]
                        [--node-arg ARG]… [--resolve-from DIR] [--module FILE]
                        [--force] [--batch function:param]…
  refresh <project> [--batch function:param]…
  record  <project> [--count N] [--seed N] [--inputs FILE] [--float-tolerance F]
                    [--module FILE] [--resolve-from DIR] [--node-arg ARG]… [--from-tests -- pytest -q]
  diff    <project> [--json] [--strict-errors] [--tolerance F] [--only FN] [--max-report N]
  shadow  <project> [--report] [--check] [--json]
  gate    <project> [--json] [--clean] [--cxx PATH] [--jobs N] [--bench]
  map     [<repo>] [--crates DIR] [--write] [--check] [--json]
  setup   [--agent claude|codex|cursor|gemini|opencode] [--remove] [--dry-run] [--json]
  rsi     check|record [--json]
)";
}

}  // namespace cppan::cli