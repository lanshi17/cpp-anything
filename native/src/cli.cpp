#include "cppan/cli.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <print>

#include "cppan/analyze.hpp"
#include "cppan/docs.hpp"
#include "cppan/json.hpp"
#include "cppan/matrix.hpp"

namespace cppan::cli {
namespace {

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

struct Target {
  std::string toolchain;
  std::string version;
  std::string standard;
};

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

Result<int> analyze_command(const Parsed& args) {
  if (args.positional.empty()) return std::unexpected("analyze needs a repository path");
  const std::filesystem::path repo = args.positional[0];
  const bool whole_app = args.flag("whole-app");
  if (const auto lang = args.value("lang"); lang.has_value() && *lang != "python" && *lang != "py") {
    return std::unexpected("unknown language " + debug_quote(*lang) +
                           ": try python, c, cpp, ts, js, go, ruby, java");
  }
  auto doc = analyze::analyze(repo);
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
    return std::unexpected(
        "matrix sync needs scripts/sync_matrix.py and a checkout; not ported yet");
  }
  return std::unexpected("unknown matrix command " + debug_quote(sub) +
                         "; try features, explain, profile, docs, check");
}

}  // namespace

std::optional<std::string> Parsed::value(std::string_view name) const {
  if (auto found = values.find(std::string(name)); found != values.end()) return found->second;
  return std::nullopt;
}

bool Parsed::flag(std::string_view name) const { return flags.contains(std::string(name)); }

std::vector<std::string> Parsed::many(std::string_view name) const {
  if (auto found = multi.find(std::string(name)); found != multi.end()) return found->second;
  return {};
}

std::optional<std::uint64_t> Parsed::number(std::string_view name) const {
  const auto text = value(name);
  if (!text.has_value()) return std::nullopt;
  try {
    std::size_t used = 0;
    const auto number = std::stoull(*text, &used);
    if (used != text->size()) return std::nullopt;
    return number;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

Result<Parsed> parse_args(const std::vector<std::string>& argv) {
  static const std::set<std::string> kBooleanFlags = {
      "json",  "force", "clean",       "check", "strict-errors", "help",
      "list",  "bench", "refresh",     "offline", "library"};
  Parsed parsed;
  std::size_t index = 0;
  while (index < argv.size()) {
    const std::string& arg = argv[index];
    if (arg == "--") {
      parsed.trailing.assign(argv.begin() + static_cast<std::ptrdiff_t>(index + 1), argv.end());
      break;
    }
    if (arg.starts_with("--")) {
      const std::string body = arg.substr(2);
      if (const std::size_t equals = body.find('='); equals != std::string::npos) {
        const std::string key = body.substr(0, equals);
        const std::string value = body.substr(equals + 1);
        parsed.values[key] = value;
        parsed.multi[key].push_back(value);
      } else if (kBooleanFlags.contains(body)) {
        parsed.flags.insert(body);
      } else if (index + 1 < argv.size() && !argv[index + 1].starts_with("--")) {
        parsed.values[body] = argv[index + 1];
        parsed.multi[body].push_back(argv[index + 1]);
        ++index;
      } else {
        parsed.flags.insert(body);
      }
    } else if (arg == "-h") {
      parsed.flags.insert("help");
    } else if (arg == "-o") {
      if (index + 1 >= argv.size()) return std::unexpected("-o needs a file name");
      parsed.values["out"] = argv[index + 1];
      ++index;
    } else {
      parsed.positional.push_back(arg);
    }
    ++index;
  }
  return parsed;
}

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
  if (command == "analyze") return analyze_command(*parsed);
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
  analyze <repo>  [-o plan.md] [--json] [--whole-app] [--lang python|c|cpp|ts|js|go|ruby|java|php|csharp]
  bridge  <repo> <unit> [--lang ts|js|go|ruby|java|c|cpp] [--out DIR] [--toolchain gcc]
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
