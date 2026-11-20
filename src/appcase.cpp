module;
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

module cppan.appcase;
import cppan.json;
import cppan.types;
import cppan.util;
import std;

namespace cppan::appcase {
namespace {

std::string io_error(int code) {
  return std::string(std::strerror(code)) + " (os error " + std::to_string(code) + ")";
}

std::string path_text(const std::filesystem::path& path) { return path.string(); }

std::optional<std::string> read_text(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return std::nullopt;
  std::string out;
  char buffer[8192];
  while (true) {
    const ssize_t got = ::read(fd, buffer, sizeof buffer);
    if (got < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return std::nullopt;
    }
    if (got == 0) break;
    out.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  return out;
}

Result<std::string> read_required(const std::filesystem::path& path) {
  if (const auto text = read_text(path); text.has_value()) return *text;
  return std::unexpected(path_text(path) + ": " + io_error(errno == 0 ? ENOENT : errno));
}

std::string lossy(std::string_view bytes) {
  // String::from_utf8_lossy: valid UTF-8 passes through, invalid bytes become U+FFFD.
  std::string out;
  out.reserve(bytes.size());
  std::size_t index = 0;
  while (index < bytes.size()) {
    const unsigned char lead = static_cast<unsigned char>(bytes[index]);
    std::size_t length = 0;
    if (lead < 0x80) {
      length = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4;
    }
    bool valid = length != 0 && index + length <= bytes.size();
    if (valid && length > 1) {
      for (std::size_t at = 1; at < length; ++at) {
        if ((static_cast<unsigned char>(bytes[index + at]) & 0xC0U) != 0x80U) {
          valid = false;
          break;
        }
      }
    }
    if (valid) {
      out.append(bytes.substr(index, length));
      index += length;
    } else {
      out += "\xEF\xBF\xBD";
      ++index;
    }
  }
  return out;
}

/// Rust's `str::lines()`: split on '\n', strip one trailing '\r', no trailing
/// empty line after a final newline.
std::vector<std::string_view> rust_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t stop = newline == std::string_view::npos ? text.size() : newline;
    std::string_view line = text.substr(start, stop - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    if (newline == std::string_view::npos) break;
    start = newline + 1;
  }
  return lines;
}

std::string trim_ascii(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

const Json* get(const Json& doc, std::string_view key) { return doc.find(key); }

std::string string_of(const Json& doc, std::string_view key, std::string fallback) {
  const Json* field = get(doc, key);
  if (field == nullptr || !field->is_string()) return fallback;
  return field->as_string();
}

std::vector<std::string> strings_of(const Json& doc, std::string_view key) {
  std::vector<std::string> out;
  const Json* field = get(doc, key);
  if (field == nullptr || !field->is_array()) return out;
  for (const Json& item : field->as_array()) {
    if (item.is_string()) out.push_back(item.as_string());
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> pairs_of(const Json& doc, std::string_view key) {
  std::vector<std::pair<std::string, std::string>> out;
  const Json* field = get(doc, key);
  if (field == nullptr || !field->is_object()) return out;
  for (const auto& [name, value] : field->as_object()) {
    out.emplace_back(name, value.is_string() ? value.as_string() : std::string());
  }
  return out;
}

std::filesystem::path canonical_or_self(const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  return error ? path : canonical;
}

Json pairs_json(const std::vector<std::pair<std::string, std::string>>& source) {
  Json::Object fields;
  for (const auto& [key, value] : source) fields.emplace_back(key, Json::string(value));
  return Json::object(std::move(fields));
}

Json strings_json(const std::vector<std::string>& source) {
  Json::Array items;
  for (const auto& item : source) items.push_back(Json::string(item));
  return Json::array(std::move(items));
}

std::size_t codepoint_length(std::string_view text, std::size_t index) {
  const unsigned char lead = static_cast<unsigned char>(text[index]);
  if (lead < 0x80) return 1;
  if ((lead & 0xE0U) == 0xC0U) return 2;
  if ((lead & 0xF0U) == 0xE0U) return 3;
  if ((lead & 0xF8U) == 0xF0U) return 4;
  return 1;
}

std::optional<std::size_t> uuid_at(std::string_view bytes, std::size_t start) {
  const std::size_t groups[5] = {8, 4, 4, 4, 12};
  std::size_t index = start;
  for (std::size_t group = 0; group < 5; ++group) {
    for (std::size_t count = 0; count < groups[group]; ++count) {
      if (index >= bytes.size() ||
          !std::isxdigit(static_cast<unsigned char>(bytes[index]))) {
        return std::nullopt;
      }
      ++index;
    }
    if (group < 4) {
      if (index >= bytes.size() || bytes[index] != '-') return std::nullopt;
      ++index;
    }
  }
  return index;
}

std::string redact_uuids(std::string_view text) {
  const std::string_view bytes = text;
  std::string out;
  out.reserve(text.size());
  std::size_t index = 0;
  while (index < bytes.size()) {
    if (const auto end = uuid_at(bytes, index); end.has_value()) {
      out += "[..]";
      index = *end;
    } else {
      const std::size_t length = codepoint_length(bytes, index);
      out.append(bytes.substr(index, length));
      index += length;
    }
  }
  return out;
}

std::optional<std::size_t> digits_at(std::string_view bytes, std::size_t start,
                                     std::size_t count) {
  if (start + count > bytes.size()) return std::nullopt;
  for (std::size_t at = start; at < start + count; ++at) {
    if (!std::isdigit(static_cast<unsigned char>(bytes[at]))) return std::nullopt;
  }
  return start + count;
}

std::optional<std::size_t> timestamp_at(std::string_view bytes, std::size_t start) {
  auto index = digits_at(bytes, start, 4);
  if (!index.has_value() || *index >= bytes.size() || bytes[*index] != '-') return std::nullopt;
  index = digits_at(bytes, *index + 1, 2);
  if (!index.has_value() || *index >= bytes.size() || bytes[*index] != '-') return std::nullopt;
  index = digits_at(bytes, *index + 1, 2);
  if (!index.has_value()) return std::nullopt;
  if (*index >= bytes.size() || (bytes[*index] != 'T' && bytes[*index] != ' ')) {
    return std::nullopt;
  }
  index = digits_at(bytes, *index + 1, 2);
  if (!index.has_value() || *index >= bytes.size() || bytes[*index] != ':') return std::nullopt;
  index = digits_at(bytes, *index + 1, 2);
  if (!index.has_value()) return std::nullopt;
  if (*index < bytes.size() && bytes[*index] == ':') {
    index = digits_at(bytes, *index + 1, 2);
    if (!index.has_value()) return std::nullopt;
    if (*index < bytes.size() && bytes[*index] == '.') {
      std::size_t fraction = *index + 1;
      while (fraction < bytes.size() && std::isdigit(static_cast<unsigned char>(bytes[fraction]))) {
        ++fraction;
      }
      if (fraction > *index + 1) index = fraction;
    }
  }
  if (*index < bytes.size() && bytes[*index] == 'Z') {
    ++*index;
  } else if (*index < bytes.size() && (bytes[*index] == '+' || bytes[*index] == '-')) {
    if (const auto after = digits_at(bytes, *index + 1, 2); after.has_value()) {
      if (*after < bytes.size() && bytes[*after] == ':') {
        if (const auto minutes = digits_at(bytes, *after + 1, 2); minutes.has_value()) {
          index = minutes;
        }
      }
    }
  }
  return index;
}

std::string redact_timestamps(std::string_view text) {
  const std::string_view bytes = text;
  std::string out;
  out.reserve(text.size());
  std::size_t index = 0;
  while (index < bytes.size()) {
    if (const auto end = timestamp_at(bytes, index); end.has_value()) {
      out += "[..]";
      index = *end;
      continue;
    }
    const std::size_t length = codepoint_length(bytes, index);
    out.append(bytes.substr(index, length));
    index += length;
  }
  return out;
}

Json sort_keys_value(const Json& value) {
  if (value.is_array()) {
    Json::Array items;
    for (const Json& item : value.as_array()) items.push_back(sort_keys_value(item));
    return Json::array(std::move(items));
  }
  if (value.is_object()) {
    std::vector<std::pair<std::string, Json>> pairs;
    for (const auto& [key, item] : value.as_object()) {
      pairs.emplace_back(key, sort_keys_value(item));
    }
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const auto& left, const auto& right) { return left.first < right.first; });
    Json::Object fields;
    for (auto& [key, item] : pairs) fields.emplace_back(std::move(key), std::move(item));
    return Json::object(std::move(fields));
  }
  return value;
}

std::string sort_json_keys(std::string_view text) {
  const std::string trimmed = trim_ascii(text);
  Json value;
  try {
    value = parse(trimmed);
  } catch (const JsonError&) {
    return std::string(text);
  }
  return dump(sort_keys_value(value), false);
}

std::string replace_all(std::string text, std::string_view needle, std::string_view replacement) {
  if (needle.empty()) {
    // Rust's `str::replace("", r)` inserts `r` at every character boundary.
    std::string out;
    for (std::size_t index = 0; index < text.size();) {
      out += replacement;
      const unsigned char lead = static_cast<unsigned char>(text[index]);
      std::size_t length = 1;
      if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
      } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
      } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
      }
      out.append(text, index, length);
      index += length;
    }
    out += replacement;
    return out;
  }
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), replacement);
    at += replacement.size();
  }
  return text;
}

