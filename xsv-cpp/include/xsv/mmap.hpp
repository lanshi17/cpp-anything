#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <vector>

#include "xsv/common.hpp"

namespace xsv {

// A read-only view of an input file. With XSV_USE_MMAP the bytes belong to the
// page cache; without it they belong to an owned buffer. Both paths expose the
// same span, so the parser never branches on the storage.
class Mapping {
 public:
  Mapping() = default;
  ~Mapping();
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&& other) noexcept;
  Mapping& operator=(Mapping&& other) noexcept;

  static Result<Mapping> open_file(const std::filesystem::path& path);
  static Result<Mapping> read_stream(std::istream& stream);
  static Mapping from_buffer(std::vector<char> bytes);

  [[nodiscard]] std::span<const char> bytes() const {
    return {data_, size_};
  }

  // Releases resident pages in [begin,end) back to the page cache. No-op for
  // an owned buffer. Used by sequential commands to keep RSS proportional to
  // the working window rather than the whole file.
  void advise_dontneed(std::uint64_t begin, std::uint64_t end) const noexcept;
  [[nodiscard]] bool empty() const { return size_ == 0; }

 private:
  void reset() noexcept;

  int fd_ = -1;
  const char* data_ = nullptr;
  std::size_t size_ = 0;
  std::vector<char> owned_;
};

}  // namespace xsv
