// `app`: record a CLI black-box, replay any build against it, and put a C++ front
// door in front of the original so the app can ship before every unit is ported.
module;
#include <cctype>
#include <cerrno>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

module cppan.app;
import cppan.types;
import cppan.appcase;
import cppan.appfront;
import cppan.apphttp;
import cppan.json;
import cppan.util;
import std;

namespace cppan::app {
namespace {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

std::string io_error(int code) {
  return std::string(std::strerror(code)) + " (os error " + std::to_string(code) + ")";
}

Result<std::string> read_required(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return std::unexpected(path.string() + ": " + io_error(errno));
  std::string out;
  char buffer[8192];
  while (true) {
    const ssize_t got = ::read(fd, buffer, sizeof buffer);
    if (got < 0) {
      if (errno == EINTR) continue;
      const int code = errno;
      ::close(fd);
      return std::unexpected(path.string() + ": " + io_error(code));
    }
    if (got == 0) break;
    out.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  return out;
}

std::string trim_ascii(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

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

std::string path_text(const std::filesystem::path& path) { return path.string(); }

std::string debug_strings(const std::vector<std::string>& items) {
  std::string out = "[";
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index > 0) out += ", ";
    out += quote_debug(items[index]);
  }
  out += ']';
  return out;
}

Json strings_json(const std::vector<std::string>& items) {
  Json::Array array;
  for (const auto& item : items) array.push_back(Json::string(item));
  return Json::array(std::move(array));
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

std::string sanitize(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (const char ch : name) {
    out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '-';
  }
  return out;
}

std::string repo_name(const std::filesystem::path& repo) {
  const std::string name = path_text(repo.filename());
  if (name.empty() || name == "." || name == "..") return "app";
  return name;
}

// ---------------------------------------------------------------------------
// side effects: local HTTP and SMTP sinks
// ---------------------------------------------------------------------------

bool read_line_fd(int fd, std::string& line) {
  line.clear();
  while (true) {
    char ch = 0;
    const ssize_t got = ::recv(fd, &ch, 1, 0);
    if (got <= 0) {
      if (got < 0 && errno == EINTR) continue;
      return false;
    }
    line += ch;
    if (ch == '\n') return true;
  }
}

bool write_all_fd(int fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t wrote = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (wrote <= 0) {
      if (wrote < 0 && errno == EINTR) continue;
      return false;
    }
    sent += static_cast<std::size_t>(wrote);
  }
  return true;
}

struct Listener {
  int fd = -1;
  std::uint16_t port = 0;
};

Result<Listener> open_listener() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return std::unexpected("bind: " + io_error(errno));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    const int code = errno;
    ::close(fd);
    return std::unexpected("bind: " + io_error(code));
  }
  socklen_t length = sizeof address;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    const int code = errno;
    ::close(fd);
    return std::unexpected(io_error(code));
  }
  if (::listen(fd, 64) != 0) {
    const int code = errno;
    ::close(fd);
    return std::unexpected("listen: " + io_error(code));
  }
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return Listener{fd, ntohs(address.sin_port)};
}

class HttpSink {
 public:
  static Result<std::unique_ptr<HttpSink>> start() {
    auto listener = open_listener();
    if (!listener.has_value()) return std::unexpected(listener.error());
    auto sink = std::unique_ptr<HttpSink>(new HttpSink());
    sink->listener_ = listener->fd;
    sink->port_ = listener->port;
    sink->worker_ = std::thread([raw = sink.get()] { raw->serve(); });
    return sink;
  }

  ~HttpSink() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    if (listener_ >= 0) ::close(listener_);
  }

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  std::vector<std::string> take() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    return log_;
  }

 private:
  HttpSink() = default;

  void serve() {
    while (!stop_) {
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      const int flags = ::fcntl(client, F_GETFL);
      if (flags >= 0) (void)::fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
      handle(client);
      ::close(client);
    }
  }

  void handle(int client) {
    std::string request_line;
    if (!read_line_fd(client, request_line)) return;
    std::size_t content_length = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    while (true) {
      std::string line;
      if (!read_line_fd(client, line) || trim_ascii(line).empty()) break;
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = trim_ascii(std::string_view(line).substr(0, colon));
      for (char& ch : key) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      }
      const std::string value = trim_ascii(std::string_view(line).substr(colon + 1));
      if (key == "content-length") {
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                            content_length);
        if (parsed.ec != std::errc{}) content_length = 0;
      }
      headers.emplace_back(key, value);
    }
    std::size_t wanted = std::min<std::size_t>(content_length, 1U << 20);
    std::string body;
    body.reserve(wanted);
    char buffer[8192];
    while (body.size() < wanted) {
      const std::size_t want = std::min<std::size_t>(sizeof buffer, wanted - body.size());
      const ssize_t got = ::recv(client, buffer, want, 0);
      if (got <= 0) break;
      body.append(buffer, static_cast<std::size_t>(got));
    }
    std::string host;
    for (const auto& [key, value] : headers) {
      if (key == "host") {
        host = value;
        break;
      }
    }
    const std::string line = "http " + trim_ascii(request_line) + " " + host + " " + body;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      log_.push_back(line);
    }
    (void)write_all_fd(client,
                       "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  }

  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::mutex mutex_;
  std::vector<std::string> log_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

class SmtpSink {
 public:
  static Result<std::unique_ptr<SmtpSink>> start() {
    auto listener = open_listener();
    if (!listener.has_value()) return std::unexpected(listener.error());
    auto sink = std::unique_ptr<SmtpSink>(new SmtpSink());
    sink->listener_ = listener->fd;
    sink->port_ = listener->port;
    sink->worker_ = std::thread([raw = sink.get()] { raw->serve(); });
    return sink;
  }

  ~SmtpSink() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    if (listener_ >= 0) ::close(listener_);
  }

  [[nodiscard]] std::uint16_t port() const { return port_; }

  std::vector<std::string> take() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    return log_;
  }

 private:
  SmtpSink() = default;

  void serve() {
    while (!stop_) {
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      const int flags = ::fcntl(client, F_GETFL);
      if (flags >= 0) (void)::fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
      handle(client);
      ::close(client);
    }
  }

  static std::string trim_envelope(std::string_view text) {
    std::string out(text);
    while (!out.empty() && (out.front() == '<' || out.front() == '>')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '<' || out.back() == '>')) out.pop_back();
    return out;
  }

  void handle(int client) {
    (void)write_all_fd(client, "220 cppan-sink ESMTP\r\n");
    std::string from;
    std::vector<std::string> to;
    bool in_data = false;
    std::string message;
    std::string line;
    while (read_line_fd(client, line)) {
      std::string trimmed = line;
      while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n')) {
        trimmed.pop_back();
      }
      if (in_data) {
        if (trimmed == ".") {
          in_data = false;
          std::string joined;
          for (std::size_t index = 0; index < to.size(); ++index) {
            if (index > 0) joined += ",";
            joined += to[index];
          }
          std::lock_guard<std::mutex> lock(mutex_);
          log_.push_back("smtp from=" + from + " to=" + joined + " " + message);
          (void)write_all_fd(client, "250 2.0.0 queued\r\n");
          continue;
        }
        message += line;
        continue;
      }
      std::string upper = trimmed;
      for (char& ch : upper) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
      if (upper.starts_with("EHLO") || upper.starts_with("HELO")) {
        (void)write_all_fd(client, "250 cppan-sink\r\n");
      } else if (upper.starts_with("MAIL FROM:")) {
        from = trim_envelope(trim_ascii(std::string_view(trimmed).substr(10)));
        (void)write_all_fd(client, "250 2.1.0 ok\r\n");
      } else if (upper.starts_with("RCPT TO:")) {
        to.push_back(trim_envelope(trim_ascii(std::string_view(trimmed).substr(8))));
        (void)write_all_fd(client, "250 2.1.5 ok\r\n");
      } else if (upper == "DATA") {
        in_data = true;
        message.clear();
        (void)write_all_fd(client, "354 end with <CRLF>.<CRLF>\r\n");
      } else if (upper == "QUIT") {
        (void)write_all_fd(client, "221 bye\r\n");
        break;
      } else {
        (void)write_all_fd(client, "250 ok\r\n");
      }
    }
  }

  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::mutex mutex_;
  std::vector<std::string> log_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