std::string sanitize(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (const char ch : name) {
    out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '-';
  }
  return out;
}

std::filesystem::path safe_join(const std::filesystem::path& root, const std::string& relative) {
  // The caller reports the error with Rust's `{:?}` on the relative path.
  if (relative.empty() || relative.front() == '/' ||
      relative.find("..") != std::string::npos) {
    throw std::runtime_error("unsafe path");
  }
  return root / relative;
}

std::string debug_strings(const std::vector<std::string>& items) {
  std::string out = "[";
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index > 0) out += ", ";
    out += quote_debug(items[index]);
  }
  out += ']';
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------

Result<Config> Config::load(const std::filesystem::path& dir) {
  const std::filesystem::path path = dir / "config.json";
  auto text = read_required(path);
  if (!text.has_value()) {
    return std::unexpected(text.error() + " (is this a recorded app? `app record` first)");
  }
  Json doc;
  try {
    doc = parse(*text);
  } catch (const JsonError& error) {
    return std::unexpected(std::string(error.what()));
  }
  Config config;
  config.name = string_of(doc, "name", "app");
  config.run = strings_of(doc, "run");
  if (const Json* timeout = get(doc, "timeout_secs");
      timeout != nullptr && timeout->is_integer()) {
    config.timeout_secs = static_cast<std::uint64_t>(timeout->as_int());
  }
  config.normalize = strings_of(doc, "normalize");
  if (const Json* tolerance = get(doc, "float_tolerance");
      tolerance != nullptr && tolerance->is_number()) {
    config.float_tolerance = tolerance->as_number();
  }
  if (const Json* http = get(doc, "http"); http != nullptr && http->is_bool()) {
    config.http = http->as_bool();
  }
  if (const Json* port = get(doc, "port"); port != nullptr && port->is_integer()) {
    config.port = static_cast<std::uint16_t>(port->as_int());
  }
  config.args = strings_of(doc, "args");
  config.fresh = strings_of(doc, "fresh");
  if (const Json* pace = get(doc, "pace_ms"); pace != nullptr && pace->is_integer()) {
    config.pace_ms = static_cast<std::uint64_t>(pace->as_int());
  }
  config.effects = strings_of(doc, "effects");
  config.effect_env = strings_of(doc, "effect_env");
  if (const Json* reset = get(doc, "db_reset"); reset != nullptr && reset->is_string()) {
    config.db_reset = reset->as_string();
  }
  if (const Json* dump = get(doc, "db_dump"); dump != nullptr && dump->is_string()) {
    config.db_dump = dump->as_string();
  }
  if (const Json* lifted = get(doc, "lifted"); lifted != nullptr && lifted->is_bool()) {
    config.lifted = lifted->as_bool();
  }
  return config;
}

