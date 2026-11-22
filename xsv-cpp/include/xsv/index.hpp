#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>

#include "xsv/common.hpp"
#include "xsv/csv.hpp"

namespace xsv {

class Mapping;

// BurntSushi/xsv stores a simple random-access index next to the CSV file:
// N+1 big-endian uint64 values, one byte offset per record (header included)
// followed by the record count. The format is frozen upstream; see
// docs/COMPATIBILITY.md for the full layout and validation rules.
class Index {
 public:
  Index() = default;
  ~Index();
  Index(const Index&) = delete;
  Index& operator=(const Index&) = delete;
  Index(Index&& other) noexcept;
  Index& operator=(Index&& other) noexcept;

  static Result<Index> open(const std::filesystem::path& path);

  [[nodiscard]] std::uint64_t count() const { return count_; }
  [[nodiscard]] Result<std::uint64_t> offset(std::uint64_t record) const;

 private:
  void reset() noexcept;

  int fd_ = -1;
  const char* data_ = nullptr;
  std::size_t size_ = 0;
  std::uint64_t count_ = 0;
};

// Reads the reader to EOF and writes the index. Includes the header record,
// because the reference format indexes every record.
Status create_index(csv::Reader& reader, std::ostream& out,
                    const Mapping* mapping = nullptr);

}  // namespace xsv