struct Sinks {
  std::unique_ptr<HttpSink> http;
  std::unique_ptr<SmtpSink> smtp;

  static Result<Sinks> start(const std::vector<std::string>& kinds) {
    Sinks sinks;
    for (const std::string& kind : kinds) {
      if (kind == "http") {
        if (sinks.http == nullptr) {
          auto started = HttpSink::start();
          if (!started.has_value()) return std::unexpected(started.error());
          sinks.http = std::move(*started);
        }
      } else if (kind == "smtp") {
        if (sinks.smtp == nullptr) {
          auto started = SmtpSink::start();
          if (!started.has_value()) return std::unexpected(started.error());
          sinks.smtp = std::move(*started);
        }
      } else {
        return std::unexpected("unknown effect " + quote_debug(kind) + ": use http or smtp");
      }
    }
    return sinks;
  }

  /// Environment that points the app's own settings at the sinks.
  std::vector<std::pair<std::string, std::string>> env(
      const std::vector<std::pair<std::string, std::string>>& extra) const {
    std::vector<std::pair<std::string, std::string>> envs;
    if (http != nullptr) {
      const std::string url = http->url();
      for (const char* key : {"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
                              "CPPAN_HTTP_SINK"}) {
        envs.emplace_back(key, url);
      }
    }
    if (smtp != nullptr) {
      envs.emplace_back("SMTP_HOST", "127.0.0.1");
      envs.emplace_back("SMTP_PORT", std::to_string(smtp->port()));
      envs.emplace_back("CPPAN_SMTP", "127.0.0.1:" + std::to_string(smtp->port()));
    }
    envs.insert(envs.end(), extra.begin(), extra.end());
    return envs;
  }

  std::vector<std::string> ports() const {
    std::vector<std::string> out;
    if (http != nullptr) out.push_back(std::to_string(http->port()));
    if (smtp != nullptr) out.push_back(std::to_string(smtp->port()));
    return out;
  }

  std::vector<std::string> take() {
    std::vector<std::string> lines;
    if (http != nullptr) {
      const auto taken = http->take();
      lines.insert(lines.end(), taken.begin(), taken.end());
    }
    if (smtp != nullptr) {
      const auto taken = smtp->take();
      lines.insert(lines.end(), taken.begin(), taken.end());
    }
    return lines;
  }
};

/// `--effect-env VAR=kind`: point one of the app's own settings at a sink.
std::vector<std::pair<std::string, std::string>> resolved_effect_env(
    const Sinks& sinks, const std::vector<std::string>& specs) {
  std::vector<std::pair<std::string, std::string>> env;
  for (const std::string& spec : specs) {
    const std::size_t equals = spec.find('=');
    if (equals == std::string::npos) continue;
    const std::string name = spec.substr(0, equals);
    const std::string kind = spec.substr(equals + 1);
    std::optional<std::string> value;
    if (kind == "http-url") {
      if (sinks.http != nullptr) value = sinks.http->url();
    } else if (kind == "smtp-host") {
      if (sinks.smtp != nullptr) value = "127.0.0.1";
    } else if (kind == "smtp-port") {
      if (sinks.smtp != nullptr) value = std::to_string(sinks.smtp->port());
    } else if (kind == "smtp-url") {
      if (sinks.smtp != nullptr) value = "smtp://127.0.0.1:" + std::to_string(sinks.smtp->port());
    }
    if (value.has_value()) env.emplace_back(name, *value);
  }
  return env;
}

/// The sink's ephemeral port is not part of the app's behaviour: mask it before
/// storing or comparing an effect line.
std::string mask_ports(const std::string& line, const std::vector<std::string>& ports) {
  std::string out = line;
  for (const std::string& port : ports) {
    const auto replace = [&](std::string_view from, std::string_view to) {
      std::size_t at = 0;
      while ((at = out.find(from, at)) != std::string::npos) {
        out.replace(at, from.size(), to);
        at += to.size();
      }
    };
    replace("127.0.0.1:" + port, "127.0.0.1:<sink>");
    replace("localhost:" + port, "localhost:<sink>");
  }
  return out;
}

// ---------------------------------------------------------------------------
// the parsed view the subcommands share
// ---------------------------------------------------------------------------

class ParsedView {
 public:
  ParsedView(const cli::Parsed& args, std::size_t skip) : args_(args), skip_(skip) {}

  [[nodiscard]] std::optional<std::string> positional(std::size_t index) const {
    if (index + skip_ >= args_.positional.size()) return std::nullopt;
    return args_.positional[index + skip_];
  }
  [[nodiscard]] std::optional<std::string> value(std::string_view name) const {
    return args_.value(name);
  }
  [[nodiscard]] bool flag(std::string_view name) const { return args_.flag(name); }
  [[nodiscard]] std::optional<std::uint64_t> number(std::string_view name) const {
    return args_.number(name);
  }
  [[nodiscard]] std::vector<std::string> values(std::string_view name) const {
    return args_.many(name);
  }
  [[nodiscard]] const std::vector<std::string>& trailing() const { return args_.trailing; }

 private:
  const cli::Parsed& args_;
  std::size_t skip_;
};

std::filesystem::path repo_of(const ParsedView& view) {
  if (const auto positional = view.positional(0); positional.has_value()) {
    return std::filesystem::path(*positional);
  }
  return std::filesystem::path(".");
}

// ---------------------------------------------------------------------------
// record
// ---------------------------------------------------------------------------

std::string label_of(const std::string& source) {
  if (source == "docs") return "example";
  if (source == "inputs") return "input";
  if (source == "trap") return "trap";
  return source;
}

std::string slug_of(std::string_view text) {
  std::string slug;
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if ((lead & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4;
    }
    if (lead < 0x80 && std::isalnum(lead)) {
      slug += static_cast<char>(std::tolower(lead));
    } else {
      slug += '-';
    }
    index += length;
  }
  std::size_t begin = 0;
  std::size_t end = slug.size();
  while (begin < end && slug[begin] == '-') ++begin;
  while (end > begin && slug[end - 1] == '-') --end;
  const std::string trimmed = slug.substr(begin, end - begin);
  if (trimmed.empty()) return "no-args";
  return trimmed.substr(0, 40);
}

const std::vector<std::string> kIntTraps = {"-7", "0", "-1", "2147483648", "9007199254740993",
                                            "9223372036854775807"};
const std::vector<std::string> kFloatTraps = {"-0.0", "0.1", "1e21", "1e-7", "5e-324",
                                              "1.7976931348623157e308"};
const std::vector<std::string> kStringTraps = {"héllo wörld", "👍🏽 thumbs", "日本語", "",
                                               "İstanbul",     "straße",       "👨‍👩‍👧 family",
                                               std::string("a\xC2\xA0") + "b"};
const std::vector<std::string> kStdinTraps = {"", "naïve café 👍🏽\nsecond line\n",
                                              "  spaced   out  \n\n"};

bool parse_f64_ok(const std::string& text) {
  if (text.empty()) return false;
  if (std::isspace(static_cast<unsigned char>(text.front()))) return false;
  char* end = nullptr;
  errno = 0;
  (void)std::strtod(text.c_str(), &end);
  return end == text.c_str() + text.size();
}

bool parse_i128_ok(const std::string& text) {
  std::size_t index = 0;
  bool negative = false;
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
    negative = text[index] == '-';
    ++index;
  }
  if (index == text.size()) return false;
  std::string digits;
  for (; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch < '0' || ch > '9') return false;
    if (digits.empty() && ch == '0') continue;
    digits += ch;
  }
  if (digits.empty()) return true;
  const std::string limit = negative ? "170141183460469231731687303715884105728"
                                     : "170141183460469231731687303715884105727";
  if (digits.size() != limit.size()) return digits.size() < limit.size();
  return digits <= limit;
}

