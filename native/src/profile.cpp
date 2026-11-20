#include "cppan/profile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <print>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "cppan/langc.hpp"
#include "cppan/langs.hpp"
#include "cppan/python.hpp"

namespace cppan::profile {
namespace {

// ---- small text helpers ------------------------------------------------------

std::string_view trim_start(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  return text;
}

std::string_view trim_end(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

std::string_view trim(std::string_view text) { return trim_start(trim_end(text)); }

/// `str::lines()`: split on `\n`, drop one trailing `\r`, no empty tail.
std::vector<std::string_view> rust_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      std::string_view line = text.substr(start);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      lines.push_back(line);
      break;
    }
    std::string_view line = text.substr(start, newline - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    start = newline + 1;
  }
  return lines;
}

std::vector<std::string_view> split_whitespace(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
    const std::size_t start = index;
    while (index < text.size() && !std::isspace(static_cast<unsigned char>(text[index]))) ++index;
    if (index > start) parts.push_back(text.substr(start, index - start));
  }
  return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out += separator;
    out += part;
  }
  return out;
}

std::string replace_all(std::string text, std::string_view needle, std::string_view with) {
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), with);
    at += with.size();
  }
  return text;
}

std::string replace_backslashes(std::string text) {
  std::replace(text.begin(), text.end(), '\\', '/');
  return text;
}

bool all_digits(std::string_view text) {
  if (text.empty()) return false;
  for (const char value : text) {
    if (value < '0' || value > '9') return false;
  }
  return true;
}