Result<void> Config::save(const std::filesystem::path& dir) const {
  const Json doc = Json::object({
      {"schema", Json::string(std::string(kSchemaConfig))},
      {"name", Json::string(name)},
      {"run", strings_json(run)},
      {"timeout_secs", Json::integer(static_cast<std::int64_t>(timeout_secs))},
      {"normalize", strings_json(normalize)},
      {"float_tolerance", Json::real(float_tolerance)},
      {"http", Json::boolean(http)},
      {"port", port.has_value() ? Json::integer(static_cast<std::int64_t>(*port)) : Json::null()},
      {"args", strings_json(args)},
      {"fresh", strings_json(fresh)},
      {"pace_ms", Json::real(static_cast<double>(pace_ms))},
      {"effects", strings_json(effects)},
      {"effect_env", strings_json(effect_env)},
      {"db_reset", db_reset.has_value() ? Json::string(*db_reset) : Json::null()},
      {"db_dump", db_dump.has_value() ? Json::string(*db_dump) : Json::null()},
      {"lifted", Json::boolean(lifted)},
  });
  return write_file(dir / "config.json", dump(doc, true));
}

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------

Json Case::to_json() const {
  Json::Object env_fields;
  for (const auto& [key, value] : env) env_fields.emplace_back(key, Json::string(value));
  Json::Object file_fields;
  for (const auto& [key, value] : files) file_fields.emplace_back(key, Json::string(value));
  Json request_doc = Json::null();
  if (this->request.has_value()) {
    Json::Object headers;
    for (const auto& [key, value] : this->request->headers) {
      headers.emplace_back(key, Json::string(value));
    }
    request_doc = Json::object({
        {"method", Json::string(this->request->method)},
        {"path", Json::string(this->request->path)},
        {"headers", Json::object(std::move(headers))},
        {"body", Json::string(this->request->body)},
    });
  }
  Json::Object expect_files;
  for (const auto& [key, value] : expect.files) expect_files.emplace_back(key, Json::string(value));
  Json http = Json::null();
  if (expect.http.has_value()) {
    http = Json::object({
        {"status", Json::integer(static_cast<std::int64_t>(expect.http->status))},
        {"content_type", Json::string(expect.http->content_type)},
        {"body", Json::string(expect.http->body)},
    });
  }
  Json::Array effect_lines;
  for (const auto& line : expect.effects) effect_lines.push_back(Json::string(line));
  const Json expect_doc = Json::object({
      {"stdout", Json::string(expect.stdout)},
      {"stderr", Json::string(expect.stderr)},
      {"exit", Json::integer(static_cast<std::int64_t>(expect.exit))},
      {"files", Json::object(std::move(expect_files))},
      {"fallback", Json::boolean(expect.fallback)},
      {"http", std::move(http)},
      {"effects", Json::array(std::move(effect_lines))},
  });
  return Json::object({
      {"name", Json::string(name)},
      {"args", strings_json(args)},
      {"stdin", Json::string(stdin)},
      {"env", Json::object(std::move(env_fields))},
      {"files", Json::object(std::move(file_fields))},
      {"request", std::move(request_doc)},
      {"expect", expect_doc},
      {"unstable", Json::boolean(unstable)},
      {"source", Json::string(source)},
  });
}

