// The scripting-language plugins: the analyzer, the recorder and the binding that
// each source language needs. The loop around them (wire format, shadow log, app,
// map, gate) is the same code Python and C already use.
//
// Supported signatures are a deliberate subset: plain module-level functions with
// values the wire can carry. Everything else is reported as skipped with a reason,
// never guessed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppan/inventory.hpp"
#include "cppan/json.hpp"
#include "cppan/util.hpp"

namespace cppan::langs {

enum class Lang : std::uint8_t { Ts, Js, Go, Ruby, Java, Php, Csharp };

[[nodiscard]] std::string_view id(Lang lang);
[[nodiscard]] std::string_view label(Lang lang);
/// PHP and C# are whole-app only: the app loop, not per-unit ports.
[[nodiscard]] bool whole_app_only(Lang lang);
[[nodiscard]] std::vector<std::string_view> extensions(Lang lang);
/// A unit is a file, except in Go where it is a package (a folder).
[[nodiscard]] bool unit_is_directory(Lang lang);
[[nodiscard]] bool is_ts_family(Lang lang);

[[nodiscard]] std::optional<Lang> parse(std::string_view name);

/// The repository's language from its manifest, else from the most common source
/// extension. `nullopt` means "not one of the plugins": the caller keeps Python.
[[nodiscard]] std::optional<Lang> detect(const std::filesystem::path& repo);

/// Which plugin owns a unit, from its path: a file extension, or a `.go` package
/// directory. `--lang` overrides this.
[[nodiscard]] std::optional<Lang> infer(const std::filesystem::path& repo, std::string_view unit);

struct FoundParam {
  std::string name;
  std::string ty;
  bool has_default = false;
  std::optional<Json> default_value;
};

struct Found {
  std::string name;
  std::uint32_t line = 0;
  /// How the harness calls the original (`TOPLEVEL`, `Pkg.Fn`, `Class.method`).
  std::string call;
  std::vector<FoundParam> params;
  std::string returns;
  std::string doc;
};

struct Analysis {
  std::vector<Found> functions;
  std::vector<std::pair<std::string, std::string>> skipped;
  std::vector<std::string> imports;
  std::uint32_t lines = 0;

  [[nodiscard]] Function to_function(const Found& found) const;
};

struct RecordOptions {
  std::optional<std::filesystem::path> inputs;
  /// Import the original from this module instead of the source file (TS/JS).
  std::optional<std::filesystem::path> module;
  std::optional<std::filesystem::path> resolve_from;
  std::vector<std::string> node_args;
};

// --- per-language analyzers (one translation unit each) ----------------------

namespace csharp {
[[nodiscard]] Result<Analysis> analyze(const std::filesystem::path& path);
}
namespace go {
[[nodiscard]] Result<Analysis> analyze_package(const std::filesystem::path& directory);
}
namespace java {
[[nodiscard]] Result<Analysis> analyze(const std::filesystem::path& path);
}
namespace php {
[[nodiscard]] Result<Analysis> analyze(const std::filesystem::path& path);
}
namespace ruby {
[[nodiscard]] Result<Analysis> analyze(const std::filesystem::path& path);
}
namespace ts {
[[nodiscard]] Result<Analysis> analyze(const std::filesystem::path& path, Lang lang);
}

// --- shared plan building ----------------------------------------------------

[[nodiscard]] Result<Json> analyze_repo(const std::filesystem::path& repo, Lang lang);
[[nodiscard]] std::vector<std::string_view> language_risks(Lang lang);
[[nodiscard]] std::string module_name(const std::filesystem::path& relative);
[[nodiscard]] Result<Analysis> analyze_file(const std::filesystem::path& file, Lang lang);

/// Resolves the unit argument to a file (or a Go package directory) and builds the
/// inventory the rest of the tool consumes.
[[nodiscard]] Result<Unit> inventory(const std::filesystem::path& repo, std::string_view unit,
                                     Lang lang);

}  // namespace cppan::langs