std::vector<std::pair<std::string, appcase::Case>> trap_cases(const appcase::Case& item) {
  std::vector<std::pair<std::string, appcase::Case>> out;
  for (std::size_t index = 0; index < item.args.size(); ++index) {
    const std::string& arg = item.args[index];
    if (!arg.empty() && arg.front() == '-' && !parse_f64_ok(arg)) continue;  // a flag
    std::vector<std::string> values;
    if (parse_i128_ok(arg)) {
      values = kIntTraps;
    } else if (arg.find('.') != std::string::npos && parse_f64_ok(arg)) {
      values = kFloatTraps;
    } else {
      values = kStringTraps;
    }
    for (const std::string& value : values) {
      if (value == arg) continue;
      appcase::Case clone = item;
      clone.args[index] = value;
      clone.source = "trap";
      out.emplace_back("trap", std::move(clone));
    }
  }
  for (const std::string& stdin_text : kStdinTraps) {
    if (stdin_text == item.stdin) continue;
    appcase::Case clone = item;
    clone.stdin = stdin_text;
    clone.source = "trap";
    out.emplace_back("trap", std::move(clone));
  }
  return out;
}

// ---------------------------------------------------------------------------
// discovery: docs examples and --inputs
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> walk_docs(const std::filesystem::path& directory,
                                             std::size_t depth) {
  if (depth > 4) return {};
  const std::vector<std::string> skip = {".git",      "node_modules", "target", "vendor",
                                         "dist",      "build",        "cpp",    "out",
                                         ".venv",     "venv",         "__pycache__"};
  std::vector<std::filesystem::path> out;
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) return out;
  for (const auto& entry : iterator) {
    const std::filesystem::path path = entry.path();
    const std::string name = path.filename().string();
    std::error_code kind_error;
    if (std::filesystem::is_directory(path, kind_error)) {
      if (std::find(skip.begin(), skip.end(), name) != skip.end() ||
          (!name.empty() && name.front() == '.')) {
        continue;
      }
      auto nested = walk_docs(path, depth + 1);
      out.insert(out.end(), nested.begin(), nested.end());
      continue;
    }
    const std::string extension = path.extension().string();
    if (extension == ".md" || extension == ".markdown" || extension == ".sh") {
      out.push_back(path);
    }
  }
  return out;
}

std::vector<std::string> commands_in(std::string_view text, bool shell_file) {
  std::vector<std::string> out;
  bool in_fence = false;
  bool fence_shell = false;
  for (const std::string_view line : rust_lines(text)) {
    const std::string trimmed = trim_ascii(line);
    if (trimmed.rfind("```", 0) == 0) {
      if (in_fence) {
        in_fence = false;
        fence_shell = false;
      } else {
        in_fence = true;
        const std::string info = trim_ascii(std::string_view(trimmed).substr(3));
        fence_shell =
            info == "sh" || info == "bash" || info == "shell" || info == "zsh";
      }
      continue;
    }
    std::string command;
    if (shell_file || (in_fence && fence_shell)) {
      command = trimmed.starts_with("$ ") ? trimmed.substr(2) : trimmed;
    } else {
      if (!trimmed.starts_with("$ ")) continue;
      command = trimmed.substr(2);
    }
    if (command.empty() || command.front() == '#') continue;
    if (command.find("$(") != std::string::npos || command.find('`') != std::string::npos ||
        command.find('|') != std::string::npos || command.find('>') != std::string::npos ||
        command.find('<') != std::string::npos || command.find("&&") != std::string::npos ||
        command.find(';') != std::string::npos) {
      continue;
    }
    out.push_back(command);
  }
  return out;
}

std::optional<std::vector<std::string>> tokenize(const std::string& command) {
  std::vector<std::string> out;
  std::string current;
  std::optional<char> quote;
  std::size_t index = 0;
  while (index < command.size()) {
    const char ch = command[index++];
    if (quote.has_value()) {
      if (ch == *quote) {
        quote.reset();
      } else if (ch == '\\') {
        if (index < command.size()) current.push_back(command[index++]);
      } else {
        current.push_back(ch);
      }
      continue;
    }
    switch (ch) {
      case '"':
      case '\'':
        quote = ch;
        break;
      case '\\':
        if (index < command.size()) current.push_back(command[index++]);
        break;
      case ' ':
      case '\t':
        if (!current.empty()) {
          out.push_back(current);
          current.clear();
        }
        break;
      default:
        current.push_back(ch);
        break;
    }
  }
  if (quote.has_value()) return std::nullopt;
  if (!current.empty()) out.push_back(std::move(current));
  if (out.empty()) return std::nullopt;
  return out;
}

std::optional<appcase::Case> case_from_command(const std::string& command,
                                               const std::string& app) {
  const auto words_option = tokenize(command);
  if (!words_option.has_value()) return std::nullopt;
  const std::vector<std::string>& words = *words_option;
  std::vector<std::pair<std::string, std::string>> env;
  std::size_t index = 0;
  while (index < words.size()) {
    const std::size_t equals = words[index].find('=');
    if (equals != std::string::npos) {
      const std::string key = words[index].substr(0, equals);
      const bool valid = !key.empty() &&
                         std::all_of(key.begin(), key.end(), [](char ch) {
                           return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
                         });
      if (valid) {
        env.emplace_back(key, words[index].substr(equals + 1));
        ++index;
        continue;
      }
    }
    break;
  }
  if (index >= words.size()) return std::nullopt;
  const std::string& program = words[index];
  const std::string stem = std::filesystem::path(program).stem().string();
  if (stem.empty()) return std::nullopt;
  if (!app.empty() && stem != app) return std::nullopt;
  appcase::Case item;
  item.args.assign(words.begin() + static_cast<std::ptrdiff_t>(index + 1), words.end());
  item.env = std::move(env);
  item.source = "docs";
  return item;
}

Result<std::vector<std::pair<std::string, appcase::Case>>> read_inputs(
    const std::filesystem::path& path) {
  auto text = read_required(path);
  if (!text.has_value()) {
    return std::unexpected(text.error());
  }
  std::vector<std::pair<std::string, appcase::Case>> out;
  std::vector<std::string_view> lines = rust_lines(*text);
  for (std::size_t number = 0; number < lines.size(); ++number) {
    const std::string_view line = lines[number];
    if (trim_ascii(line).empty()) continue;
    Json doc;
    try {
      doc = parse(line);
    } catch (const JsonError& error) {
      return std::unexpected(path_text(path) + ":" + std::to_string(number + 1) + ": " +
                             error.what());
    }
    appcase::Case item;
    item.name = string_of(doc, "name", "");
    item.args = strings_of(doc, "args");
    item.stdin = string_of(doc, "stdin", "");
    item.env = pairs_of(doc, "env");
    item.files = pairs_of(doc, "files");
    item.source = "inputs";
    out.emplace_back("inputs", std::move(item));
  }
  return out;
}

struct Discovered {
  std::vector<std::pair<std::string, appcase::Case>> examples;
  std::vector<std::pair<std::string, appcase::Case>> inputs;
};

Result<Discovered> discover_cases(const std::filesystem::path& repo,
                                  const appcase::Config& config) {
  Discovered found;
  for (const std::filesystem::path& path : walk_docs(repo, 0)) {
    const std::string text = read_file(path).value_or(std::string());
    const bool is_shell = path.extension() == ".sh";
    for (const std::string& line : commands_in(text, is_shell)) {
      auto item = case_from_command(line, config.name);
      if (!item.has_value()) continue;
      found.examples.emplace_back("docs", std::move(*item));
    }
  }
  if (const char* inputs = std::getenv("CPPAN_APP_INPUTS"); inputs != nullptr) {
    auto read = read_inputs(std::filesystem::path(inputs));
    if (!read.has_value()) return std::unexpected(read.error());
    found.inputs = std::move(*read);
  }
  return found;
}