std::optional<std::int64_t> parse_i64(std::string_view text) {
  if (text.empty()) return std::nullopt;
  if (text.front() == '+') text.remove_prefix(1);
  if (text.empty()) return std::nullopt;
  std::int64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<double> parse_f64(std::string_view text) {
  if (text.empty()) return std::nullopt;
  if (text.front() == '+') text.remove_prefix(1);
  if (text.empty()) return std::nullopt;
  double value = 0.0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::string env_value(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

long process_id() {
#if defined(_WIN32)
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

// ---- JSON field access, mirroring `Json::get` + `as_*` ------------------------

const Json::Array& empty_array() {
  static const Json::Array value;
  return value;
}

std::string string_at(const Json& value, std::string_view key) {
  const Json* item = value.find(key);
  return item != nullptr && item->is_string() ? item->as_string() : std::string();
}

std::optional<double> number_at(const Json& value, std::string_view key) {
  const Json* item = value.find(key);
  if (item == nullptr || !item->is_number()) return std::nullopt;
  return item->as_number();
}

std::optional<std::int64_t> int_at(const Json& value, std::string_view key) {
  const Json* item = value.find(key);
  if (item == nullptr || !item->is_integer()) return std::nullopt;
  return item->as_int();
}

const Json::Array& array_at(const Json& value, std::string_view key) {
  const Json* item = value.find(key);
  return item != nullptr && item->is_array() ? item->as_array() : empty_array();
}

// ---- frame classification ----------------------------------------------------

std::string decode_file_url(std::string_view raw) {
  std::string_view path = raw;
  if (path.starts_with("file://")) path.remove_prefix(7);
  if (path.find('%') == std::string_view::npos) return std::string(path);
  std::string bytes;
  bytes.reserve(path.size());
  std::size_t index = 0;
  const auto hex_digit = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  while (index < path.size()) {
    if (path[index] == '%' && index + 2 < path.size()) {
      int high = hex_digit(path[index + 1]);
      const int low = hex_digit(path[index + 2]);
      if (high < 0 && path[index + 1] == '+') high = 0;  // `u8::from_str_radix` accepts `+`
      if (high >= 0 && low >= 0) {
        bytes.push_back(static_cast<char>((high << 4) | low));
        index += 3;
        continue;
      }
    }
    bytes.push_back(path[index]);
    ++index;
  }
  // `String::from_utf8_lossy`: valid sequences pass through, everything else
  // becomes U+FFFD so a path is never truncated.
  std::string out;
  out.reserve(bytes.size());
  const auto is_continuation = [](unsigned char value) { return (value & 0xC0) == 0x80; };
  std::size_t at = 0;
  while (at < bytes.size()) {
    const unsigned char lead = static_cast<unsigned char>(bytes[at]);
    if (lead < 0x80) {
      out.push_back(static_cast<char>(lead));
      ++at;
      continue;
    }
    std::size_t length = 0;
    if (lead >= 0xC2 && lead <= 0xDF) length = 2;
    else if (lead >= 0xE0 && lead <= 0xEF) length = 3;
    else if (lead >= 0xF0 && lead <= 0xF4) length = 4;
    bool valid = length != 0 && at + length <= bytes.size();
    for (std::size_t offset = 1; valid && offset < length; ++offset) {
      if (!is_continuation(static_cast<unsigned char>(bytes[at + offset]))) valid = false;
    }
    if (valid) {
      out.append(bytes, at, length);
      at += length;
    } else {
      out += "\xEF\xBF\xBD";
      ++at;
    }
  }
  return out;
}

/// Turns a profiler's file reference into `(file, external)`: repository files
/// stay relative, files elsewhere keep their absolute path and are marked
/// external, and runtime pseudo-frames (`node:internal/…`, `<frozen …>`) are
/// dropped because there is nothing to port there.
std::optional<std::pair<std::string, bool>> classify(const std::filesystem::path& root,
                                                     std::string_view raw) {
  const std::string decoded = decode_file_url(raw);
  std::string_view path = decoded;
  if (path.empty() || path.front() == '<' || path.front() == '~') return std::nullopt;
  const bool absolute =
      path.front() == '/' ||
      (path.size() > 2 && path[1] == ':' && (path[2] == '\\' || path[2] == '/'));
  if (!absolute) {
    if (path.find(':') != std::string_view::npos) return std::nullopt;  // a URL scheme, not a file
    while (path.starts_with("./")) path.remove_prefix(2);
    std::error_code error;
    if (std::filesystem::is_regular_file(root / std::string(path), error)) {
      return std::pair{replace_backslashes(std::string(path)), false};
    }
    return std::nullopt;
  }
  // Installed dependencies stay external even when they sit inside the repo:
  // they are not code the user wrote.
  bool library = false;
  std::size_t start = 0;
  for (std::size_t index = 0; index <= path.size(); ++index) {
    if (index != path.size() && path[index] != '/' && path[index] != '\\') continue;
    const std::string_view part = path.substr(start, index - start);
    if (part == "node_modules" || part == "site-packages" || part == "dist-packages" ||
        part == ".venv" || part == "venv" || part == "gems" || part == "vendor") {
      library = true;
      break;
    }
    start = index + 1;
  }
  if (!library) {
    for (const std::string_view directory : {"/lib/python3", "/lib/ruby/", "/pkg/mod/"}) {
      if (path.find(directory) != std::string_view::npos) {
        library = true;
        break;
      }
    }
  }
  if (library) return std::pair{std::string(path), true};
  // `Path::strip_prefix`, component by component and purely lexical.
  const std::filesystem::path candidate(path);
  auto base = root.begin();
  auto part = candidate.begin();
  while (base != root.end() && part != candidate.end() && *base == *part) {
    ++base;
    ++part;
  }
  if (base == root.end()) {
    std::filesystem::path relative;
    for (; part != candidate.end(); ++part) relative /= *part;
    return std::pair{replace_backslashes(relative.generic_string()), false};
  }
  return std::pair{std::string(path), true};
}

// ---- shared model ------------------------------------------------------------

/// A frame in user terms: file (repository-relative, or absolute when the file
/// is outside the repository), function, line and whether it is library code.
struct FrameKey {
  std::string file;
  std::string function;
  std::int64_t line = 0;
  bool external = false;
};

bool operator==(const FrameKey& left, const FrameKey& right) {
  return left.line == right.line && left.external == right.external &&
         left.file == right.file && left.function == right.function;
}

struct FrameKeyLess {
  bool operator()(const FrameKey& left, const FrameKey& right) const {
    if (left.file != right.file) return left.file < right.file;
    if (left.function != right.function) return left.function < right.function;
    if (left.line != right.line) return left.line < right.line;
    return left.external < right.external;
  }
};

struct FrameKeyHash {
  std::size_t operator()(const FrameKey& key) const {
    std::size_t value = std::hash<std::string>{}(key.file);
    value ^= std::hash<std::string>{}(key.function) + 0x9e3779b97f4a7c15ULL + (value << 6) +
             (value >> 2);
    value ^= std::hash<std::int64_t>{}(key.line) + 0x9e3779b97f4a7c15ULL + (value << 6) +
             (value >> 2);
    value ^= std::hash<bool>{}(key.external) + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
    return value;
  }
};

struct Acc {
  double self_seconds = 0.0;
  double cum_seconds = 0.0;
  std::int64_t calls = 0;
  /// Caller -> (inclusive seconds observed under it, calls it made).
  std::map<FrameKey, std::pair<double, std::int64_t>, FrameKeyLess> callers;
};

using Functions = std::unordered_map<FrameKey, Acc, FrameKeyHash>;
using Sample = std::pair<std::vector<std::optional<FrameKey>>, double>;

std::string format_ns(double nanos) {
  if (!std::isfinite(nanos) || nanos <= 0.0) return "0 ns";
  if (nanos >= 1'000'000.0) return std::format("{:.2f} ms", nanos / 1'000'000.0);
  if (nanos >= 1'000.0) return std::format("{:.2f} µs", nanos / 1'000.0);
  return std::format("{:.0f} ns", nanos);
}

/// The rules an experienced porter applies, written down:
/// a cheap leaf called in a loop is not worth one call per item; batching is.
std::string advice(const Entry& entry, double total_seconds) {
  if (entry.harness) {
    return "the workload harness: not part of the port; profile what it calls";
  }
  if (entry.external) {
    return "third-party or stdlib code: not yours to port; port your callers instead";
  }
  if (entry.calls >= 500 && entry.per_call_ns < 2'000.0) {
    return std::format(
        "called {} times at {:.0f} ns each: one call per item costs more than the work, port it "
        "with its callers or batch it (`bridge <repo> <unit> --batch {}:<param>`)",
        entry.calls, entry.per_call_ns, entry.function);
  }
  if (entry.calls >= 500) {
    return std::format(
        "hot leaf ({} calls, {:.0f} ns each): batch it (`--batch {}:<param>`) so the loop stays on "
        "the C++ side",
        entry.calls, entry.per_call_ns, entry.function);
  }
  if (entry.share >= 0.10) {
    return std::format(
        "top cost ({:.0f}% of the workload): port this first, then `diff` and `bench`",
        entry.share * 100.0);
  }
  // Sampling profilers see inlined frames only as callers: their own time is
  // attributed to the leaf, but their inclusive time is still the best signal.
  if (entry.self_seconds <= 0.0 && total_seconds > 0.0 &&
      entry.cum_seconds / total_seconds >= 0.10) {
    return std::format(
        "top cost inclusive ({:.0f}% of the time sampled here, mostly in what it calls): port "
        "this first, then `diff` and `bench`",
        entry.cum_seconds / total_seconds * 100.0);
  }
  return "port with its callers; alone it is unlikely to pay for the crossing";
}

Report assemble(const std::vector<std::string>& workload, const Functions& functions,
                double total_seconds, std::size_t top) {
  std::vector<Entry> entries;
  entries.reserve(functions.size());
  for (const auto& [key, acc] : functions) {
    if (!(acc.self_seconds > 0.0 || acc.cum_seconds > 0.0)) continue;
    std::vector<std::pair<std::string, double>> callers;
    callers.reserve(acc.callers.size());
    for (const auto& [caller, observed] : acc.callers) {
      callers.emplace_back(caller.function, observed.first);
    }
    std::stable_sort(callers.begin(), callers.end(),
                     [](const auto& left, const auto& right) { return right.second < left.second; });
    if (callers.size() > 5) callers.resize(5);
    Entry entry;
    entry.harness = false;
    entry.function = key.function;
    entry.file = key.file;
    entry.line = key.line;
    entry.calls = acc.calls;
    entry.self_seconds = acc.self_seconds;
    entry.cum_seconds = acc.cum_seconds;
    entry.per_call_ns =
        acc.calls > 0 ? acc.cum_seconds * 1e9 / static_cast<double>(acc.calls) : 0.0;
    entry.share = total_seconds > 0.0 ? acc.self_seconds / total_seconds : 0.0;
    entry.callers.reserve(callers.size());
    for (const auto& [name, seconds] : callers) {
      (void)seconds;
      entry.callers.push_back(name);
    }
    entry.external = key.external;
    entries.push_back(std::move(entry));
  }
  for (Entry& entry : entries) entry.advice = advice(entry, total_seconds);
  // Cost order, but never let a library frame hide a repository one: the point
  // of the report is what the user can port.
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
    if (left.external != right.external) return !left.external;
    if (left.cum_seconds != right.cum_seconds) return right.cum_seconds < left.cum_seconds;
    return right.self_seconds < left.self_seconds;
  });
  const std::size_t limit = std::max<std::size_t>(top, 1);
  if (entries.size() > limit) entries.resize(limit);
  Report report;
  report.workload = workload;
  report.total_seconds = total_seconds;
  report.entries = std::move(entries);
  return report;
}

// ---- runtime dispatch --------------------------------------------------------

enum class Runtime { Node, Ruby, Go, Java, Python };

Runtime runtime_of(std::string_view program) {
  if (program.ends_with(".exe")) program.remove_suffix(4);
  if (program == "node" || program == "nodejs") return Runtime::Node;
  if (program == "go") return Runtime::Go;
  if (program == "java") return Runtime::Java;
  if (program == "ruby" || program.starts_with("ruby3") || program.starts_with("ruby2")) {
    return Runtime::Ruby;
  }
  return Runtime::Python;
}

Result<std::filesystem::path> canonical_root(const std::filesystem::path& root) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(root, error);
  if (error) return std::unexpected(root.string() + ": " + error.message());
  return canonical;
}

/// A private scratch directory under the system temp dir; profilers write their
/// raw recordings there and nothing leaks into the repository.
Result<std::filesystem::path> work_dir() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const std::uint64_t stamp = nanos < 0 ? 0 : static_cast<std::uint64_t>(nanos % 1'000'000'000);
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) return std::unexpected(error.message());
  const std::filesystem::path work =
      base / std::format("cpp-anything-profile-{}-{}", process_id(), stamp);
  std::filesystem::remove_all(work, error);
  error.clear();
  std::filesystem::create_directories(work, error);
  if (error) return std::unexpected(work.string() + ": " + error.message());
  return work;
}