Result<Case> Case::parse(const Json& doc) {
  Case item;
  const Json* request_doc = get(doc, "request");
  if (request_doc != nullptr && request_doc->is_object()) {
    HttpRequest request;
    request.method = string_of(*request_doc, "method", "GET");
    request.path = string_of(*request_doc, "path", "/");
    request.headers = pairs_of(*request_doc, "headers");
    request.body = string_of(*request_doc, "body", "");
    item.request = std::move(request);
  }
  item.name = string_of(doc, "name", "case");
  item.args = strings_of(doc, "args");
  item.stdin = string_of(doc, "stdin", "");
  item.env = pairs_of(doc, "env");
  item.files = pairs_of(doc, "files");
  const Json* expect_doc = get(doc, "expect");
  const Json null_expect;
  const Json& expect = expect_doc != nullptr ? *expect_doc : null_expect;
  const Json* http_doc = get(expect, "http");
  if (http_doc != nullptr && http_doc->is_object()) {
    HttpExpect http;
    if (const Json* status = get(*http_doc, "status");
        status != nullptr && status->is_integer()) {
      http.status = static_cast<std::uint16_t>(status->as_int());
    }
    http.content_type = string_of(*http_doc, "content_type", "");
    http.body = string_of(*http_doc, "body", "");
    item.expect.http = std::move(http);
  }
  item.expect.stdout = string_of(expect, "stdout", "");
  item.expect.stderr = string_of(expect, "stderr", "");
  if (const Json* exit = get(expect, "exit"); exit != nullptr && exit->is_integer()) {
    item.expect.exit = static_cast<int>(exit->as_int());
  }
  item.expect.files = pairs_of(expect, "files");
  if (const Json* fallback = get(expect, "fallback");
      fallback != nullptr && fallback->is_bool()) {
    item.expect.fallback = fallback->as_bool();
  }
  item.expect.effects = strings_of(expect, "effects");
  if (const Json* unstable = get(doc, "unstable"); unstable != nullptr && unstable->is_bool()) {
    item.unstable = unstable->as_bool();
  }
  item.source = string_of(doc, "source", "");
  return item;
}