std::optional<std::string> script_name(const std::string& text) {
  // `[project.scripts]` or `"bin": { "name": ... }` without a TOML/JSON dependency:
  // find the first `name =` / `"name":` after the marker.
  const std::vector<std::string_view> lines = rust_lines(text);
  std::size_t index = 0;
  while (index < lines.size()) {
    const std::string trimmed = trim_ascii(lines[index]);
    if (trimmed == "[project.scripts]" || trimmed == "[tool.poetry.scripts]" ||
        trimmed.find("\"bin\"") != std::string::npos) {
      ++index;
      for (std::size_t taken = 0; taken < 4 && index < lines.size(); ++taken, ++index) {
        const std::string candidate = trim_ascii(lines[index]);
        const auto clean = [](std::string key) {
          std::size_t begin = 0;
          std::size_t end = key.size();
          while (begin < end && std::isspace(static_cast<unsigned char>(key[begin]))) ++begin;
          while (end > begin && std::isspace(static_cast<unsigned char>(key[end - 1]))) --end;
          key = key.substr(begin, end - begin);
          while (!key.empty() && key.front() == '"') key.erase(key.begin());
          while (!key.empty() && key.back() == '"') key.pop_back();
          return key;
        };
        if (const std::size_t equals = candidate.find('='); equals != std::string::npos) {
          const std::string key = clean(candidate.substr(0, equals));
          if (!key.empty() && key != "}") return key;
        }
        if (const std::size_t colon = candidate.find(':'); colon != std::string::npos) {
          const std::string key = clean(candidate.substr(0, colon));
          if (!key.empty() && key != "}" && key != "{") return key;
        }
      }
    } else {
      ++index;
    }
  }
  return std::nullopt;
}

std::string discover_name(const std::filesystem::path& repo,
                          const std::vector<std::string>& run) {
  for (const std::string& word : run) {
    if ((!word.empty() && word.front() == '-') || word.find('/') != std::string::npos ||
        word.find('\\') != std::string::npos) {
      continue;
    }
    const std::string stem = std::filesystem::path(word).stem().string();
    if (stem.empty()) continue;
    if (stem != "python" && stem != "python3" && stem != "node" && stem != "sh" &&
        stem != "bash" && stem != "ruby" && stem != "go" && stem != "java" && stem != "php" &&
        stem != "dotnet" && stem != "cargo" && stem != "rustc") {
      return stem;
    }
  }
  for (const char* manifest : {"pyproject.toml", "package.json"}) {
    const auto text = read_file(repo / manifest);
    if (!text.has_value()) continue;
    if (auto name = script_name(*text); name.has_value()) return *name;
  }
  return "app";
}

Result<int> record_http(const ParsedView& view, const std::filesystem::path& repo);