// ---- node --cpu-prof ---------------------------------------------------------

struct NodeFrame {
  std::optional<FrameKey> key;
  double self_seconds = 0.0;
  std::vector<std::int64_t> children;
};

/// Adds one `.cpuprofile` to `functions`; returns its duration in seconds.
double merge_cpuprofile(const std::filesystem::path& root, const Json& profile,
                        Functions& functions) {
  const Json* nodes = profile.find("nodes");
  if (nodes == nullptr || !nodes->is_array()) return 0.0;
  const std::size_t sample_count = std::max<std::size_t>(array_at(profile, "samples").size(), 1);
  const double start = number_at(profile, "startTime").value_or(0.0);
  const double end_time = number_at(profile, "endTime").value_or(0.0);
  const double duration_seconds = std::max((end_time - start) / 1e6, 0.0);
  const double per_sample = duration_seconds / static_cast<double>(sample_count);
  // `hitCount` is the sample count per node; fall back to counting `samples`
  // when a producer omits it.
  std::unordered_map<std::int64_t, double> hits;
  for (const Json& id : array_at(profile, "samples")) {
    if (id.is_integer()) hits[id.as_int()] += 1.0;
  }
  std::unordered_map<std::int64_t, NodeFrame> by_id;
  std::vector<std::int64_t> order;
  std::unordered_set<std::int64_t> is_child;
  for (const Json& node : nodes->as_array()) {
    const std::int64_t id = int_at(node, "id").value_or(0);
    const Json* frame = node.find("callFrame");
    const std::string name = frame != nullptr ? string_at(*frame, "functionName") : std::string();
    const std::string url = frame != nullptr ? string_at(*frame, "url") : std::string();
    const Json* line_json = frame != nullptr ? frame->find("lineNumber") : nullptr;
    const std::int64_t raw_line =
        line_json != nullptr && line_json->is_integer() ? line_json->as_int() : -1;
    const std::int64_t line = std::max<std::int64_t>(raw_line + 1, 0);
    std::optional<FrameKey> key;
    if (auto classified = classify(root, url); classified.has_value()) {
      key = FrameKey{classified->first, name.empty() ? std::string("(anonymous)") : name, line,
                     classified->second};
    }
    std::vector<std::int64_t> children;
    for (const Json& child : array_at(node, "children")) {
      if (child.is_integer()) children.push_back(child.as_int());
    }
    for (const std::int64_t child : children) is_child.insert(child);
    double self_seconds = 0.0;
    const Json* hit = node.find("hitCount");
    if (hit != nullptr && hit->is_number()) {
      self_seconds = hit->as_number() * per_sample;
    } else {
      const auto found = hits.find(id);
      self_seconds = (found != hits.end() ? found->second : 0.0) * per_sample;
    }
    if (!by_id.contains(id)) order.push_back(id);
    by_id[id] = NodeFrame{std::move(key), self_seconds, std::move(children)};
  }
  // Inclusive time per node, depth first.
  std::unordered_map<std::int64_t, double> memo;
  std::function<double(std::int64_t)> inclusive = [&](std::int64_t id) -> double {
    if (const auto found = memo.find(id); found != memo.end()) return found->second;
    const auto frame = by_id.find(id);
    if (frame == by_id.end()) return 0.0;
    double total = frame->second.self_seconds;
    for (const std::int64_t child : frame->second.children) total += inclusive(child);
    memo[id] = total;
    return total;
  };
  // Walk from the roots with the repository frames on the stack: recursion
  // counts once, and a callee's caller is the nearest *named* frame above it,
  // so anonymous callbacks are transparent.
  std::vector<std::pair<std::int64_t, std::vector<FrameKey>>> stack;
  for (const std::int64_t id : order) {
    if (!is_child.contains(id)) stack.emplace_back(id, std::vector<FrameKey>{});
  }
  while (!stack.empty()) {
    auto [id, active] = std::move(stack.back());
    stack.pop_back();
    const auto frame = by_id.find(id);
    if (frame == by_id.end()) continue;
    if (frame->second.key.has_value()) {
      const FrameKey& key = *frame->second.key;
      const double total = inclusive(id);
      Acc& entry = functions[key];
      entry.self_seconds += frame->second.self_seconds;
      if (std::find(active.begin(), active.end(), key) == active.end()) {
        entry.cum_seconds += total;
        const FrameKey* caller = nullptr;
        for (auto it = active.rbegin(); it != active.rend(); ++it) {
          if (!it->function.starts_with("(anonymous)")) {
            caller = &*it;
            break;
          }
        }
        if (caller != nullptr && !(*caller == key)) {
          entry.callers[*caller].first += total;
        }
      }
    }
    std::vector<FrameKey> next = active;
    if (frame->second.key.has_value()) next.push_back(*frame->second.key);
    for (const std::int64_t child : frame->second.children) {
      stack.emplace_back(child, next);
    }
  }
  return duration_seconds;
}

Result<std::pair<Functions, double>> node(const std::filesystem::path& root,
                                          const std::vector<std::string>& command,
                                          const std::filesystem::path& work) {
  const std::filesystem::path dir = work / "node";
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  if (error) return std::unexpected(dir.string() + ": " + error.message());
  // NODE_OPTIONS splits on whitespace but honours double quotes; escape the
  // path so a temp directory with spaces still works.
  std::string dir_option = dir.string();
  dir_option = replace_all(std::move(dir_option), "\\", "\\\\");
  dir_option = replace_all(std::move(dir_option), "\"", "\\\"");
  const std::string options =
      std::string(trim(env_value("NODE_OPTIONS") + " --cpu-prof --cpu-prof-dir=\"" +
                       dir_option + "\""));
  const std::vector<std::string> args(command.begin() + 1, command.end());
  const Run run = run_command(command[0], args, root, {{"NODE_OPTIONS", options}});
  if (!run.ok) return std::unexpected("the workload failed (" + run.failure() + ")");
  std::vector<std::filesystem::path> profiles;
  std::filesystem::directory_iterator iterator(dir, error);
  if (error) return std::unexpected(dir.string() + ": " + error.message());
  for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
    if (error) return std::unexpected(dir.string() + ": " + error.message());
    if (iterator->path().extension().string() == ".cpuprofile") {
      profiles.push_back(iterator->path());
    }
  }
  if (profiles.empty()) {
    return std::unexpected(
        "no CPU profile was written: is the workload a node process, and did it run?");
  }
  std::sort(profiles.begin(), profiles.end());
  Functions functions;
  double total_seconds = 0.0;
  for (const std::filesystem::path& path : profiles) {
    const auto text = read_file(path);
    if (!text.has_value()) return std::unexpected(path.string() + ": cannot read");
    Json profile;
    try {
      profile = parse(trim(*text));
    } catch (const JsonError& failure) {
      return std::unexpected(path.string() + ": " + failure.what());
    }
    total_seconds += merge_cpuprofile(root, profile, functions);
  }
  return std::pair{std::move(functions), total_seconds};
}