std::filesystem::path cases_path(const std::filesystem::path& repo) {
  return repo / kCasesDir / "cases.jsonl";
}

Result<std::vector<Case>> load_cases(const std::filesystem::path& repo) {
  const std::filesystem::path path = cases_path(repo);
  auto text = read_required(path);
  if (!text.has_value()) {
    return std::unexpected(text.error() + " (run `cpp-anything app record` first)");
  }
  std::vector<Case> cases;
  for (const std::string_view line : rust_lines(*text)) {
    if (trim_ascii(line).empty()) continue;
    Json doc;
    try {
      doc = parse(line);
    } catch (const JsonError& error) {
      return std::unexpected(std::string(error.what()));
    }
    auto item = Case::parse(doc);
    if (!item.has_value()) return std::unexpected(item.error());
    cases.push_back(std::move(*item));
  }
  if (cases.empty()) return std::unexpected(path_text(path) + ": no cases");
  return cases;
}

Result<void> save_cases(const std::filesystem::path& repo, const std::vector<Case>& cases) {
  std::string text;
  for (const Case& item : cases) {
    text += dump(item.to_json(), false);
    text += '\n';
  }
  return write_file(cases_path(repo), text);
}

// ---------------------------------------------------------------------------
// running
// ---------------------------------------------------------------------------

std::vector<std::string> resolve_command(const std::filesystem::path& repo,
                                         const std::vector<std::string>& command) {
  std::vector<std::string> out;
  out.reserve(command.size());
  for (const std::string& word : command) {
    if (!word.empty() && word.front() == '-') {
      out.push_back(word);
      continue;
    }
    const std::filesystem::path candidate = repo / word;
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) {
      out.push_back(canonical_or_self(candidate).string());
      continue;
    }
    const std::filesystem::path direct(word);
    if (std::filesystem::exists(direct, error)) {
      out.push_back(canonical_or_self(direct).string());
      continue;
    }
    out.push_back(word);
  }
  return out;
}

