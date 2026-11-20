module;
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

module cppan.apphttp;
import cppan.json;
import cppan.types;
import cppan.util;
import std;

namespace cppan::apphttp {
namespace {

std::string io_error(int code) {
  return std::string(std::strerror(code)) + " (os error " + std::to_string(code) + ")";
}

std::string trim_ascii(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return std::string(text.substr(begin, end - begin));
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(left[index])) !=
        std::tolower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

std::string lower_ascii(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

std::string lossy(std::string_view bytes) {
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

std::optional<int> connect_to(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return std::nullopt;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  return fd;
}

bool send_all(int fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t wrote = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (wrote == 0) return false;
    sent += static_cast<std::size_t>(wrote);
  }
  return true;
}

std::optional<std::uint16_t> parse_u16(std::string_view text) {
  std::uint16_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<std::size_t> parse_size(std::string_view text) {
  std::size_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::size_t parse_hex(std::string_view text) {
  std::size_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return 0;
  return value;
}

std::string status_text(int status) {
  if (WIFEXITED(status)) return "exit status: " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) return "signal: " + std::to_string(WTERMSIG(status));
  return "exit status: -1";
}

std::vector<std::uint8_t> decode_chunked(const std::string& body) {
  std::vector<std::uint8_t> out;
  std::size_t index = 0;
  while (index < body.size()) {
    const std::size_t end = body.find("\r\n", index);
    if (end == std::string::npos) break;
    std::string size_text = lossy(std::string_view(body).substr(index, end - index));
    const std::size_t semicolon = size_text.find(';');
    if (semicolon != std::string::npos) size_text = size_text.substr(0, semicolon);
    const std::size_t size = parse_hex(trim_ascii(size_text));
    index = end + 2;
    if (size == 0) break;
    const std::size_t stop = std::min(index + size, body.size());
    out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(index),
               body.begin() + static_cast<std::ptrdiff_t>(stop));
    index = stop + 2;
  }
  return out;
}

Result<Response> parse_response(const std::string& raw) {
  const std::size_t split = raw.find("\r\n\r\n");
  if (split == std::string::npos) {
    return std::unexpected("the response had no header terminator");
  }
  const std::string head = lossy(std::string_view(raw).substr(0, split));
  std::string body = raw.substr(split + 4);
  const std::vector<std::string_view> lines = rust_lines(head);
  const std::string_view status_line = lines.empty() ? std::string_view() : lines.front();
  // split_whitespace().nth(1)
  std::optional<std::uint16_t> status;
  {
    std::size_t at = 0;
    int word = 0;
    while (at < status_line.size()) {
      while (at < status_line.size() &&
             std::isspace(static_cast<unsigned char>(status_line[at]))) {
        ++at;
      }
      const std::size_t start = at;
      while (at < status_line.size() &&
             !std::isspace(static_cast<unsigned char>(status_line[at]))) {
        ++at;
      }
      if (at == start) break;
      if (word == 1) {
        status = parse_u16(status_line.substr(start, at - start));
        break;
      }
      ++word;
    }
  }
  if (!status.has_value()) {
    return std::unexpected("unreadable status line " + quote_debug(status_line));
  }
  Response response;
  response.status = *status;
  bool chunked = false;
  std::optional<std::size_t> content_length;
  for (std::size_t index = 1; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string key = trim_ascii(line.substr(0, colon));
    const std::string value = trim_ascii(line.substr(colon + 1));
    if (equals_ignore_case(key, "transfer-encoding") &&
        lower_ascii(value).find("chunked") != std::string::npos) {
      chunked = true;
    }
    if (equals_ignore_case(key, "content-length")) {
      content_length = parse_size(value);
    }
    response.headers.emplace_back(key, value);
  }
  if (chunked) {
    const auto decoded = decode_chunked(body);
    body.assign(decoded.begin(), decoded.end());
  } else if (content_length.has_value()) {
    body.resize(std::min(*content_length, body.size()));
  }
  response.body = lossy(body);
  return response;
}

std::optional<std::vector<std::string>> shell_words(std::string_view command) {
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
  return out;
}

std::optional<appcase::HttpRequest> parse_curl(const std::string& command) {
  const auto words_option = shell_words(command);
  if (!words_option.has_value()) return std::nullopt;
  const std::vector<std::string>& words = *words_option;
  std::string method;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string url;
  std::size_t index = 1;
  while (index < words.size()) {
    const std::string& word = words[index];
    if (word == "-X" || word == "--request") {
      ++index;
      if (index >= words.size()) return std::nullopt;
      method = words[index];
    } else if (word == "-H" || word == "--header") {
      ++index;
      if (index >= words.size()) return std::nullopt;
      const std::string& header = words[index];
      if (const std::size_t colon = header.find(':'); colon != std::string::npos) {
        headers.emplace_back(trim_ascii(header.substr(0, colon)),
                             trim_ascii(header.substr(colon + 1)));
      }
    } else if (word == "-d" || word == "--data" || word == "--data-raw" ||
               word == "--data-binary") {
      ++index;
      if (index >= words.size()) return std::nullopt;
      body = words[index];
      if (method.empty()) method = "POST";
    } else if (word == "-s" || word == "-S" || word == "--silent" || word == "--show-error" ||
               word == "-i" || word == "-v" || word == "--location" || word == "-L") {
      // ignored
    } else if (!word.empty() && word.front() != '-') {
      url = word;
    }
    ++index;
  }
  if (url.empty()) return std::nullopt;
  std::string path;
  const std::size_t scheme = url.find("://");
  if (scheme != std::string::npos) {
    const std::string after = url.substr(scheme + 3);
    const std::size_t slash = after.find('/');
    path = slash == std::string::npos ? "/" : after.substr(slash);
  } else {
    path = url;
  }
  appcase::HttpRequest request;
  request.method = method.empty() ? "GET" : method;
  request.path = path;
  request.headers = std::move(headers);
  request.body = std::move(body);
  return request;
}

}  // namespace

Server::Server(Server&& other) noexcept : port(other.port) {
  child_ = other.child_;
  other.child_ = -1;
  other.port = 0;
}

Server& Server::operator=(Server&& other) noexcept {
  if (this == &other) return *this;
  stop();
  child_ = other.child_;
  port = other.port;
  other.child_ = -1;
  other.port = 0;
  return *this;
}

Server::~Server() { stop(); }

void Server::stop() {
  if (child_ <= 0) return;
  (void)::kill(child_, SIGKILL);
  int status = 0;
  while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {
  }
  child_ = -1;
}

Result<std::uint16_t> free_port() {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return std::unexpected("bind: " + io_error(errno));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    const int code = errno;
    ::close(listener);
    return std::unexpected("bind: " + io_error(code));
  }
  socklen_t length = sizeof address;
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    const int code = errno;
    ::close(listener);
    return std::unexpected("local_addr: " + io_error(code));
  }
  const std::uint16_t port = ntohs(address.sin_port);
  ::close(listener);
  return port;
}