// ---- python cProfile (the bundled probe) -------------------------------------

std::string script_of(const std::vector<std::string>& workload) {
  for (const std::string& part : workload) {
    if (part.ends_with(".py")) return part;
  }
  return std::string();
}

Report rank(const Json& doc, std::size_t top) {
  std::vector<std::string> workload;
  for (const Json& item : array_at(doc, "workload")) {
    if (item.is_string()) workload.push_back(item.as_string());
  }
  std::vector<Entry> entries;
  double total_seconds = 0.0;
  const std::string script = script_of(workload);
  for (const Json& item : array_at(doc, "functions")) {
    const std::string file = string_at(item, "file");
    const std::string function = string_at(item, "function");
    const double self_seconds = number_at(item, "self_seconds").value_or(0.0);
    if (self_seconds <= 0.0 || function.empty()) continue;
    // Frozen modules and C builtins have no file to port, and the string pstats
    // builds for them is unreadable: drop them.
    if (!file.empty() && file.front() == '<') continue;
    total_seconds += self_seconds;
    Entry entry;
    entry.harness = file == script;
    entry.function = function;
    entry.file = file;
    entry.line = int_at(item, "line").value_or(0);
    entry.calls = int_at(item, "calls").value_or(0);
    entry.self_seconds = self_seconds;
    entry.cum_seconds = number_at(item, "cum_seconds").value_or(0.0);
    entry.per_call_ns = number_at(item, "per_call_seconds").value_or(0.0) * 1e9;
    const Json* callers = item.find("callers");
    if (callers != nullptr && callers->is_array()) {
      for (const Json& caller : callers->as_array()) {
        const std::string name = string_at(caller, "function");
        if (name != "<module>") entry.callers.push_back(name);
      }
    }
    entry.external = file.find("site-packages") != std::string::npos ||
                     file.find("/lib/python") != std::string::npos ||
                     file.starts_with("<frozen");
    entries.push_back(std::move(entry));
  }
  for (Entry& entry : entries) {
    entry.share = total_seconds > 0.0 ? entry.self_seconds / total_seconds : 0.0;
    entry.advice = advice(entry, total_seconds);
  }
  const std::size_t limit = std::max<std::size_t>(top, 1);
  if (entries.size() > limit) entries.resize(limit);
  Report report;
  report.workload = std::move(workload);
  report.total_seconds = total_seconds;
  report.entries = std::move(entries);
  return report;
}

// ---- Ruby TracePoint tracer --------------------------------------------------

/// Loaded with `RUBYOPT=-r`: times every Ruby method call on the main thread
/// (inclusive, self, callers) and writes cProfile-like entries at exit.
/// Tracing slows the program down; the shares stay comparable.
constexpr std::string_view kRubyTracer = R"RUBY(
require "json"

module CppAnythingProfile
  STATS = Hash.new { |h, k| h[k] = { calls: 0, self: 0.0, total: 0.0, callers: Hash.new { |c, ck| c[ck] = [0, 0.0] } } }
  STACK = []
  ACTIVE = Hash.new(0)
  MAIN = Thread.current
  TRACE = TracePoint.new(:call, :return) do |tp|
    next unless Thread.current == MAIN
    now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    if tp.event == :call
      path = tp.path
      path = File.expand_path(path) unless path.start_with?("<")
      key = [path, tp.lineno, tp.method_id.to_s]
      ACTIVE[key] += 1
      STACK << [key, now, 0.0]
    elsif (frame = STACK.pop)
      key, start, child = frame
      elapsed = now - start
      ACTIVE[key] -= 1
      outer = ACTIVE[key].zero?
      s = STATS[key]
      s[:calls] += 1
      s[:self] += elapsed - child
      s[:total] += elapsed if outer
      if (parent = STACK.last)
        parent[2] += elapsed
        c = s[:callers][parent[0]]
        c[0] += 1
        c[1] += elapsed if outer
      end
    end
  end
  TRACE.enable

  at_exit do
    TRACE.disable
    out = STATS.map do |(file, line, name), s|
      { file: file, line: line, name: name, calls: s[:calls], self: s[:self] * 1000, total: s[:total] * 1000,
        callers: s[:callers].map { |(f, l, n), (calls, total)| { file: f, line: l, name: n, calls: calls, total: total * 1000 } } }
    end
    File.write(ENV.fetch("CPP_ANYTHING_PROFILE_OUT"), JSON.generate(out))
  end
end
)RUBY";

/// Function entries from a tracing profiler (the Ruby tracer): file, line,
/// name, calls, self and total milliseconds, and callers.
void ingest_entries(const std::filesystem::path& root, const Json::Array& entries,
                    Functions& functions) {
  for (const Json& entry : entries) {
    const std::string raw_file = string_at(entry, "file");
    const auto classified = classify(root, raw_file);
    if (!classified.has_value()) continue;
    const std::string function = string_at(entry, "name");
    if (function.empty()) continue;
    const FrameKey key{classified->first, function, int_at(entry, "line").value_or(0),
                       classified->second};
    Acc& acc = functions[key];
    acc.self_seconds += number_at(entry, "self").value_or(0.0) / 1000.0;
    acc.cum_seconds += number_at(entry, "total").value_or(0.0) / 1000.0;
    acc.calls += int_at(entry, "calls").value_or(0);
    for (const Json& caller : array_at(entry, "callers")) {
      const std::string raw_caller = string_at(caller, "file");
      const auto caller_classified = classify(root, raw_caller);
      if (!caller_classified.has_value()) continue;
      FrameKey caller_key{caller_classified->first, string_at(caller, "name"),
                          int_at(caller, "line").value_or(0), caller_classified->second};
      if (caller_key.function.empty()) continue;
      auto& slot = acc.callers[caller_key];
      slot.first += number_at(caller, "total").value_or(0.0) / 1000.0;
      slot.second += int_at(caller, "calls").value_or(0);
    }
  }
}

