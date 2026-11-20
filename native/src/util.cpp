#include "cppan/util.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cppan {
namespace {

std::size_t utf8_length(std::string_view text) {
  std::size_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0U) != 0x80U) ++count;
  }
  return count;
}

}  // namespace

std::vector<std::uint64_t> version_key(std::string_view text) {
  std::vector<std::uint64_t> out;
  std::string current;
  for (const char ch : text) {
    if (ch >= '0' && ch <= '9') {
      current += ch;
    } else if (!current.empty()) {
      out.push_back(std::stoull(current));
      current.clear();
    }
  }
  if (!current.empty()) out.push_back(std::stoull(current));
  return out;
}

int version_cmp(std::string_view left, std::string_view right) {
  const auto a = version_key(left);
  const auto b = version_key(right);
  const std::size_t limit = std::max(a.size(), b.size());
  for (std::size_t index = 0; index < limit; ++index) {
    const std::uint64_t x = index < a.size() ? a[index] : 0;
    const std::uint64_t y = index < b.size() ? b[index] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

bool version_at_least(std::string_view version, std::string_view minimum) {
  return version_cmp(version, minimum) >= 0;
}

std::string truncate_text(std::string_view text, std::size_t width) {
  std::string out;
  out.reserve(text.size() + 3);
  std::size_t count = 0;
  for (std::size_t index = 0; index < text.size();) {
    if (count >= width) {
      out += "…";
      break;
    }
    // Copy one UTF-8 code point whole: names carry `—`, `…` and `×`.
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if ((lead & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4;
    }
    out.append(text.substr(index, std::min(length, text.size() - index)));
    index += length;
    ++count;
  }
  std::string replaced;
  replaced.reserve(out.size());
  for (const char ch : out) {
    if (ch == '\n') {
      replaced += "\\n";
    } else {
      replaced += ch;
    }
  }
  return replaced;
}

std::string pad_right(std::string_view text, std::size_t width) {
  std::string out(text);
  const std::size_t length = utf8_length(text);
  if (length < width) out.append(width - length, ' ');
  return out;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

Result<void> write_file(const std::filesystem::path& path, std::string_view contents) {
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return std::unexpected(path.parent_path().string() + ": " + error.message());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) return std::unexpected(path.string() + ": cannot open for writing");
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!stream) return std::unexpected(path.string() + ": write failed");
  return {};
}

std::optional<std::filesystem::path> repo_root() {
  std::error_code error;
  auto directory = std::filesystem::current_path(error);
  if (error) return std::nullopt;
  while (true) {
    if (std::filesystem::is_regular_file(directory / "data" / "matrix.json", error)) {
      return directory;
    }
    if (!directory.has_parent_path() || directory.parent_path() == directory) return std::nullopt;
    directory = directory.parent_path();
  }
}

std::string short_path(const std::filesystem::path& path) {
  std::error_code error;
  const auto cwd = std::filesystem::current_path(error);
  if (!error) {
    const auto relative = std::filesystem::relative(path, cwd, error);
    if (!error) return relative.string();
  }
  return path.string();
}

std::string indent_text(std::string_view text, std::size_t spaces) {
  const std::string pad(spaces, ' ');
  std::string out;
  std::string line;
  const auto flush = [&] {
    if (!line.empty()) out += pad;
    out += line;
    out += '\n';
    line.clear();
  };
  for (const char ch : text) {
    if (ch == '\n') {
      flush();
    } else {
      line += ch;
    }
  }
  if (!line.empty()) flush();
  if (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

}  // namespace cppan

namespace cppan {

std::string Run::failure() const {
  const std::string code = status.has_value() ? std::to_string(*status) : "signal";
  std::string_view trimmed = err;
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
    trimmed.remove_suffix(1);
  }
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
    trimmed.remove_prefix(1);
  }
  if (trimmed.empty()) return "exited with " + code;
  return "exited with " + code + ": " + std::string(trimmed);
}

std::string hash64(std::string_view text) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char ch : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= 0x100000001b3ULL;
  }
  char buffer[17];
  std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(hash));
  return buffer;
}

}  // namespace cppan

#if defined(__unix__) || defined(__APPLE__)
namespace cppan {
namespace {

void write_all(int fd, std::string_view text) {
  while (!text.empty()) {
    const ssize_t written = ::write(fd, text.data(), text.size());
    if (written <= 0) {
      if (errno == EINTR) continue;
      return;
    }
    text.remove_prefix(static_cast<std::size_t>(written));
  }
}

}  // namespace

Run run_command(const std::string& program, const std::vector<std::string>& args,
                const std::optional<std::filesystem::path>& cwd,
                const std::vector<std::pair<std::string, std::string>>& envs) {
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
    Run failed;
    failed.err = std::string("pipe: ") + std::strerror(errno);
    return failed;
  }