Result<int> record(const ParsedView& view) {
  const std::filesystem::path repo = repo_of(view);
  bool http = view.flag("http") || view.number("port").has_value();
  if (!http) {
    auto existing = appcase::Config::load(repo / appcase::kCasesDir);
    http = existing.has_value() && existing->http;
  }
  if (http) return record_http(view, repo);
  const std::filesystem::path dir = repo / appcase::kCasesDir;
  auto loaded = appcase::Config::load(dir);
  appcase::Config config = loaded.has_value() ? *loaded : appcase::Config();

  std::vector<std::string> run = view.trailing();
  if (run.empty()) run = config.run;
  if (run.empty()) {
    return std::unexpected(
        "app record needs the command that runs the original, after `--`: `cpp-anything app "
        "record . -- python3 cli.py`");
  }
  // The recorded command keeps its repository-relative words: a checkout at another
  // path still runs it, and `observe` resolves them against the repository.
  config.run = run;
  config.timeout_secs = std::max<std::uint64_t>(view.number("timeout").value_or(
                                                    config.timeout_secs),
                                                1);
  if (!view.values("normalize").empty()) config.normalize = view.values("normalize");
  if (const auto tolerance = view.value("float-tolerance"); tolerance.has_value()) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(tolerance->c_str(), &end);
    if (tolerance->empty() ||
        std::isspace(static_cast<unsigned char>(tolerance->front())) ||
        end != tolerance->c_str() + tolerance->size()) {
      return std::unexpected("--float-tolerance is not a number");
    }
    config.float_tolerance = value;
  }
  if (!view.values("effects").empty()) {
    config.effects.clear();
    for (const std::string& value : view.values("effects")) {
      std::size_t start = 0;
      while (true) {
        const std::size_t comma = value.find(',', start);
        config.effects.push_back(value.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
  }
  if (!view.values("effect-env").empty()) config.effect_env = view.values("effect-env");
  if (const auto command = view.value("db-reset"); command.has_value()) {
    config.db_reset = *command;
  }
  if (const auto command = view.value("db-dump"); command.has_value()) {
    config.db_dump = *command;
  }
  if (view.flag("http") || view.number("port").has_value()) config.http = true;
  if (const auto port = view.number("port"); port.has_value()) {
    config.port = static_cast<std::uint16_t>(*port);
  }
  if (!view.values("arg").empty()) config.args = view.values("arg");
  if (!view.values("fresh").empty()) config.fresh = view.values("fresh");
  if (const auto pace = view.number("pace"); pace.has_value()) config.pace_ms = *pace;
  if (const auto name = view.value("name"); name.has_value()) {
    config.name = *name;
  } else if (config.name.empty()) {
    config.name = discover_name(repo, run);
  }

  auto discovered = discover_cases(repo, config);
  if (!discovered.has_value()) return std::unexpected(discovered.error());
  std::vector<std::pair<std::string, appcase::Case>> sources;
  if (!view.flag("no-traps")) {
    for (auto& item : discovered->examples) sources.push_back(std::move(item));
    for (auto& item : discovered->inputs) sources.push_back(std::move(item));
  } else {
    for (auto& item : discovered->inputs) sources.push_back(std::move(item));
  }
  if (sources.empty()) {
    appcase::Case item;
    item.source = "no-args";
    sources.emplace_back("no-args", std::move(item));
  }

  // Semantic traps: vary each argument and the stdin, so the app meets values the
  // docs never show. This is where whole-app ports break.
  std::vector<std::pair<std::string, appcase::Case>> trapped = sources;
  if (!view.flag("no-traps")) {
    for (const auto& [_, item] : sources) {
      auto extra = trap_cases(item);
      trapped.insert(trapped.end(), extra.begin(), extra.end());
    }
  }
  if (trapped.size() > 300) trapped.resize(300);

  std::set<std::string> taken;
  std::vector<appcase::Case> cases;
  for (std::size_t index = 0; index < trapped.size(); ++index) {
    const std::string& source = trapped[index].first;
    appcase::Case item = trapped[index].second;
    const std::string first_arg = item.args.empty() ? "no-args" : item.args.front();
    const std::string base = std::format("{}-{:03}-{}", label_of(source), index + 1,
                                         slug_of(first_arg));
    std::string name = base;
    int suffix = 2;
    while (!taken.insert(name).second) {
      name = base + "-" + std::to_string(suffix);
      ++suffix;
    }
    item.name = name;
    item.source = source;

    auto started = Sinks::start(config.effects);
    if (!started.has_value()) return std::unexpected(started.error());
    Sinks sinks = std::move(*started);
    const std::vector<std::pair<std::string, std::string>> extra_env =
        sinks.env(resolved_effect_env(sinks, config.effect_env));
    const appcase::RunOptions options{repo, config.run, config.timeout_secs, extra_env,
                                      config.db_reset, config.db_dump};
    auto first = appcase::observe(item, options);
    if (!first.has_value()) return std::unexpected(first.error());
    const std::vector<std::string> first_database = first->database;
    std::optional<appcase::Observation> second;
    if (config.effects.empty() && !config.db_dump.has_value()) {
      auto observed_second = appcase::observe(item, options);
      if (!observed_second.has_value()) return std::unexpected(observed_second.error());
      second = std::move(*observed_second);
    } else {
      // Side effects are not idempotent: a second run would double them, so the
      // case is recorded once and marked stable.
      second = *first;
    }
    appcase::Case settled =
        appcase::settle(item.name, item.source, *first, *second, config.normalize);
    const std::vector<std::string> ports = sinks.ports();
    std::vector<std::string> effects;
    for (const std::string& line : sinks.take()) {
      effects.push_back(mask_ports(appcase::normalize(line, config.normalize), ports));
    }
    for (const std::string& line : first_database) {
      effects.push_back(appcase::normalize(line, config.normalize));
    }
    settled.expect.effects = effects;
    // `settle` only describes what it saw; the case's own inputs are what ran.
    settled.args = item.args;
    settled.stdin = item.stdin;
    settled.env = item.env;
    settled.files = item.files;
    if (view.flag("verbose")) {
      std::println("  {} exit {} {} bytes out{}", pad_right(settled.name, 40),
                   pad_right(std::to_string(settled.expect.exit), 4),
                   pad_left(std::to_string(settled.expect.stdout.size()), 6),
                   settled.unstable ? "  (unstable)" : "");
    }
    cases.push_back(std::move(settled));
  }

  std::error_code error;
  std::filesystem::create_directories(dir, error);
  if (error) return std::unexpected(path_text(dir) + ": " + io_error(error.value()));
  if (auto saved = config.save(dir); !saved.has_value()) return std::unexpected(saved.error());
  if (auto saved = appcase::save_cases(repo, cases); !saved.has_value()) {
    return std::unexpected(saved.error());
  }
  std::size_t unstable = 0;
  for (const auto& item : cases) {
    if (item.unstable) ++unstable;
  }
  if (view.flag("json")) {
    const Json doc = Json::object({
        {"schema", Json::string("cpp-anything/app-record@1")},
        {"app", Json::string(config.name)},
        {"run", strings_json(config.run)},
        {"cases", Json::integer(static_cast<std::int64_t>(cases.size()))},
        {"unstable", Json::integer(static_cast<std::int64_t>(unstable))},
        {"dir", Json::string(path_text(dir))},
    });
    std::println("{}", dump(doc, true));
  } else {
    std::println("cpp-anything app record: {} — {} case(s) in {} ({} unstable)", config.name,
                 cases.size(), path_text(dir), unstable);
    std::println(
        "  next: `cpp-anything app replay .` to check the original against its own cases");
    std::println(
        "        `cpp-anything app front-door .` to put a C++ front door in front of it");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// replay
// ---------------------------------------------------------------------------

Result<int> replay_http_with(const std::filesystem::path& repo,
                             const appcase::Config& config,
                             const std::vector<std::string>& build, const ParsedView& view,
                             const std::vector<std::pair<std::string, std::string>>& extra,
                             std::size_t& fallbacks);

Result<int> replay_http(const std::filesystem::path& repo, const appcase::Config& config,
                        const std::vector<std::string>& build, const ParsedView& view) {
  std::size_t fallbacks = 0;
  return replay_http_with(repo, config, build, view, {}, fallbacks);
}

Result<int> replay(const ParsedView& view) {
  const std::filesystem::path repo = repo_of(view);
  const std::filesystem::path dir = repo / appcase::kCasesDir;
  auto loaded = appcase::Config::load(dir);
  if (!loaded.has_value()) return std::unexpected(loaded.error());
  const appcase::Config config = *loaded;
  std::vector<std::string> build;
  if (const auto command = view.value("build"); command.has_value()) {
    std::size_t start = 0;
    while (start < command->size()) {
      while (start < command->size() &&
             std::isspace(static_cast<unsigned char>((*command)[start]))) {
        ++start;
      }
      std::size_t stop = start;
      while (stop < command->size() &&
             !std::isspace(static_cast<unsigned char>((*command)[stop]))) {
        ++stop;
      }
      if (stop > start) build.push_back(command->substr(start, stop - start));
      start = stop;
    }
  } else {
    build = config.run;
  }
  if (config.http) return replay_http(repo, config, build, view);

  auto loaded_cases = appcase::load_cases(repo);
  if (!loaded_cases.has_value()) return std::unexpected(loaded_cases.error());
  std::vector<appcase::Case> cases = std::move(*loaded_cases);
  if (const auto only = view.value("case"); only.has_value()) {
    cases.erase(std::remove_if(cases.begin(), cases.end(),
                               [&](const appcase::Case& item) { return item.name != *only; }),
                cases.end());
    if (cases.empty()) {
      return std::unexpected("no case named " + quote_debug(*only) +
                             ": `app replay --json` lists them");
    }
  }
  if (build.empty()) {
    return std::unexpected("no build command: pass `--build <command>` or record with one");
  }

  std::size_t passed = 0;
  std::size_t unstable = 0;
  std::size_t fallbacks = 0;
  std::size_t effects_checked = 0;
  std::vector<std::pair<std::string, std::string>> failures;
  const std::uint64_t timeout = view.number("timeout").value_or(config.timeout_secs);
  for (const appcase::Case& item : cases) {
    auto started = Sinks::start(config.effects);
    if (!started.has_value()) return std::unexpected(started.error());
    Sinks sinks = std::move(*started);
    const std::vector<std::pair<std::string, std::string>> extra_env =
        sinks.env(resolved_effect_env(sinks, config.effect_env));
    const appcase::RunOptions options{repo, build, timeout, extra_env, config.db_reset,
                                      config.db_dump};
    auto observed = appcase::observe(item, options);
    if (!observed.has_value()) return std::unexpected(observed.error());
    const std::vector<std::string> ports = sinks.ports();
    observed->effects.clear();
    for (const std::string& line : sinks.take()) {
      observed->effects.push_back(
          mask_ports(appcase::normalize(line, config.normalize), ports));
    }
    for (const std::string& line : observed->database) {
      observed->effects.push_back(appcase::normalize(line, config.normalize));
    }
    if (item.unstable) ++unstable;
    if (observed->fallback) ++fallbacks;
    if (!item.expect.effects.empty()) {
      ++effects_checked;
      if (observed->effects != item.expect.effects) {
        failures.emplace_back(item.name, "effects: expected " + debug_strings(item.expect.effects));
        continue;
      }
    }
    if (auto reason = appcase::compare(item, *observed, config.normalize);
        reason.has_value()) {
      failures.emplace_back(item.name, *reason);
    } else {
      ++passed;
    }
  }

  if (view.flag("json")) {
    Json::Array failure_docs;
    for (const auto& [name, reason] : failures) {
      failure_docs.push_back(Json::object(
          {{"case", Json::string(name)}, {"reason", Json::string(reason)}}));
    }
    const Json doc = Json::object({
        {"schema", Json::string("cpp-anything/app-replay@1")},
        {"app", Json::string(config.name)},
        {"build", strings_json(build)},
        {"cases", Json::integer(static_cast<std::int64_t>(cases.size()))},
        {"passed", Json::integer(static_cast<std::int64_t>(passed))},
        {"unstable", Json::integer(static_cast<std::int64_t>(unstable))},
        {"fallback", Json::integer(static_cast<std::int64_t>(fallbacks))},
        {"effects_checked", Json::integer(static_cast<std::int64_t>(effects_checked))},
        {"failures", Json::array(std::move(failure_docs))},
    });
    std::println("{}", dump(doc, true));
  } else {
    std::string suffix;
    if (unstable > 0) suffix = " (" + std::to_string(unstable) + " unstable)";
    std::println("cpp-anything app replay: {} — {}/{} cases passed{}", config.name, passed,
                 cases.size(), suffix);
    std::string joined;
    for (std::size_t index = 0; index < build.size(); ++index) {
      if (index > 0) joined += ' ';
      joined += build[index];
    }
    std::println("  build: {}", joined);
    if (fallbacks > 0) {
      std::println("  fallback: {}/{} cases ran the original instead of the build", fallbacks,
                   cases.size());
    }
    for (std::size_t index = 0; index < failures.size() && index < 10; ++index) {
      std::println("  ✗ {}\n    {}", failures[index].first, failures[index].second);
    }
    if (failures.size() > 10) {
      std::println("  … {} more", failures.size() - 10);
    }
  }
  return failures.empty() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// HTTP: servers
// ---------------------------------------------------------------------------

Result<void> copy_tree(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::create_directories(to, error);
  if (error) return std::unexpected(path_text(to) + ": " + io_error(error.value()));
  std::filesystem::directory_iterator iterator(from, error);
  if (error) return std::unexpected(path_text(from) + ": " + io_error(error.value()));
  for (const auto& entry : iterator) {
    const std::filesystem::path target = to / entry.path().filename();
    std::error_code kind_error;
    if (std::filesystem::is_directory(entry.path(), kind_error)) {
      auto copied = copy_tree(entry.path(), target);
      if (!copied.has_value()) return copied;
    } else {
      std::error_code copy_error;
      std::filesystem::copy_file(entry.path(), target, copy_error);
      if (copy_error) return std::unexpected(path_text(target) + ": " + io_error(copy_error.value()));
    }
  }
  return {};
}

Result<std::vector<std::pair<std::string, appcase::HttpRequest>>> read_http_inputs() {
  const char* path = std::getenv("CPPAN_APP_INPUTS");
  if (path == nullptr) return std::vector<std::pair<std::string, appcase::HttpRequest>>();
  auto text = read_required(path);
  if (!text.has_value()) return std::unexpected(text.error());
  std::vector<std::pair<std::string, appcase::HttpRequest>> out;
  const std::vector<std::string_view> lines = rust_lines(*text);
  for (std::size_t number = 0; number < lines.size(); ++number) {
    const std::string_view line = lines[number];
    if (trim_ascii(line).empty()) continue;
    Json doc;
    try {
      doc = parse(line);
    } catch (const JsonError& error) {
      return std::unexpected(path_text(path) + ":" + std::to_string(number + 1) + ": " +
                             error.what());
    }
    appcase::HttpRequest item;
    item.method = string_of(doc, "method", "GET");
    item.path = string_of(doc, "path", "/");
    item.headers = pairs_of(doc, "headers");
    if (const Json* body = get(doc, "body"); body != nullptr) {
      item.body = body->is_string() ? body->as_string() : dump(*body, false);
    }
    out.emplace_back("inputs", std::move(item));
  }
  return out;
}

Result<int> record_http(const ParsedView& view, const std::filesystem::path& repo) {
  const std::filesystem::path dir = repo / appcase::kCasesDir;
  auto loaded = appcase::Config::load(dir);
  appcase::Config config = loaded.has_value() ? *loaded : appcase::Config();
  std::vector<std::string> run = view.trailing();
  if (run.empty()) run = config.run;
  if (run.empty()) {
    return std::unexpected(
        "app record --http needs the command that starts the server, after `--`:              "
        "`cpp-anything app record . --http -- python3 server.py`");
  }
  config.http = true;
  config.run = run;
  config.timeout_secs =
      std::max<std::uint64_t>(view.number("timeout").value_or(config.timeout_secs), 1);
  if (const auto name = view.value("name"); name.has_value()) {
    config.name = *name;
  } else if (config.name.empty()) {
    config.name = discover_name(repo, config.run);
  }
  if (const auto port = view.number("port"); port.has_value()) {
    config.port = static_cast<std::uint16_t>(*port);
  }
  if (!view.values("arg").empty()) config.args = view.values("arg");
  if (!view.values("normalize").empty()) config.normalize = view.values("normalize");
  if (!view.values("fresh").empty()) config.fresh = view.values("fresh");
  if (const auto pace = view.number("pace"); pace.has_value()) config.pace_ms = *pace;

  std::uint16_t port = 0;
  if (config.port.has_value()) {
    port = *config.port;
  } else {
    auto free = apphttp::free_port();
    if (!free.has_value()) return std::unexpected(free.error());
    port = *free;
  }
  std::error_code error;
  std::filesystem::path temp = std::filesystem::temp_directory_path(error);
  if (error) temp = "/tmp";
  const std::filesystem::path sandbox =
      temp / "cpp-anything" / "app-http" /
      (std::to_string(static_cast<long long>(::getpid())) + "-" + config.name);
  std::filesystem::remove_all(sandbox, error);
  std::filesystem::create_directories(sandbox, error);
  if (error) return std::unexpected(path_text(sandbox) + ": " + io_error(error.value()));
  for (const std::string& path : config.fresh) {
    const std::filesystem::path source = repo / path;
    const std::filesystem::path target = sandbox / path;
    std::error_code kind_error;
    if (std::filesystem::is_directory(source, kind_error)) {
      auto copied = copy_tree(source, target);
      if (!copied.has_value()) return std::unexpected(copied.error());
    } else if (std::filesystem::is_regular_file(source, kind_error)) {
      std::error_code create_error;
      if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), create_error);
        if (create_error) {
          return std::unexpected(path_text(target.parent_path()) + ": " + io_error(create_error.value()));
        }
      }
      std::error_code copy_error;
      std::filesystem::copy_file(source, target, copy_error);
      if (copy_error) return std::unexpected(path_text(source) + ": " + io_error(copy_error.value()));
    }
  }

  {
    auto started = apphttp::start(config.run, repo, sandbox, port, config.timeout_secs, {});
    if (!started.has_value() && config.port.has_value()) {
      // A server that ignores PORT can still be recorded on a fixed port.
      started = apphttp::start(config.run, repo, sandbox, *config.port, config.timeout_secs, {});
    }
    if (!started.has_value()) return std::unexpected(started.error());
    apphttp::Server server = std::move(*started);

    std::vector<std::pair<std::string, appcase::HttpRequest>> requests;
    for (const std::filesystem::path& path : walk_docs(repo, 0)) {
      const std::string text = read_file(path).value_or(std::string());
      for (appcase::HttpRequest& request : apphttp::requests_in(text)) {
        if (!request.path.empty()) requests.emplace_back("docs", std::move(request));
      }
    }
    auto inputs = read_http_inputs();
    if (!inputs.has_value()) return std::unexpected(inputs.error());
    for (auto& item : *inputs) requests.push_back(std::move(item));
    if (requests.empty()) {
      appcase::HttpRequest request;
      request.method = "GET";
      request.path = "/";
      requests.emplace_back("no-request", std::move(request));
    }
    if (!view.flag("no-traps")) {
      std::vector<std::pair<std::string, appcase::HttpRequest>> trapped;
      for (const auto& [source, request] : requests) {
        trapped.emplace_back(source, request);
        for (const char* path : {"/%C3%BCnicode", "/?q=%F0%9F%91%8D"}) {
          appcase::HttpRequest clone = request;
          clone.path = path;
          trapped.emplace_back("trap", std::move(clone));
        }
        if (!request.body.empty()) {
          for (const char* body : {"{}", "{\"n\":1e21,\"neg\":-0.0,\"s\":\"café\"}", ""}) {
            appcase::HttpRequest clone = request;
            clone.body = body;
            trapped.emplace_back("trap", std::move(clone));
          }
        }
      }
      requests = std::move(trapped);
    }

    std::vector<appcase::Case> cases;
    std::set<std::string> taken;
    for (std::size_t index = 0; index < requests.size(); ++index) {
      const std::string& source = requests[index].first;
      appcase::HttpRequest request = std::move(requests[index].second);
      std::string path = request.path;
      while (!path.empty() && path.front() == '/') path.erase(path.begin());
      const std::string base =
          std::format("{}-{:03}-{}", label_of(source), index + 1, slug_of(path));
      std::string name = base;
      int suffix = 2;
      while (!taken.insert(name).second) {
        name = base + "-" + std::to_string(suffix);
        ++suffix;
      }
      auto response = apphttp::request(server.port, request);
      if (!response.has_value()) return std::unexpected(response.error());
      appcase::Case item;
      item.request = std::move(request);
      item.name = name;
      item.expect.http = apphttp::expectation(std::move(*response));
      item.source = source;
      cases.push_back(std::move(item));
      if (config.pace_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config.pace_ms));
      }
    }
    server.stop();
    if (auto saved = config.save(dir); !saved.has_value()) return std::unexpected(saved.error());
    if (auto saved = appcase::save_cases(repo, cases); !saved.has_value()) {
      return std::unexpected(saved.error());
    }
    if (view.flag("json")) {
      const Json doc = Json::object({
          {"schema", Json::string("cpp-anything/app-record@1")},
          {"app", Json::string(config.name)},
          {"http", Json::boolean(true)},
          {"port", Json::integer(static_cast<std::int64_t>(port))},
          {"cases", Json::integer(static_cast<std::int64_t>(cases.size()))},
      });
      std::println("{}", dump(doc, true));
    } else {
      std::println("cpp-anything app record: {} — {} HTTP case(s) on port {} in {}", config.name,
                   cases.size(), port, path_text(dir));
      std::println("  next: `cpp-anything app replay .` and `cpp-anything app front-door .`");
    }
  }
  return 0;
}