/// Ruby: the tracer above, loaded into any Ruby command through `RUBYOPT`.
Result<std::pair<Functions, double>> ruby(const std::filesystem::path& root,
                                          const std::vector<std::string>& command,
                                          const std::filesystem::path& work) {
  const std::filesystem::path tracer = work / "cpp_anything_profile.rb";
  if (auto written = write_file(tracer, kRubyTracer); !written) {
    return std::unexpected(written.error());
  }
  const std::filesystem::path out = work / "profile.json";
  const std::string rubyopt =
      std::string(trim(env_value("RUBYOPT") + " -r" + tracer.string()));
  const auto start = std::chrono::steady_clock::now();
  const std::vector<std::string> args(command.begin() + 1, command.end());
  const Run run = run_command(command[0], args, root,
                              {{"RUBYOPT", rubyopt},
                               {"CPP_ANYTHING_PROFILE_OUT", out.string()}});
  const double total_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (!run.ok) return std::unexpected("the workload failed (" + run.failure() + ")");
  const auto text = read_file(out);
  if (!text.has_value()) {
    return std::unexpected(
        "the Ruby tracer wrote nothing: is the workload a Ruby process, and did it reach exit?");
  }
  Json entries;
  try {
    entries = parse(trim(*text));
  } catch (const JsonError& failure) {
    return std::unexpected(std::string("ruby tracer: ") + failure.what());
  }
  Functions functions;
  if (entries.is_array()) ingest_entries(root, entries.as_array(), functions);
  return std::pair{std::move(functions), total_seconds};
}

// ---- sampled stacks (Go pprof, Java JFR) -------------------------------------

/// Adds sampled stacks, each root first with `None` for frames outside user
/// code, and the time the sample stands for. A function's inclusive time counts
/// once per sample (recursion); its caller is the nearest named user frame above
/// it (lambdas and closures are skipped).
void merge_samples(const std::vector<Sample>& samples, Functions& functions) {
  for (const auto& [stack, seconds] : samples) {
    std::vector<FrameKey> seen;
    const FrameKey* named = nullptr;
    for (const auto& frame : stack) {
      if (!frame.has_value()) continue;
      const FrameKey& key = *frame;
      if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
        seen.push_back(key);
        Acc& acc = functions[key];
        acc.cum_seconds += seconds;
        if (named != nullptr && !(*named == key)) acc.callers[*named].first += seconds;
      }
      if (!key.function.starts_with("(anonymous)")) named = &key;
    }
    if (!stack.empty() && stack.back().has_value()) {
      functions[*stack.back()].self_seconds += seconds;
    }
  }
}

// ---- go test -cpuprofile -----------------------------------------------------

/// A Go function's own name: `example.com/x/pkg.Slugify` is `Slugify`,
/// `pkg.(*T).M` is `M`; closures (`Outer.func1`) are anonymous.
std::string go_name(std::string_view full) {
  const std::size_t slash = full.rfind('/');
  const std::string_view tail =
      slash == std::string_view::npos ? full : full.substr(slash + 1);
  std::string_view rest = tail;
  if (const std::size_t dot = tail.find('.'); dot != std::string_view::npos) {
    rest = tail.substr(dot + 1);
  }
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t dot = rest.find('.', start);
    if (dot == std::string_view::npos) {
      parts.push_back(rest.substr(start));
      break;
    }
    parts.push_back(rest.substr(start, dot - start));
    start = dot + 1;
  }
  if (parts.size() >= 2) {
    const std::string_view function = parts.back();
    if (function.starts_with("func")) {
      const std::string_view suffix = function.substr(4);
      const bool numbered = std::all_of(suffix.begin(), suffix.end(), [](char value) {
        return value >= '0' && value <= '9';
      });
      if (numbered) return "(anonymous) in " + std::string(parts[parts.size() - 2]);
    }
  }
  return std::string(parts.back());
}

/// Strips the `:line:column` suffix `go tool pprof` puts after a file.
std::string_view pprof_file(std::string_view place) {
  for (int round = 0; round < 2; ++round) {
    const std::size_t colon = place.rfind(':');
    if (colon == std::string_view::npos) break;
    if (!all_digits(place.substr(colon + 1))) break;
    place = place.substr(0, colon);
  }
  return place;
}

/// Seconds per unit in the sample type line, e.g. `cpu/nanoseconds`.
std::optional<double> unit_seconds(std::string_view unit) {
  if (unit == "nanoseconds") return 1e-9;
  if (unit == "microseconds" || unit == "us") return 1e-6;
  if (unit == "milliseconds" || unit == "ms") return 1e-3;
  if (unit == "seconds" || unit == "s") return 1.0;
  return std::nullopt;
}

/// Go's own sources; asking `go` once is cheaper than embedding a GOROOT guess.
const std::optional<std::string>& goroot() {
  static const std::optional<std::string> value = []() -> std::optional<std::string> {
    const Run run = run_command("go", {"env", "GOROOT"});
    const std::string root(trim(run.out));
    if (!run.ok || root.empty()) return std::nullopt;
    std::string prefix = root;
    if (!prefix.ends_with('/')) prefix.push_back('/');
    return prefix;
  }();
  return value;
}

/// One frame line inside a pprof location; `None` in the outer option means the
/// frame is runtime/library code that is not reported.
std::optional<std::optional<FrameKey>> pprof_frame(const std::filesystem::path& root,
                                                   std::string_view text) {
  const std::vector<std::string_view> parts = split_whitespace(text);
  if (parts.size() < 2) return std::nullopt;
  const std::string_view function = parts[0];
  const std::string_view place = parts[1];
  std::int64_t start = 0;
  for (std::size_t index = 2; index < parts.size(); ++index) {
    if (parts[index].starts_with("s=")) {
      start = parse_i64(parts[index].substr(2)).value_or(0);
      break;
    }
  }
  const std::string_view file = pprof_file(place);
  if (const auto& prefix = goroot(); prefix.has_value() && file.starts_with(*prefix)) {
    return std::optional<FrameKey>{};  // Go's runtime and standard library
  }
  const auto classified = classify(root, file);
  if (!classified.has_value()) return std::optional<FrameKey>{};
  return std::optional<FrameKey>{
      FrameKey{classified->first, go_name(function), start, classified->second}};
}

