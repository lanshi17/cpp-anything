#include "xsv/mmap.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <istream>
#include <iterator>
#include <system_error>
#include <utility>

namespace xsv {
namespace {

std::unexpected<Error> system_error(const char* what,
                                    const std::filesystem::path& path) {
  return fail(std::string(what) + " " + path.string() + ": " +
              std::system_category().message(errno));
}

}  // namespace

Mapping::~Mapping() { reset(); }

Mapping::Mapping(Mapping&& other) noexcept
    : fd_(other.fd_),
      data_(other.data_),
      size_(other.size_),
      owned_(std::move(other.owned_)) {
  other.fd_ = -1;
  other.data_ = nullptr;
  other.size_ = 0;
}

Mapping& Mapping::operator=(Mapping&& other) noexcept {
  if (this == &other) return *this;
  reset();
  fd_ = std::exchange(other.fd_, -1);
  data_ = std::exchange(other.data_, nullptr);
  size_ = std::exchange(other.size_, 0);
  owned_ = std::move(other.owned_);
  return *this;
}

void Mapping::reset() noexcept {
  if (data_ != nullptr && fd_ != -1 && owned_.empty()) {
    ::munmap(const_cast<char*>(data_), size_);
  }
  if (fd_ != -1) ::close(fd_);
  fd_ = -1;
  data_ = nullptr;
  size_ = 0;
  owned_.clear();
}

Mapping Mapping::from_buffer(std::vector<char> bytes) {
  Mapping mapping;
  mapping.owned_ = std::move(bytes);
  mapping.data_ = mapping.owned_.data();
  mapping.size_ = mapping.owned_.size();
  return mapping;
}

Result<Mapping> Mapping::read_stream(std::istream& stream) {
  std::vector<char> bytes;
  char chunk[64 * 1024];
  while (stream) {
    stream.read(chunk, sizeof(chunk));
    const std::streamsize got = stream.gcount();
    if (got > 0) bytes.insert(bytes.end(), chunk, chunk + got);
  }
  if (stream.bad()) return fail("failed to read input stream");
  return from_buffer(std::move(bytes));
}

void Mapping::advise_dontneed(std::uint64_t begin,
                              std::uint64_t end) const noexcept {
#if XSV_USE_MMAP
  if (data_ == nullptr || begin >= end || begin >= size_) return;
  const long page = ::sysconf(_SC_PAGESIZE);
  if (page <= 0) return;
  const std::uint64_t mask = static_cast<std::uint64_t>(page) - 1;
  begin &= ~mask;
  end = std::min<std::uint64_t>(end, size_);
  end = (end + mask) & ~mask;
  if (end > begin) {
    ::madvise(const_cast<char*>(data_) + begin,
              static_cast<std::size_t>(end - begin), MADV_DONTNEED);
  }
#else
  (void)begin;
  (void)end;
#endif
}

Result<Mapping> Mapping::open_file(const std::filesystem::path& path) {
#if XSV_USE_MMAP
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) return system_error("failed to open", path);

  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const auto error = system_error("failed to stat", path);
    ::close(fd);
    return error;
  }
  if (info.st_size == 0) {
    Mapping mapping;
    mapping.fd_ = fd;
    return mapping;
  }
  void* address = ::mmap(nullptr, static_cast<std::size_t>(info.st_size),
                         PROT_READ, MAP_PRIVATE, fd, 0);
  if (address == MAP_FAILED) {
    const auto error = system_error("failed to mmap", path);
    ::close(fd);
    return error;
  }
  Mapping mapping;
  mapping.fd_ = fd;
  mapping.data_ = static_cast<const char*>(address);
  mapping.size_ = static_cast<std::size_t>(info.st_size);
  return mapping;
#else
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return fail("failed to open " + path.string() + ": " +
                std::system_category().message(errno));
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
  if (file.bad()) return fail("failed to read " + path.string());
  return from_buffer(std::move(bytes));
#endif
}

}  // namespace xsv
