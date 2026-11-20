// Whole-app cases: run a CLI black-box, twice, and store what it did.
//
// The unit loop records functions; this records the app: arguments, stdin, the
// environment, files it was given, and everything observable afterwards — stdout,
// stderr, exit code, files written, and whether the build answered from the port
// or fell back to the original.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppan/json.hpp"
#include "cppan/util.hpp"

namespace cppan::appcase {

inline constexpr std::string_view kSchemaConfig = "cpp-anything/app-cases@1";
inline constexpr std::string_view kCasesDir = "cpp/app-cases";

struct Config {
  std::string name;
  std::vector<std::string> run;
  std::uint64_t timeout_secs = 30;
  std::vector<std::string> normalize;
  double float_tolerance = 0.0;
  bool http = false;
  std::optional<std::uint16_t> port;
  std::vector<std::string> args;
  std::vector<std::string> fresh;
  std::uint64_t pace_ms = 0;
  std::vector<std::string> effects;
  std::vector<std::string> effect_env;
  std::optional<std::string> db_reset;
  std::optional<std::string> db_dump;
  bool lifted = false;

  [[nodiscard]] static Result<Config> load(const std::filesystem::path& dir);
  [[nodiscard]] Result<void> save(const std::filesystem::path& dir) const;
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

struct HttpExpect {
  std::uint16_t status = 0;
  std::string content_type;
  std::string body;
};

struct Expect {
  std::optional<HttpExpect> http;
  std::string stdout;
  std::string stderr;
  int exit = 0;
  std::vector<std::pair<std::string, std::string>> files;
  bool fallback = false;
  std::vector<std::string> effects;
};

struct Case {
  std::optional<HttpRequest> request;
  std::string name;
  std::vector<std::string> args;
  std::string stdin;
  std::vector<std::pair<std::string, std::string>> env;
  std::vector<std::pair<std::string, std::string>> files;
  Expect expect;
  /// The app did not produce the same output twice: replay reports it, never fails on it.
  bool unstable = false;
  std::string source;

  [[nodiscard]] Json to_json() const;
  [[nodiscard]] static Result<Case> parse(const Json& doc);
};

[[nodiscard]] std::filesystem::path cases_path(const std::filesystem::path& repo);
[[nodiscard]] Result<std::vector<Case>> load_cases(const std::filesystem::path& repo);
[[nodiscard]] Result<void> save_cases(const std::filesystem::path& repo,
                                      const std::vector<Case>& cases);

struct RunOptions {
  const std::filesystem::path& repo;
  const std::vector<std::string>& command;
  std::uint64_t timeout = 0;
  std::vector<std::pair<std::string, std::string>> extra_env;
  /// A command run in the case's sandbox before the app (a database reset).
  std::optional<std::string> pre_command;
  /// A command run after the app; its output is part of the case (a database dump).
  std::optional<std::string> post_command;
};

/// A recorded command stores repository-relative words; running it must not depend
/// on where the tool was started.
[[nodiscard]] std::vector<std::string> resolve_command(const std::filesystem::path& repo,
                                                       const std::vector<std::string>& command);

struct Observation {
  std::string stdout_text;
  std::string stderr_text;
  int exit = 0;
  std::vector<std::pair<std::string, std::string>> files;
  bool fallback = false;
  std::vector<std::string> effects;
  /// Output of the post-running command (a database dump), one line per entry.
  std::vector<std::string> database;
};

/// Runs one case in a fresh sandbox: its files written, its stdin piped, its
/// environment merged over the caller's, and a `timeout` so a hung app is a case,
/// not a hang.
[[nodiscard]] Result<Observation> observe(const Case& item, const RunOptions& options);

/// Timestamps, uuids, json key order, or `rule=literal` replacements.
[[nodiscard]] std::string normalize(std::string_view text, const std::vector<std::string>& rules);

/// Two runs must agree; when they do not, the case is kept but marked unstable and
/// replay reports it instead of failing on a value the app does not control.
[[nodiscard]] Case settle(const std::string& name, const std::string& source, Observation first,
                          Observation second, const std::vector<std::string>& rules);

/// Text comparison for replay: a case that was unstable when recorded is reported,
/// not failed, because the app itself did not produce one answer.
[[nodiscard]] std::optional<std::string> compare(const Case& item, const Observation& observed,
                                                 const std::vector<std::string>& rules);

}  // namespace cppan::appcase