/// `go tool pprof -raw` output: samples (leaf first, by location id) and
/// locations (each one or more frames, innermost first: `func file:line:col s=start`).
std::vector<Sample> parse_pprof_raw(const std::filesystem::path& root, std::string_view raw) {
  std::unordered_map<std::int64_t, std::vector<std::optional<FrameKey>>> locations;
  std::vector<std::pair<std::vector<std::int64_t>, double>> samples;
  std::string section;
  std::optional<std::int64_t> current;
  // The CPU value is the second column of `samples/count cpu/nanoseconds`;
  // when no unit header is present Go CPU profiles store nanoseconds.
  std::size_t value_index = 1;
  double scale = 1e-9;
  for (const std::string_view line : rust_lines(raw)) {
    const std::string_view text = trim_end(line);
    if (text == "Samples:") {
      section = "samples";
      continue;
    }
    if (text == "Locations" || text == "Locations:") {
      section = "locations";
      continue;
    }
    if (text.starts_with("Mappings")) {
      section.clear();
      continue;
    }
    if (section == "samples" && text.find(':') != std::string_view::npos) {
      const std::size_t colon = text.find(':');
      const std::string_view values_text = text.substr(0, colon);
      const std::string_view ids_text = text.substr(colon + 1);
      std::vector<double> values;
      for (const std::string_view token : split_whitespace(values_text)) {
        if (const auto value = parse_f64(token); value.has_value()) values.push_back(*value);
      }
      std::vector<std::int64_t> ids;
      for (const std::string_view token : split_whitespace(ids_text)) {
        if (const auto value = parse_i64(token); value.has_value()) ids.push_back(*value);
      }
      if (!values.empty()) {
        const double value = value_index < values.size() ? values[value_index] : values.back();
        samples.emplace_back(std::move(ids), value * scale);
      }
      continue;
    }
    if (section == "samples" && text.find('/') != std::string_view::npos) {
      const std::vector<std::string_view> kinds = split_whitespace(text);
      std::optional<std::size_t> index;
      for (std::size_t position = 0; position < kinds.size(); ++position) {
        if (kinds[position].ends_with("/nanoseconds")) {
          index = position;
          break;
        }
      }
      if (!index.has_value()) {
        for (std::size_t position = 0; position < kinds.size(); ++position) {
          const std::string_view kind = kinds[position];
          const std::size_t slash = kind.rfind('/');
          const std::string_view unit =
              slash == std::string_view::npos ? kind : kind.substr(slash + 1);
          if (unit_seconds(unit).has_value()) {
            index = position;
            break;
          }
        }
      }
      if (index.has_value()) {
        value_index = *index;
        const std::string_view kind = kinds[*index];
        const std::size_t slash = kind.rfind('/');
        const std::string_view unit =
            slash == std::string_view::npos ? kind : kind.substr(slash + 1);
        scale = unit_seconds(unit).value_or(1e-9);
      }
      continue;
    }
    if (section != "locations") continue;
    const std::string_view trimmed = trim_start(text);
    std::string frame_text;
    const std::size_t separator = trimmed.find(": ");
    bool header = false;
    if (separator != std::string_view::npos) {
      const std::string_view id = trimmed.substr(0, separator);
      if (all_digits(id)) {
        current = parse_i64(id);
        const std::vector<std::string_view> rest = split_whitespace(trimmed.substr(separator + 2));
        std::string joined;
        // `0x4c2ded M=1 func file:line:col s=start`
        for (std::size_t position = 2; position < rest.size(); ++position) {
          if (!joined.empty()) joined += ' ';
          joined += rest[position];
        }
        frame_text = std::move(joined);
        header = true;
      }
    }
    if (!header) frame_text = std::string(trimmed);
    if (!current.has_value()) continue;
    if (const auto key = pprof_frame(root, frame_text); key.has_value()) {
      locations[*current].push_back(*key);
    }
  }
  std::vector<Sample> stacks;
  stacks.reserve(samples.size());
  for (auto& [ids, seconds] : samples) {
    // Leaf first, and inlined frames innermost first: reversed, the stack is
    // root first.
    std::vector<std::optional<FrameKey>> stack;
    for (const std::int64_t id : ids) {
      if (const auto found = locations.find(id); found != locations.end()) {
        stack.insert(stack.end(), found->second.begin(), found->second.end());
      }
    }
    std::reverse(stack.begin(), stack.end());
    stacks.emplace_back(std::move(stack), seconds);
  }
  return stacks;
}

/// Go: `go test` with `-cpuprofile` (Go has no profiler to attach from
/// outside), read with `go tool pprof -raw`.
Result<std::pair<Functions, double>> go(const std::filesystem::path& root,
                                        const std::vector<std::string>& command,
                                        const std::filesystem::path& work) {
  if (command.size() < 2 || command[1] != "test") {
    return std::unexpected(
        "for Go, profile a test or benchmark: `cpp-anything profile . -- go test -run '^$' "
        "-bench . ./pkg` (Go profiles only from inside the process; `go test` adds "
        "-cpuprofile)");
  }
  const std::filesystem::path profile = work / "cpu.pprof";
  std::vector<std::string> args = {"test", "-cpuprofile=" + profile.string()};
  for (std::size_t index = 2; index < command.size(); ++index) args.push_back(command[index]);
  const Run run = run_command(command[0], args, root);
  if (!run.ok) return std::unexpected("the workload failed (" + run.failure() + ")");
  if (!std::filesystem::is_regular_file(profile)) {
    return std::unexpected("no CPU profile was written to " + profile.string() +
                           ": does the package have tests to run?");
  }
  const Run raw = run_command(command[0], {"tool", "pprof", "-raw", profile.string()}, root);
  if (!raw.ok) return std::unexpected("go tool pprof failed: " + std::string(trim(raw.err)));
  const std::vector<Sample> samples = parse_pprof_raw(root, raw.out);
  double total_seconds = 0.0;
  for (const auto& sample : samples) total_seconds += sample.second;
  Functions functions;
  merge_samples(samples, functions);
  return std::pair{std::move(functions), total_seconds};
}

// ---- Java Flight Recorder ----------------------------------------------------

/// The line declaring method `name` in Java source: the nearest declaration at
/// or above `near`.
std::optional<std::int64_t> java_declaration(std::string_view text, std::string_view name,
                                             std::int64_t near) {
  const std::string needle = std::string(name) + "(";
  std::optional<std::int64_t> best;
  std::int64_t number = 0;
  const std::int64_t limit = std::max<std::int64_t>(near, 1);
  for (const std::string_view line : rust_lines(text)) {
    ++number;
    if (number > limit) break;
    const std::size_t at = line.find(needle);
    if (at == std::string_view::npos) continue;
    const std::string_view before = trim_end(line.substr(0, at));
    bool declares = false;
    if (!before.empty()) {
      const unsigned char last = static_cast<unsigned char>(before.back());
      declares = (std::isalnum(last) || last == '>' || last == ']' || last == '_') &&
                 before.find('=') == std::string_view::npos &&
                 before.find("return") == std::string_view::npos &&
                 before.find("new ") == std::string_view::npos && last != '.' &&
                 !trim_end(line).ends_with(';');
    }
    if (declares && at > 0 && std::isspace(static_cast<unsigned char>(line[at - 1]))) {
      best = number;
    }
  }
  return best;
}

bool skip_java_dir(std::string_view name) {
  for (const std::string_view skipped :
       {"target", "build", "out", "bin", ".git", "node_modules", ".gradle", ".idea", ".mvn",
        ".venv", "venv", "__pycache__"}) {
    if (name == skipped) return true;
  }
  return false;
}