Result<int> replay_http_with(const std::filesystem::path& repo,
                             const appcase::Config& config,
                             const std::vector<std::string>& build, const ParsedView& view,
                             const std::vector<std::pair<std::string, std::string>>& extra,
                             std::size_t& fallbacks) {
  auto loaded_cases = appcase::load_cases(repo);
  if (!loaded_cases.has_value()) return std::unexpected(loaded_cases.error());
  const std::vector<appcase::Case>& cases = *loaded_cases;
  std::uint16_t port = 0;
  if (config.port.has_value()) {
    port = *config.port;
  } else {
    auto free = apphttp::free_port();
    if (!free.has_value()) return std::unexpected(free.error());
    port = *free;
  }
  std::error_code error;
  std::filesystem::path temp = std::filesystem::temp_directory_path(error);
  if (error) temp = "/tmp";
  const std::filesystem::path sandbox =
      temp / "cpp-anything" / "app-http-replay" /
      std::to_string(static_cast<long long>(::getpid()));
  std::filesystem::remove_all(sandbox, error);
  std::filesystem::create_directories(sandbox, error);
  if (error) return std::unexpected(path_text(sandbox) + ": " + io_error(error.value()));
  auto started = apphttp::start(build, repo, sandbox, port, config.timeout_secs, extra);
  if (!started.has_value()) return std::unexpected(started.error());
  apphttp::Server server = std::move(*started);
  std::size_t passed = 0;
  std::vector<std::pair<std::string, std::string>> failures;
  for (const appcase::Case& item : cases) {
    if (const auto only = view.value("case"); only.has_value() && item.name != *only) continue;
    if (!item.request.has_value()) continue;
    auto response = apphttp::request(server.port, *item.request);
    if (!response.has_value()) {
      failures.emplace_back(item.name, response.error());
    } else if (item.expect.http.has_value()) {
      const appcase::HttpExpect& expected = *item.expect.http;
      const std::string body = appcase::normalize(response->body, config.normalize);
      const std::string expected_body =
          appcase::normalize(expected.body, config.normalize);
      const std::string content_type = apphttp::content_type(*response);
      if (response->status != expected.status) {
        failures.emplace_back(item.name,
                              "status: expected " + std::to_string(expected.status) + ", got " +
                                  std::to_string(response->status));
      } else if (!expected.content_type.empty() && content_type != expected.content_type) {
        failures.emplace_back(item.name, "content-type: expected " +
                                              quote_debug(expected.content_type) + ", got " +
                                              quote_debug(content_type));
      } else if (body != expected_body) {
        failures.emplace_back(item.name, "body: expected " +
                                              quote_debug(truncate_text(expected_body, 120)) +
                                              ", got " +
                                              quote_debug(truncate_text(body, 120)));
      } else {
        ++passed;
      }
    }
    if (config.pace_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(config.pace_ms));
    }
  }
  server.stop();
  if (view.flag("json")) {
    Json::Array failure_docs;
    for (const auto& [name, reason] : failures) {
      failure_docs.push_back(
          Json::object({{"case", Json::string(name)}, {"reason", Json::string(reason)}}));
    }
    const Json doc = Json::object({
        {"schema", Json::string("cpp-anything/app-replay@1")},
        {"app", Json::string(config.name)},
        {"http", Json::boolean(true)},
        {"cases", Json::integer(static_cast<std::int64_t>(cases.size()))},
        {"passed", Json::integer(static_cast<std::int64_t>(passed))},
        {"failures", Json::array(std::move(failure_docs))},
    });
    std::println("{}", dump(doc, true));
  } else {
    std::println("cpp-anything app replay: {} (HTTP) — {}/{} cases passed", config.name, passed,
                 cases.size());
    for (std::size_t index = 0; index < failures.size() && index < 10; ++index) {
      std::println("  ✗ {}\n    {}", failures[index].first, failures[index].second);
    }
  }
  fallbacks = 0;
  for (const auto& [key, path] : extra) {
    if (key != "CPPAN_FALLBACK_LOG") continue;
    if (const auto text = read_file(path); text.has_value()) {
      for (const std::string_view line : rust_lines(*text)) {
        if (!trim_ascii(line).empty()) ++fallbacks;
      }
    }
  }
  return failures.empty() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// front door