Result<Server> start(const std::vector<std::string>& command, const std::filesystem::path& repo,
                     const std::filesystem::path& cwd, std::uint16_t port,
                     std::uint64_t timeout_secs,
                     const std::vector<std::pair<std::string, std::string>>& extra) {
  if (command.empty()) {
    return std::unexpected("no command to start: pass `-- <command>` or record one");
  }
  const std::vector<std::string> resolved = appcase::resolve_command(repo, command);
  std::vector<std::string> argv_store = resolved;
  std::vector<char*> argv;
  argv.reserve(argv_store.size() + 1);
  for (std::string& word : argv_store) argv.push_back(word.data());
  argv.push_back(nullptr);

  int exec_pipe[2] = {-1, -1};
  if (::pipe(exec_pipe) != 0) return std::unexpected("pipe: " + io_error(errno));
  const int flags = ::fcntl(exec_pipe[1], F_GETFD);
  if (flags >= 0) (void)::fcntl(exec_pipe[1], F_SETFD, flags | FD_CLOEXEC);

  const pid_t child = ::fork();
  if (child < 0) {
    const int code = errno;
    ::close(exec_pipe[0]);
    ::close(exec_pipe[1]);
    return std::unexpected("fork: " + io_error(code));
  }
  if (child == 0) {
    ::close(exec_pipe[0]);
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      ::dup2(null_fd, STDIN_FILENO);
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO) ::close(null_fd);
    }
    if (::chdir(cwd.c_str()) != 0) ::_exit(126);
    const std::string port_text = std::to_string(port);
    ::setenv("PORT", port_text.c_str(), 1);
    ::setenv("CPPAN_PORT", port_text.c_str(), 1);
    for (const auto& [key, value] : extra) ::setenv(key.c_str(), value.c_str(), 1);
    ::execvp(argv[0], argv.data());
    const int code = errno;
    (void)!::write(exec_pipe[1], &code, sizeof code);
    ::_exit(127);
  }
  ::close(exec_pipe[1]);
  int exec_fd = exec_pipe[0];
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs == 0 ? 1 : timeout_secs);
  while (true) {
    if (const auto fd = connect_to(port); fd.has_value()) {
      ::close(*fd);
      if (exec_fd >= 0) ::close(exec_fd);
      return Server(static_cast<int>(child), port);
    }
    if (exec_fd >= 0) {
      int code = 0;
      const ssize_t got = ::read(exec_fd, &code, sizeof code);
      if (got == static_cast<ssize_t>(sizeof code)) {
        ::close(exec_fd);
        int status = 0;
        (void)::waitpid(child, &status, 0);
        return std::unexpected((resolved.empty() ? std::string() : resolved.front()) + ": " +
                               io_error(code));
      }
      if (got == 0) {
        ::close(exec_fd);
        exec_fd = -1;
      }
    }
    int status = 0;
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (exec_fd >= 0) ::close(exec_fd);
      return std::unexpected("the server exited before listening on " + std::to_string(port) +
                             " (" + status_text(status) + "); is it reading PORT?");
    }
    if (std::chrono::steady_clock::now() > deadline) {
      int ignored = 0;
      (void)::kill(child, SIGKILL);
      (void)::waitpid(child, &ignored, 0);
      if (exec_fd >= 0) ::close(exec_fd);
      return std::unexpected("the server did not listen on " + std::to_string(port) + " within " +
                             std::to_string(timeout_secs) +
                             "s; it must read the PORT environment variable");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

Result<Response> request(std::uint16_t port, const appcase::HttpRequest& request) {
  const int stream = ::socket(AF_INET, SOCK_STREAM, 0);
  if (stream < 0) return std::unexpected("connect to " + std::to_string(port) + ": " + io_error(errno));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(stream, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    const int code = errno;
    ::close(stream);
    return std::unexpected("connect to " + std::to_string(port) + ": " + io_error(code));
  }
  timeval timeout{};
  timeout.tv_sec = 30;
  if (::setsockopt(stream, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0) {
    const int code = errno;
    ::close(stream);
    return std::unexpected("read timeout: " + io_error(code));
  }
  const std::string& method = request.method.empty() ? std::string("GET") : request.method;
  const std::string& path = request.path.empty() ? std::string("/") : request.path;
  std::string text = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                     std::to_string(port) + "\r\nConnection: close\r\n";
  bool has_content_type = false;
  for (const auto& [key, value] : request.headers) {
    if (equals_ignore_case(key, "host") || equals_ignore_case(key, "connection")) continue;
    if (equals_ignore_case(key, "content-type")) has_content_type = true;
    text += key + ": " + value + "\r\n";
  }
  if (!request.body.empty()) {
    if (!has_content_type) text += "Content-Type: application/json\r\n";
    text += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  }
  text += "\r\n";
  if (!send_all(stream, text)) {
    const int code = errno;
    ::close(stream);
    return std::unexpected("write: " + io_error(code));
  }
  if (!send_all(stream, request.body)) {
    const int code = errno;
    ::close(stream);
    return std::unexpected("write: " + io_error(code));
  }
  std::string raw;
  char buffer[8192];
  while (true) {
    const ssize_t got = ::recv(stream, buffer, sizeof buffer, 0);
    if (got > 0) {
      raw.append(buffer, static_cast<std::size_t>(got));
      continue;
    }
    if (got == 0) break;
    if (errno == EINTR) continue;
    const int code = errno;
    ::close(stream);
    return std::unexpected("read: " + io_error(code));
  }
  ::close(stream);
  return parse_response(raw);
}

std::string content_type(const Response& response) {
  for (const auto& [key, value] : response.headers) {
    if (equals_ignore_case(key, "content-type")) return value;
  }
  return std::string();
}

appcase::HttpExpect expectation(Response response) {
  appcase::HttpExpect expect;
  expect.status = response.status;
  expect.content_type = content_type(response);
  expect.body = std::move(response.body);
  return expect;
}

std::vector<appcase::HttpRequest> requests_in(std::string_view text) {
  std::vector<appcase::HttpRequest> out;
  for (const std::string_view line : rust_lines(text)) {
    std::string trimmed = trim_ascii(line);
    while (trimmed.starts_with("$ ")) trimmed = trim_ascii(trimmed.substr(2));
    if (!trimmed.starts_with("curl ")) continue;
    if (auto parsed = parse_curl(trimmed); parsed.has_value()) out.push_back(std::move(*parsed));
  }
  return out;
}

}  // namespace cppan::apphttp