void collect_java(const std::filesystem::path& dir, const std::filesystem::path& root,
                  std::unordered_map<std::string, std::string>& out, std::size_t depth) {
  if (depth > 16) return;
  std::error_code error;
  std::filesystem::directory_iterator iterator(dir, error);
  if (error) return;
  for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
    if (error) return;
    const std::filesystem::directory_entry& entry = *iterator;
    const std::filesystem::path path = entry.path();
    std::error_code kind_error;
    const std::filesystem::file_type kind = entry.symlink_status(kind_error).type();
    if (kind_error) continue;
    if (kind == std::filesystem::file_type::directory) {
      const std::string name = path.filename().string();
      if (skip_java_dir(name)) continue;
      collect_java(path, root, out, depth + 1);
      continue;
    }
    if (path.extension().string() != ".java") continue;
    auto base = root.begin();
    auto part = path.begin();
    while (base != root.end() && part != path.end() && *base == *part) {
      ++base;
      ++part;
    }
    if (base != root.end()) continue;
    std::filesystem::path relative;
    for (; part != path.end(); ++part) relative /= *part;
    const std::string relative_text = replace_backslashes(relative.generic_string());
    const std::string text = read_file(path).value_or(std::string());
    std::string package;
    for (const std::string_view line : rust_lines(text)) {
      const std::string_view trimmed = trim(line);
      if (trimmed.starts_with("package ")) {
        std::string_view rest = trimmed.substr(8);
        while (!rest.empty() && rest.back() == ';') rest.remove_suffix(1);
        package = std::string(trim(rest));
        break;
      }
    }
    std::string prefix;
    if (!package.empty()) {
      prefix = replace_all(std::move(package), ".", "/");
      prefix.push_back('/');
    }
    const std::string stem = path.stem().string();
    if (!stem.empty()) out.insert_or_assign(prefix + stem, relative_text);
    // Other (often non-public) top-level types in the same file still get
    // their own class name in samples.
    for (const std::string_view line : rust_lines(text)) {
      for (const std::string_view keyword : {"class ", "interface ", "record ", "enum "}) {
        const std::size_t at = line.find(keyword);
        if (at == std::string_view::npos) continue;
        const std::string_view before = line.substr(0, at);
        if (!before.empty()) {
          const unsigned char last = static_cast<unsigned char>(before.back());
          if (std::isalnum(last) || last == '_' || last == '.' || last == '$') {
            continue;  // `Foo.class`, `a.classify(`, not a declaration
          }
        }
        std::string name;
        for (const char value : trim_start(line.substr(at + keyword.size()))) {
          if (std::isalnum(static_cast<unsigned char>(value)) || value == '_' || value == '$') {
            name.push_back(value);
          } else {
            break;
          }
        }
        if (!name.empty()) out.insert_or_assign(prefix + name, relative_text);
      }
    }
  }
}

/// Java: Flight Recorder's execution samples, read with `jfr print --json`.
Result<std::pair<Functions, double>> java(const std::filesystem::path& root,
                                          const std::vector<std::string>& command,
                                          const std::filesystem::path& work) {
  const std::filesystem::path recording = work / "profile.jfr";
  const std::string options =
      std::string(trim(env_value("JAVA_TOOL_OPTIONS") + " -XX:StartFlightRecording=filename=" +
                       recording.string() +
                       ",settings=profile -XX:FlightRecorderOptions=stackdepth=256"));
  const std::vector<std::string> args(command.begin() + 1, command.end());
  const Run run = run_command(command[0], args, root, {{"JAVA_TOOL_OPTIONS", options}});
  if (!run.ok) return std::unexpected("the workload failed (" + run.failure() + ")");
  if (!std::filesystem::is_regular_file(recording)) {
    return std::unexpected(
        "no Flight Recorder file was written: is the workload a Java process launched by `java`?");
  }
  const Run printed =
      run_command("jfr",
                  {"print", "--json", "--stack-depth", "256", "--events", "jdk.ExecutionSample",
                   recording.string()},
                  root);
  if (!printed.ok) {
    return std::unexpected("jfr print failed (it comes with the JDK): " +
                           std::string(trim(printed.err)));
  }
  Json doc;
  try {
    doc = parse(trim(printed.out));
  } catch (const JsonError& failure) {
    return std::unexpected(std::string("reading the Flight Recorder output: ") + failure.what());
  }
  const Json* node = doc.find("recording");
  const Json* events_json = node != nullptr ? node->find("events") : nullptr;
  const Json::Array& events =
      events_json != nullptr && events_json->is_array() ? events_json->as_array() : empty_array();
  std::unordered_map<std::string, std::string> sources;
  collect_java(root, root, sources, 0);
  // The `profile` settings sample Java threads every 10 ms.
  constexpr double kSampleSeconds = 0.010;
  std::vector<std::vector<std::optional<FrameKey>>> stacks;
  std::size_t sample_events = 0;
  for (const Json& event : events) {
    if (const Json* type = event.find("type"); type != nullptr && type->is_string() &&
                                                   type->as_string() != "jdk.ExecutionSample") {
      continue;
    }
    ++sample_events;
    const Json* values = event.find("values");
    const Json* trace = values != nullptr ? values->find("stackTrace") : nullptr;
    const Json* frames = trace != nullptr ? trace->find("frames") : nullptr;
    const Json::Array& frame_list =
        frames != nullptr && frames->is_array() ? frames->as_array() : empty_array();
    std::vector<std::optional<FrameKey>> stack;
    for (const Json& frame : frame_list) {
      const Json* method = frame.find("method");
      const Json* kind = method != nullptr ? method->find("type") : nullptr;
      const std::string class_name = kind != nullptr ? string_at(*kind, "name") : std::string();
      const std::size_t dollar = class_name.find('$');
      const std::string outer =
          dollar == std::string::npos ? class_name : class_name.substr(0, dollar);
      const auto source = sources.find(outer);
      if (source == sources.end()) continue;  // JDK and third-party classes: nothing to port
      const std::string name = method != nullptr ? string_at(*method, "name") : std::string();
      if (name.empty()) continue;
      const std::int64_t line = int_at(frame, "lineNumber").value_or(0);
      std::string function = name;
      if (name.starts_with("lambda$") || name.starts_with('$')) {
        function = "(anonymous) " + name;
      }
      stack.push_back(FrameKey{source->second, std::move(function), line, false});
    }
    std::reverse(stack.begin(), stack.end());
    stacks.push_back(std::move(stack));
  }
  // Key each method by its declaration line, found from the lowest sampled line.
  std::map<std::pair<std::string, std::string>, std::int64_t> lowest;
  for (const auto& stack : stacks) {
    for (const auto& frame : stack) {
      if (!frame.has_value()) continue;
      auto [slot, inserted] = lowest.try_emplace({frame->file, frame->function}, frame->line);
      if (!inserted) slot->second = std::min(slot->second, frame->line);
    }
  }
  std::vector<Sample> resolved;
  resolved.reserve(stacks.size());
  std::unordered_map<std::string, std::string> texts;
  for (auto& stack : stacks) {
    std::vector<std::optional<FrameKey>> frames;
    frames.reserve(stack.size());
    for (auto& frame : stack) {
      if (!frame.has_value()) {
        frames.push_back(std::nullopt);
        continue;
      }
      FrameKey key = *frame;
      if (key.function.starts_with("(anonymous)")) {
        frames.push_back(std::move(key));
        continue;
      }
      const auto found = lowest.find({key.file, key.function});
      const std::int64_t near = found != lowest.end() ? found->second : key.line;
      auto [text, inserted] = texts.try_emplace(key.file);
      if (inserted) text->second = read_file(root / key.file).value_or(std::string());
      key.line = key.function.starts_with('<')
                     ? near
                     : java_declaration(text->second, key.function, near).value_or(near);
      frames.push_back(std::move(key));
    }
    resolved.emplace_back(std::move(frames), kSampleSeconds);
  }
  const double total_seconds = static_cast<double>(sample_events) * kSampleSeconds;
  Functions functions;
  merge_samples(resolved, functions);
  return std::pair{std::move(functions), total_seconds};
}

