// Small, dependency-free plumbing: versions, text, paths, embedded data.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cppan {

/// The error type every fallible helper returns: a message meant for a person.
using Error = std::string;

template <typename T>
using Result = std::expected<T, Error>;

/// Numeric-aware version compare: `19.9 < 19.10`, `15 < 15.2`.
[[nodiscard]] std::vector<std::uint64_t> version_key(std::string_view text);
[[nodiscard]] bool version_at_least(std::string_view version, std::string_view minimum);
[[nodiscard]] int version_cmp(std::string_view left, std::string_view right);

/// Shortens `text` to `width` characters, ending in `…`, and escapes newlines.
[[nodiscard]] std::string truncate_text(std::string_view text, std::size_t width);

/// Left-aligns `text` to `width` characters (UTF-8 code points, as Rust's `{:<N}`).
[[nodiscard]] std::string pad_right(std::string_view text, std::size_t width);
[[nodiscard]] std::string pad_left(std::string_view text, std::size_t width);
/// Rust's `{:?}` for a string: quoted, with the escapes a report needs.
[[nodiscard]] std::string quote_debug(std::string_view text);

/// A finished child process: exit status (none when it could not be started),
/// captured stdout and stderr.
struct Run {
  bool ok = false;
  std::optional<int> status;
  std::string out;
  std::string err;

  [[nodiscard]] std::string failure() const;
};

/// Runs a command with no shell, capturing both streams. A missing binary is a
/// failure with `status == nullopt`, so callers can report it instead of crashing.
[[nodiscard]] Run run_command(const std::string& program, const std::vector<std::string>& args,
                              const std::optional<std::filesystem::path>& cwd = std::nullopt,
                              const std::vector<std::pair<std::string, std::string>>& envs = {});

/// Same, feeding `input` to stdin and closing it: how the doctor asks the MCP
/// server its two questions.
[[nodiscard]] Run run_command_with_input(const std::string& program,
                                         const std::vector<std::string>& args,
                                         std::string_view input);

/// FNV-1a 64: enough to notice that generated docs drifted, without a hash crate.
[[nodiscard]] std::string hash64(std::string_view text);

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path);
Result<void> write_file(const std::filesystem::path& path, std::string_view contents);

/// Creates a directory, refusing when it is neither absent nor empty unless
/// `force` is set. Returns true when an existing non-empty directory was reused.
[[nodiscard]] Result<bool> prepare_dir(const std::filesystem::path& path, bool force);

/// Walks up from the current directory looking for the cpp-anything checkout.
/// The running binary, wherever the platform keeps it.
[[nodiscard]] std::filesystem::path current_executable();

/// The first executable named `program` on PATH.
[[nodiscard]] std::optional<std::filesystem::path> which_on_path(std::string_view program);

[[nodiscard]] std::optional<std::filesystem::path> repo_root();

/// `path` relative to the current directory when it is below it.
[[nodiscard]] std::string short_path(const std::filesystem::path& path);

[[nodiscard]] std::string indent_text(std::string_view text, std::size_t spaces);

/// `text.lines()`: one view per line, no trailing empty line after a final newline.
[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text);
[[nodiscard]] std::size_t line_count(std::string_view text);

}  // namespace cppan

namespace cppan {

/// `2026-11-20T02:15:00Z`, the timestamp `project.json` records.
[[nodiscard]] std::string now_iso8601();

/// `path` relative to `base` (`../` segments when needed), for metadata that
/// survives moving the checkout.
[[nodiscard]] std::optional<std::filesystem::path> relative_path(
    const std::filesystem::path& base, const std::filesystem::path& path);

/// Lowercase ASCII alphanumerics with `_` runs; the stem a C++ identifier can use.
[[nodiscard]] std::string sanitize_identifier(std::string_view text);

}  // namespace cppan