// ---------------------------------------------------------------------------

Result<int> replay_build(const std::filesystem::path& repo,
                         const std::filesystem::path& build, const ParsedView& view) {
  const std::filesystem::path dir = repo / appcase::kCasesDir;
  auto loaded = appcase::Config::load(dir);
  if (!loaded.has_value()) return std::unexpected(loaded.error());
  const appcase::Config& config = *loaded;
  auto loaded_cases = appcase::load_cases(repo);
  if (!loaded_cases.has_value()) return std::unexpected(loaded_cases.error());
  const std::vector<appcase::Case>& cases = *loaded_cases;
  const std::vector<std::string> command = {path_text(build)};
  const appcase::RunOptions options{repo, command,
                                    view.number("timeout").value_or(config.timeout_secs), {}, {},
                                    {}};
  std::size_t passed = 0;
  std::size_t fallbacks = 0;
  std::vector<std::pair<std::string, std::string>> failures;
  for (const appcase::Case& item : cases) {
    auto observed = appcase::observe(item, options);
    if (!observed.has_value()) return std::unexpected(observed.error());
    if (observed->fallback) ++fallbacks;
    if (auto reason = appcase::compare(item, *observed, config.normalize);
        reason.has_value()) {
      failures.emplace_back(item.name, *reason);
    } else {
      ++passed;
    }
  }
  std::println("  replay: {}/{} passed, {} fallback", passed, cases.size(), fallbacks);
  for (std::size_t index = 0; index < failures.size() && index < 5; ++index) {
    std::println("  ✗ {}\n    {}", failures[index].first, failures[index].second);
  }
  return failures.empty() ? 0 : 1;
}

Result<int> front_door(const ParsedView& view) {
  const std::filesystem::path repo = repo_of(view);
  if (view.flag("library")) {
    const std::filesystem::path app_dir = repo / "cpp" / "lib";
    auto written = appfront::write_library(repo, app_dir);
    if (!written.has_value()) return std::unexpected(written.error());
    const std::string name = repo_name(repo);
    std::println("cpp-anything app front-door --library: {} — {} file(s) in {}", name, *written,
                 short_path(app_dir));
    if (view.flag("no-check")) return 0;
    auto library = appfront::build_library(app_dir);
    if (!library.has_value()) return std::unexpected(library.error());
    std::println("  built {}", short_path(*library));
    return 0;
  }
  const std::filesystem::path dir = repo / appcase::kCasesDir;
  auto loaded = appcase::Config::load(dir);
  if (!loaded.has_value()) return std::unexpected(loaded.error());
  const appcase::Config& config = *loaded;
  // Servers get a proxy front door: it owns PORT and forwards to the original.
  const std::filesystem::path app_dir = repo / "cpp" / "app";
  auto routes = appfront::write(app_dir, config);
  if (!routes.has_value()) return std::unexpected(routes.error());
  std::println("cpp-anything app front-door: {} — {} file(s) in {}, {} route(s)", config.name,
               *routes, short_path(app_dir), appfront::route_count(app_dir));
  if (view.flag("no-check")) return 0;
  auto built = appfront::build(app_dir);
  if (!built.has_value()) return std::unexpected(built.error());
  std::println("  built {}", short_path(*built));
  auto reloaded = appcase::Config::load(dir);
  if (!reloaded.has_value()) return std::unexpected(reloaded.error());
  if (reloaded->http) {
    return replay_http(repo, *reloaded, {path_text(*built)}, view);
  }
  return replay_build(repo, *built, view);
}

// ---------------------------------------------------------------------------
// wire
// ---------------------------------------------------------------------------

