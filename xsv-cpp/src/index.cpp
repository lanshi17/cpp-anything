#include "xsv/index.hpp"

#include "xsv/mmap.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ostream>
#include <system_error>
#include <utility>

namespace xsv {
namespace {

std::uint64_t read_be64(const char* bytes) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) |
            static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]);
  }
  return value;
}

void write_be64(std::ostream& out, std::uint64_t value) {
  char bytes[8];
  for (int i = 7; i >= 0; --i) {
    bytes[i] = static_cast<char>(value & 0xFF);
    value >>= 8;
  }
  out.write(bytes, sizeof(bytes));
}

}  // namespace

Index::~Index() { reset(); }

Index::Index(Index&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      count_(std::exchange(other.count_, 0)) {}

Index& Index::operator=(Index&& other) noexcept {
  if (this == &other) return *this;
  reset();
  fd_ = std::exchange(other.fd_, -1);
  data_ = std::exchange(other.data_, nullptr);
  size_ = std::exchange(other.size_, 0);
  count_ = std::exchange(other.count_, 0);
  return *this;
}

void Index::reset() noexcept {
  if (data_ != nullptr) ::munmap(const_cast<char*>(data_), size_);
  if (fd_ != -1) ::close(fd_);
  fd_ = -1;
  data_ = nullptr;
  size_ = 0;
  count_ = 0;
}

Result<Index> Index::open(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    return fail("failed to open index " + path.string() + ": " +
                std::system_category().message(errno));
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const auto error = fail("failed to stat index " + path.string() + ": " +
                            std::system_category().message(errno));
    ::close(fd);
    return error;
  }
  const auto size = static_cast<std::size_t>(info.st_size);
  if (size < 8) {
    ::close(fd);
    return fail("invalid index file " + path.string() + ": too short");
  }
  void* address = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (address == MAP_FAILED) {
    const auto error = fail("failed to mmap index " + path.string() + ": " +
                            std::system_category().message(errno));
    ::close(fd);
    return error;
  }
  const char* data = static_cast<const char*>(address);
  const std::uint64_t count = read_be64(data + size - 8);
  if (count > size / 8 - 1) {
    ::munmap(address, size);
    ::close(fd);
    return fail("invalid index file " + path.string() +
                ": record count exceeds file size");
  }
  Index index;
  index.fd_ = fd;
  index.data_ = data;
  index.size_ = size;
  index.count_ = count;
  return index;
}

Result<std::uint64_t> Index::offset(std::uint64_t record) const {
  if (record >= count_) {
    return fail("invalid record index " + std::to_string(record) +
                " (there are " + std::to_string(count_) + " records)");
  }
  return read_be64(data_ + record * 8);
}

Status create_index(csv::Reader& reader, std::ostream& out,
                    const Mapping* mapping) {
  if (Status status = reader.prime(); !status) return status;

  std::uint64_t count = 0;
  std::uint64_t last_advised = 0;
  if (reader.has_headers() && !reader.headers().empty()) {
    write_be64(out, reader.headers().byte_offset);
    ++count;
  }

  csv::Record record;
  while (true) {
    Result<bool> next = reader.next(record);
    if (!next) return std::unexpected(next.error());
    if (!*next) break;
    write_be64(out, record.byte_offset);
    if (mapping != nullptr && record.byte_offset > last_advised + 8 * 1024 * 1024) {
      mapping->advise_dontneed(last_advised, record.byte_offset);
      last_advised = record.byte_offset;
    }
    ++count;
  }
  write_be64(out, count);
  out.flush();
  if (!out) return fail("failed to write index");
  return success();
}

}  // namespace xsv