Result<Observation> observe(const Case& item, const RunOptions& options) {
  std::error_code error;
  std::filesystem::path temp = std::filesystem::temp_directory_path(error);
  if (error) temp = "/tmp";
  const std::filesystem::path sandbox =
      temp / "cpp-anything" / "app-run" /
      (std::to_string(static_cast<long long>(::getpid())) + "-" + sanitize(item.name));
  std::filesystem::remove_all(sandbox, error);
  const std::filesystem::path cwd = sandbox / "cwd";
  std::filesystem::create_directories(cwd, error);
  if (error) return std::unexpected(path_text(cwd) + ": " + io_error(error.value()));
  for (const auto& [relative, contents] : item.files) {
    std::filesystem::path path;
    try {
      path = safe_join(cwd, relative);
    } catch (const std::runtime_error&) {
      return std::unexpected(quote_debug(relative) +
                             ": case files must be relative paths inside the sandbox");
    }
    if (auto written = write_file(path, contents); !written.has_value()) {
      return std::unexpected(written.error());
    }
  }

  const std::vector<std::string> resolved = resolve_command(options.repo, options.command);
  std::vector<std::string> args;
  std::string program = resolved.empty() ? std::string() : resolved.front();
  if (program.empty()) {
    return std::unexpected(
        "no command to run: pass `-- <command>` or record with one");
  }
  if (options.timeout > 0 && which_on_path("timeout").has_value()) {
    // `timeout` gives process-group killing without a dependency; without it,
    // a hung app hangs the run (documented in docs/how-it-works.md).
    args.push_back(std::to_string(options.timeout) + "s");
    args.push_back(program);
    program = "timeout";
  }
  for (std::size_t index = 1; index < resolved.size(); ++index) args.push_back(resolved[index]);
  args.insert(args.end(), item.args.begin(), item.args.end());

  const std::filesystem::path fallback_log = sandbox / "fallback.log";
  std::vector<std::pair<std::string, std::string>> envs = options.extra_env;
  envs.insert(envs.end(), item.env.begin(), item.env.end());
  envs.emplace_back("CPPAN_FALLBACK_LOG", path_text(fallback_log));
  const std::filesystem::path stdin_path = sandbox / "stdin.txt";
  if (auto written = write_file(stdin_path, item.stdin); !written.has_value()) {
    return std::unexpected(written.error());
  }
  // Redirect stdin from a file without a shell: `sh -c 'exec "$@" < file'`.
  std::vector<std::string> shell = {"-c", "exec \"$@\" < \"$CPPAN_STDIN\"", "cppan-app", program};
  shell.insert(shell.end(), args.begin(), args.end());
  envs.emplace_back("CPPAN_STDIN", path_text(stdin_path));

  if (options.pre_command.has_value()) {
    (void)run_command("sh", {"-c", *options.pre_command}, cwd);
  }
  const Run run = run_command("sh", shell, cwd, envs);
  std::vector<std::string> database;
  if (options.post_command.has_value()) {
    const Run dumped = run_command("sh", {"-c", *options.post_command}, cwd);
    const std::string text = dumped.out + dumped.err;
    for (const std::string_view line : rust_lines(text)) {
      if (trim_ascii(line).empty()) continue;
      database.push_back("db " + std::string(line));
    }
  }

  std::vector<std::pair<std::string, std::string>> files;
  std::function<Result<void>(const std::filesystem::path&)> walk =
      [&](const std::filesystem::path& directory) -> Result<void> {
    std::error_code walk_error;
    std::filesystem::directory_iterator iterator(directory, walk_error);
    if (walk_error) {
      return std::unexpected(path_text(directory) + ": " + io_error(walk_error.value()));
    }
    for (const auto& entry : iterator) {
      const std::filesystem::path path = entry.path();
      std::error_code kind_error;
      if (entry.is_directory(kind_error)) {
        auto nested = walk(path);
        if (!nested.has_value()) return nested;
        continue;
      }
      auto relative = std::filesystem::relative(path, cwd, kind_error);
      if (kind_error) return std::unexpected("path outside the sandbox");
      std::string name = relative.generic_string();
      const auto bytes = read_text(path);
      if (!bytes.has_value()) {
        return std::unexpected(path_text(path) + ": " + io_error(ENOENT));
      }
      const std::string text = lossy(*bytes);
      const bool unchanged =
          std::any_of(item.files.begin(), item.files.end(),
                      [&](const auto& given) { return given.first == name && given.second == text; });
      if (!unchanged) files.emplace_back(name, text);
    }
    return {};
  };
  if (auto collected = walk(cwd); !collected.has_value()) return std::unexpected(collected.error());
  std::sort(files.begin(), files.end());

  bool fallback = false;
  if (const auto log = read_text(fallback_log); log.has_value()) {
    fallback = !trim_ascii(*log).empty();
  }
  std::filesystem::remove_all(sandbox, error);
  Observation observation;
  observation.stdout_text = run.out;
  observation.stderr_text = run.err;
  observation.exit = run.status.value_or(-1);
  observation.files = std::move(files);
  observation.fallback = fallback;
  observation.database = std::move(database);
  return observation;
}