// ---- the report --------------------------------------------------------------

std::string module_of(std::string_view file) {
  return std::filesystem::path(std::string(file)).stem().string();
}

}  // namespace

Result<Report> run(const std::filesystem::path& root, const std::vector<std::string>& command,
                   std::size_t top) {
  if (command.empty()) {
    return std::unexpected(
        "profile needs the workload after `--`, e.g. `cpp-anything profile . -- python3 bench.py`");
  }
  const std::string program = std::filesystem::path(command[0]).filename().string();
  const Runtime runtime = runtime_of(program);
  if (runtime == Runtime::Python) {
    std::vector<std::string> args = {"profile"};
    args.insert(args.end(), command.begin(), command.end());
    // The workload's paths are relative to the caller's cwd, like every
    // other command the user would type; `root` is only for detection and
    // the report.
    auto output = python::probe_command(args);
    if (!output) return std::unexpected(output.error());
    Json doc;
    try {
      doc = parse(trim(*output));
    } catch (const JsonError& failure) {
      return std::unexpected(failure.what());
    }
    if (const Json* error = doc.find("error"); error != nullptr && error->is_string()) {
      const std::string stderr = string_at(doc, "stderr");
      return std::unexpected(error->as_string() + "\n" + indent_text(stderr, 2));
    }
    return rank(doc, top);
  }
  auto canonical = canonical_root(root);
  if (!canonical) return std::unexpected(canonical.error());
  auto work = work_dir();
  if (!work) return std::unexpected(work.error());
  Result<std::pair<Functions, double>> result = std::unexpected("unreachable runtime");
  switch (runtime) {
    case Runtime::Node: result = node(*canonical, command, *work); break;
    case Runtime::Ruby: result = ruby(*canonical, command, *work); break;
    case Runtime::Java: result = java(*canonical, command, *work); break;
    case Runtime::Go: result = go(*canonical, command, *work); break;
    case Runtime::Python: break;
  }
  std::error_code ignored;
  std::filesystem::remove_all(*work, ignored);
  if (!result) return std::unexpected(result.error());
  return assemble(command, result->first, result->second, top);
}

std::string render(const Report& report) {
  std::string out;
  out += std::format("cpp-anything profile: {} — {:.2f} s of self time across {} function(s)\n",
                     report.workload.empty() ? std::string("workload") : join(report.workload, " "),
                     report.total_seconds, report.entries.size());
  out += "\n   self%     calls     per call   function (file:line)\n";
  if (report.entries.empty()) {
    out +=
        "\nNo functions were found in the profile: run the workload longer, or all of its time "
        "is outside the repository.\n";
    out +=
        "\nNext: `cpp-anything analyze .`, then `cpp-anything bridge . <unit>` for the unit the "
        "hot function lives in.\n";
    return out;
  }
  for (const Entry& entry : report.entries) {
    out += std::format("  {:>5.1f}%  {:>9}  {:>10}   {}.{}  {}:{}\n", entry.share * 100.0,
                       entry.calls, format_ns(entry.per_call_ns), module_of(entry.file),
                       entry.function, short_path(std::filesystem::path(entry.file)), entry.line);
    if (entry.external || entry.harness) {
      out += std::format("            {}\n", entry.advice);
      continue;
    }
    if (!entry.callers.empty()) {
      std::string names;
      for (std::size_t index = 0; index < entry.callers.size() && index < 3; ++index) {
        if (index != 0) names += ", ";
        names += entry.callers[index];
      }
      out += std::format("            called from: {}\n", names);
    }
    out += std::format("            {}\n", entry.advice);
  }
  const bool all_external =
      std::all_of(report.entries.begin(), report.entries.end(),
                  [](const Entry& entry) { return entry.external; });
  if (all_external) {
    out +=
        "\nEverything hot is in a dependency: profile the code that calls it, or the work is not "
        "in the functions you own.\n";
  }
  out +=
      "\nNext: `cpp-anything analyze .`, then `cpp-anything bridge . <unit>` for the unit the hot "
      "function lives in.\n";
  return out;
}

Json to_json(const Report& report, const std::filesystem::path& root) {
  Json::Array workload;
  workload.reserve(report.workload.size());
  for (const std::string& part : report.workload) workload.push_back(Json::string(part));
  Json::Array functions;
  functions.reserve(report.entries.size());
  for (const Entry& entry : report.entries) {
    Json::Array callers;
    callers.reserve(entry.callers.size());
    for (const std::string& name : entry.callers) callers.push_back(Json::string(name));
    functions.push_back(Json::object({
        {"function", Json::string(entry.function)},
        {"file", Json::string(entry.file)},
        {"line", Json::integer(entry.line)},
        {"calls", Json::integer(entry.calls)},
        {"self_seconds", Json::real(entry.self_seconds)},
        {"cum_seconds", Json::real(entry.cum_seconds)},
        {"per_call_ns", Json::real(entry.per_call_ns)},
        {"share", Json::real(entry.share)},
        {"callers", Json::array(std::move(callers))},
        {"external", Json::boolean(entry.external)},
        {"advice", Json::string(entry.advice)},
    }));
  }
  return Json::object({
      {"schema", Json::string("cpp-anything/profile@1")},
      {"root", Json::string(root.string())},
      {"workload", Json::array(std::move(workload))},
      {"self_seconds", Json::real(report.total_seconds)},
      {"functions", Json::array(std::move(functions))},
  });
}

Result<int> command(const cli::Parsed& args) {
  const std::string root_text = args.positional.empty() ? "." : args.positional.front();
  if (const auto lang = args.value("lang"); lang.has_value()) {
    if (!(*lang == "python" || *lang == "py" || langc::parse_lang(*lang).has_value() ||
          langs::parse(*lang).has_value())) {
      return std::unexpected(
          "unknown language " + quote_debug(*lang) +
          ": cpp-anything reads python, c, cpp, ts, js, go, ruby, java, php and csharp (PHP and "
          "C# are whole-app only)");
    }
  }
  auto report = run(std::filesystem::path(root_text), args.trailing,
                    static_cast<std::size_t>(args.number("top").value_or(12)));
  if (!report) return std::unexpected(report.error());
  if (args.flag("json")) {
    std::print("{}\n", dump(to_json(*report, std::filesystem::path(root_text)), true));
  } else {
    std::print("{}", render(*report));
  }
  return 0;
}

}  // namespace cppan::profile