Result<int> wire(const ParsedView& view) {
  const std::filesystem::path repo = repo_of(view);
  const auto unit = view.positional(1);
  if (!unit.has_value()) {
    return std::unexpected("app wire needs a unit: the folder or unit id it was bridged from");
  }
  const std::filesystem::path app_dir = repo / "cpp" / "app";
  std::error_code error;
  if (!std::filesystem::exists(app_dir, error)) {
    return std::unexpected("no front door yet: run `cpp-anything app front-door .` first");
  }
  auto loaded = appcase::Config::load(repo / appcase::kCasesDir);
  if (!loaded.has_value()) return std::unexpected(loaded.error());
  const appcase::Config& config = *loaded;
  std::optional<std::filesystem::path> project;
  if (const auto value = view.value("project"); value.has_value()) {
    project = std::filesystem::path(*value);
  }
  if (config.http) {
    const std::vector<std::string> texts = view.values("route");
    if (texts.empty()) {
      return std::unexpected(
          "a server unit answers HTTP routes: pass `--route \"GET /path\"` (repeatable; `{param}` "
          "captures a segment, `*` catches everything)");
    }
    auto file = appfront::wire_http(app_dir, repo, *unit, texts, project);
    if (!file.has_value()) return std::unexpected(file.error());
    std::string joined;
    for (std::size_t index = 0; index < texts.size(); ++index) {
      if (index > 0) joined += ", ";
      joined += texts[index];
    }
    std::println("cpp-anything app wire: {} → {}\n  wrote {} — fill the handler, then `app "
                 "front-door .`",
                 joined, *unit, short_path(*file));
    return 0;
  }
  const auto command = view.value("command");
  if (!command.has_value()) {
    return std::unexpected(
        "app wire needs `--command NAME`: the CLI command this unit answers");
  }
  auto file = appfront::wire(app_dir, repo, *unit, *command, project);
  if (!file.has_value()) return std::unexpected(file.error());
  std::println("cpp-anything app wire: {} → {}\n  wrote {} — fill the handler, then `app "
               "front-door .`",
               *command, *unit, short_path(*file));
  return 0;
}

// ---------------------------------------------------------------------------
// lift
// ---------------------------------------------------------------------------

/// Lifting a server: every recorded request must be answered by a native route and
/// nothing may reach the original. Then the front door is rebuilt without the
/// original process and replayed once more to prove it stands alone.
Result<int> lift_http(const std::filesystem::path& repo, appcase::Config& config,
                      const ParsedView& view) {
  const std::filesystem::path app_dir = repo / "cpp" / "app";
  auto built = appfront::build(app_dir);
  if (!built.has_value()) return std::unexpected(built.error());
  std::error_code error;
  std::filesystem::path temp = std::filesystem::temp_directory_path(error);
  if (error) temp = "/tmp";
  const std::filesystem::path log =
      temp / ("cppan-lift-http-" + std::to_string(static_cast<long long>(::getpid())) + ".log");
  std::filesystem::remove(log, error);
  const std::vector<std::pair<std::string, std::string>> extra = {
      {"CPPAN_FALLBACK_LOG", path_text(log)}};
  const std::vector<std::string> build_arg = {path_text(*built)};
  std::size_t fallbacks = 0;
  auto code = replay_http_with(repo, config, build_arg, view, extra, fallbacks);
  if (!code.has_value()) return std::unexpected(code.error());
  if (*code != 0) return *code;
  if (fallbacks > 0) {
    std::println(
        "cpp-anything app lift: {} recorded request(s) still go to the original; wire the routes "
        "they need (`app wire <repo> <unit> --route \"METHOD /path\"`) and lift again",
        fallbacks);
    return 1;
  }
  config.lifted = true;
  if (auto saved = config.save(repo / appcase::kCasesDir); !saved.has_value()) {
    return std::unexpected(saved.error());
  }
  if (auto written = appfront::write(app_dir, config); !written.has_value()) {
    return std::unexpected(written.error());
  }
  auto rebuilt = appfront::build(app_dir);
  if (!rebuilt.has_value()) return std::unexpected(rebuilt.error());
  const std::vector<std::string> rebuilt_arg = {path_text(*rebuilt)};
  std::size_t ignored = 0;
  auto final_code = replay_http_with(repo, config, rebuilt_arg, view, {}, ignored);
  if (!final_code.has_value()) return std::unexpected(final_code.error());
  if (*final_code != 0) return *final_code;
  std::println(
      "cpp-anything app lift: {} — every recorded request is answered natively; the original is "
      "no longer started",
      config.name);
  return 0;
}

Result<int> lift(const ParsedView& view) {
  const std::filesystem::path repo = repo_of(view);
  const std::filesystem::path dir = repo / appcase::kCasesDir;
  std::error_code error;
  if (!std::filesystem::exists(dir / "config.json", error) &&
      std::filesystem::exists(repo / "cpp" / "lib", error)) {
    std::println(
        "cpp-anything app lift: {} is a library front door: every unit is a plain C++ library, "
        "so there is no fallback to remove",
        short_path(repo / "cpp" / "lib"));
    return 0;
  }
  auto loaded = appcase::Config::load(dir);
  if (!loaded.has_value()) return std::unexpected(loaded.error());
  appcase::Config config = *loaded;
  if (config.http) return lift_http(repo, config, view);
  std::filesystem::path build =
      repo / "cpp" / "app" / "build" /
#if defined(_WIN32)
      "cppan_app.exe";
#else
      "cppan_app";
#endif
  if (!std::filesystem::exists(build, error)) {
    // A fresh checkout has no build tree: build it here rather than make the user
    // discover the order.
    auto built = appfront::build(repo / "cpp" / "app");
    if (!built.has_value()) return std::unexpected(built.error());
    build = *built;
  }
  auto canonical = std::filesystem::canonical(build, error);
  if (error) return std::unexpected(path_text(build) + ": " + io_error(error.value()));
  build = canonical;
  auto code = replay_build(repo, build, view);
  if (!code.has_value()) return std::unexpected(code.error());
  if (*code != 0) return *code;
  // Lifting means the front door answered every case itself: no fallback.
  auto loaded_cases = appcase::load_cases(repo);
  if (!loaded_cases.has_value()) return std::unexpected(loaded_cases.error());
  const std::vector<appcase::Case>& cases = *loaded_cases;
  const std::vector<std::string> command = {path_text(build)};
  const appcase::RunOptions options{repo, command, config.timeout_secs, {}, {}, {}};
  std::size_t fallbacks = 0;
  for (const appcase::Case& item : cases) {
    auto observed = appcase::observe(item, options);
    if (!observed.has_value()) return std::unexpected(observed.error());
    if (observed->fallback) ++fallbacks;
  }
  if (fallbacks > 0) {
    std::println(
        "cpp-anything app lift: {} case(s) still run the original; wire the units they need "
        "(`app wire`) and lift again",
        fallbacks);
    return 1;
  }
  config.lifted = true;
  if (auto saved = config.save(dir); !saved.has_value()) {
    return std::unexpected(saved.error());
  }
  const std::string standalone =
      "# " + config.name +
      " is standalone\n\nEvery recorded case is answered by the C++ front door in `cpp/app/`; the "
      "original is no longer on any path a case takes. Keep it as reference, keep `cpp-anything "
      "app replay .` in CI, and keep the cases: they are the contract.\n";
  if (auto written = write_file(dir / "STANDALONE.md", standalone); !written.has_value()) {
    return std::unexpected(written.error());
  }
  std::println(
      "cpp-anything app lift: {} is standalone — {} case(s) answered by the front door, 0 "
      "fallback\n  wrote {}",
      config.name, cases.size(), short_path(dir / "STANDALONE.md"));
  return 0;
}

}  // namespace

Result<int> command(const cli::Parsed& args) {
  const std::string sub =
      args.positional.empty() ? std::string("record") : args.positional.front();
  const ParsedView rest(args, 1);
  if (sub == "record") return record(rest);
  if (sub == "replay") return replay(rest);
  if (sub == "front-door" || sub == "front_door") return front_door(rest);
  if (sub == "wire") return wire(rest);
  if (sub == "lift") return lift(rest);
  return std::unexpected("unknown app command " + quote_debug(sub) +
                         "; try record, replay, front-door, wire, lift");
}

}  // namespace cppan::app
