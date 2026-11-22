#include "xsv/config.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <system_error>

namespace xsv {
namespace {

bool ends_with_tsv(const std::filesystem::path& path) {
  return path.extension() == ".tsv";
}

}  // namespace

Result<char> parse_delimiter(std::string_view text) {
  if (text == "\\t") return '\t';
  if (text.size() != 1) {
    return fail("Could not convert '" + std::string(text) +
                "' to a single ASCII character.");
  }
  const unsigned char byte = static_cast<unsigned char>(text.front());
  if (byte > 0x7F) {
    return fail("Could not convert '" + std::string(text) +
                "' to ASCII delimiter.");
  }
  return static_cast<char>(byte);
}

Result<Config> Config::from_path(std::optional<std::string_view> input) {
  Config config;
  if (input.has_value() && *input != "-") {
    config.path = std::filesystem::path(std::string(*input));
    if (ends_with_tsv(*config.path)) config.delimiter = '\t';
  }
  return config;
}

std::filesystem::path Config::index_path() const {
  std::filesystem::path path = *this->path;
  path += ".idx";
  return path;
}

bool toggled_no_headers(bool requested) {
  const char* value = std::getenv("XSV_TOGGLE_HEADERS");
  if (value != nullptr && std::string_view(value) == "1") return !requested;
  return requested;
}

Result<std::uint64_t> mtime_seconds(const std::filesystem::path& input_path) {
  struct stat info {};
  if (::stat(input_path.c_str(), &info) != 0) {
    return fail("failed to stat " + input_path.string() + ": " +
                std::system_category().message(errno));
  }
  return static_cast<std::uint64_t>(info.st_mtime);
}

}  // namespace xsv