  std::vector<std::string> argv_store;
  argv_store.push_back(program);
  for (const std::string& arg : args) argv_store.push_back(arg);
  std::vector<char*> argv;
  argv.reserve(argv_store.size() + 1);
  for (std::string& arg : argv_store) argv.push_back(arg.data());
  argv.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    Run failed;
    failed.err = std::string("fork: ") + std::strerror(errno);
    return failed;
  }
  if (child == 0) {
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    if (cwd.has_value() && ::chdir(cwd->c_str()) != 0) {
      write_all(STDERR_FILENO, "cannot enter " + cwd->string() + "\n");
      ::_exit(126);
    }
    for (const auto& [name, value] : envs) ::setenv(name.c_str(), value.c_str(), 1);
    ::execvp(program.c_str(), argv.data());
    const int code = errno == ENOENT ? 127 : 126;
    write_all(STDERR_FILENO,
              errno == ENOENT ? program + ": command not found\n"
                              : program + ": " + std::strerror(errno) + "\n");
    ::_exit(code);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  std::string out;
  std::string err;
  pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
  int open_streams = 2;
  char buffer[8192];
  while (open_streams > 0) {
    const int ready = ::poll(fds, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int index = 0; index < 2; ++index) {
      if (fds[index].fd < 0) continue;
      if ((fds[index].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
      const ssize_t got = ::read(fds[index].fd, buffer, sizeof buffer);
      if (got > 0) {
        (index == 0 ? out : err).append(buffer, static_cast<std::size_t>(got));
      } else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
        ::close(fds[index].fd);
        fds[index].fd = -1;
        --open_streams;
      }
    }
  }
  if (fds[0].fd >= 0) ::close(fds[0].fd);
  if (fds[1].fd >= 0) ::close(fds[1].fd);

  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  Run run;
  run.out = std::move(out);
  run.err = std::move(err);
  if (WIFEXITED(status)) {
    run.status = WEXITSTATUS(status);
    run.ok = *run.status == 0;
  }
  return run;
}

}  // namespace cppan
#endif

namespace cppan {

std::string now_iso8601() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::int64_t epoch_seconds =
      std::chrono::floor<std::chrono::seconds>(now.time_since_epoch()).count();
  const std::int64_t days = epoch_seconds / 86'400;
  const std::int64_t time = epoch_seconds % 86'400;
  // Howard Hinnant's days-to-civil algorithm, which is public domain.
  const std::int64_t z = days + 719'468;
  const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
  const auto doe = static_cast<std::uint64_t>(z - era * 146'097);
  const std::uint64_t yoe =
      (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365;
  const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
  const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::uint64_t mp = (5 * doy + 2) / 153;
  const std::uint32_t day = static_cast<std::uint32_t>(doy - (153 * mp + 2) / 5 + 1);
  const std::uint32_t month = static_cast<std::uint32_t>(mp < 10 ? mp + 3 : mp - 9);
  const std::int64_t civil_year = month <= 2 ? year + 1 : year;
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ",
                static_cast<long long>(civil_year), month, day,
                static_cast<long long>(time / 3600),
                static_cast<long long>((time % 3600) / 60),
                static_cast<long long>(time % 60));
  return buffer;
}

std::optional<std::filesystem::path> relative_path(const std::filesystem::path& base,
                                                   const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute_base = std::filesystem::canonical(base, error);
  if (error) return std::nullopt;
  const auto absolute_path = std::filesystem::canonical(path, error);
  if (error) return std::nullopt;
  std::filesystem::path out;
  auto base_part = absolute_base.begin();
  auto path_part = absolute_path.begin();
  while (base_part != absolute_base.end() && path_part != absolute_path.end() &&
         *base_part == *path_part) {
    ++base_part;
    ++path_part;
  }
  for (; base_part != absolute_base.end(); ++base_part) out /= "..";
  for (; path_part != absolute_path.end(); ++path_part) out /= *path_part;
  return out;
}

std::string sanitize_identifier(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    } else if (!out.ends_with('_')) {
      out += '_';
    }
  }
  while (!out.empty() && out.front() == '_') out.erase(out.begin());
  while (!out.empty() && out.back() == '_') out.pop_back();
  if (out.empty()) return "unit";
  if (out.front() >= '0' && out.front() <= '9') return "u_" + out;
  return out;
}

}  // namespace cppan