// ---------------------------------------------------------------------------
// normalisation: timestamps, uuids, json key order
// ---------------------------------------------------------------------------

std::string normalize(std::string_view text, const std::vector<std::string>& rules) {
  std::string out(text);
  for (const std::string& rule : rules) {
    if (rule == "timestamps") {
      out = redact_timestamps(out);
    } else if (rule == "uuids") {
      out = redact_uuids(out);
    } else if (rule == "key-order") {
      out = sort_json_keys(out);
    } else {
      const std::size_t equals = rule.find('=');
      if (equals != std::string::npos) {
        out = replace_all(std::move(out), rule.substr(equals + 1), "[..]");
      }
    }
  }
  return out;
}

Case settle(const std::string& name, const std::string& source, Observation first,
            Observation second, const std::vector<std::string>& rules) {
  const bool same = first.stdout_text == second.stdout_text &&
                    first.stderr_text == second.stderr_text && first.exit == second.exit &&
                    first.files == second.files;
  Case item;
  item.name = name;
  item.expect.http = std::nullopt;
  item.expect.stdout = normalize(first.stdout_text, rules);
  item.expect.stderr = normalize(first.stderr_text, rules);
  item.expect.exit = first.exit;
  std::map<std::string, std::string> files;
  for (const auto& [path, contents] : first.files) {
    files.emplace(path, normalize(contents, rules));
  }
  item.expect.files.assign(files.begin(), files.end());
  item.expect.fallback = first.fallback;
  item.expect.effects = first.effects;
  item.unstable = !same;
  item.source = source;
  return item;
}

std::optional<std::string> compare(const Case& item, const Observation& observed,
                                   const std::vector<std::string>& rules) {
  if (item.unstable) return std::nullopt;
  const std::string stdout_text = normalize(observed.stdout_text, rules);
  if (stdout_text != item.expect.stdout) {
    return "stdout: expected " + quote_debug(truncate_text(item.expect.stdout, 120)) + ", got " +
           quote_debug(truncate_text(stdout_text, 120));
  }
  const std::string stderr_text = normalize(observed.stderr_text, rules);
  if (stderr_text != item.expect.stderr) {
    return "stderr: expected " + quote_debug(truncate_text(item.expect.stderr, 120)) + ", got " +
           quote_debug(truncate_text(stderr_text, 120));
  }
  if (observed.exit != item.expect.exit) {
    return "exit: expected " + std::to_string(item.expect.exit) + ", got " +
           std::to_string(observed.exit);
  }
  std::map<std::string, std::string> files;
  for (const auto& [path, contents] : item.expect.files) {
    files.emplace(path, normalize(contents, rules));
  }
  std::map<std::string, std::string> observed_files;
  for (const auto& [path, contents] : observed.files) {
    observed_files.emplace(path, normalize(contents, rules));
  }
  if (files != observed_files) {
    std::vector<std::string> missing;
    std::vector<std::string> extra;
    for (const auto& [path, _] : files) {
      if (!observed_files.contains(path)) missing.push_back(path);
    }
    for (const auto& [path, _] : observed_files) {
      if (!files.contains(path)) extra.push_back(path);
    }
    if (!missing.empty() || !extra.empty()) {
      return "files: missing " + debug_strings(missing) + ", extra " + debug_strings(extra);
    }
    return "files: contents differ";
  }
  return std::nullopt;
}

}  // namespace cppan::appcase
