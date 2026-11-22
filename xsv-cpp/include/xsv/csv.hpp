#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/common.hpp"

namespace xsv::csv {

struct Record {
  std::vector<std::string_view> fields;
  std::uint64_t byte_offset = 0;
  std::uint64_t line = 1;

  void clear() { fields.clear(); }
  [[nodiscard]] std::size_t size() const { return fields.size(); }
  [[nodiscard]] bool empty() const { return fields.empty(); }
  std::string_view operator[](std::size_t i) const { return fields[i]; }
};

struct ReaderOptions {
  char delimiter = ',';
  char quote = '"';
  bool double_quote = true;
  std::optional<char> escape;
  bool flexible = false;
  bool has_headers = true;
  bool quoting = true;
};

// A byte-oriented CSV reader over a contiguous buffer. Fields are views into
// the buffer; only quoted fields that need unescaping are copied, into a
// per-reader scratch area that stays valid until the next call to next().
class Reader {
 public:
  Reader(std::span<const char> data, ReaderOptions options);

  // Parses the header record (when has_headers). Idempotent. Must be called
  // before headers(); next() calls it automatically.
  Status prime();

  // Reads the next data record. Returns false at EOF.
  Result<bool> next(Record& out);

  // Repositions to a byte offset produced by the index. Headers stay loaded;
  // the next call to next() parses from the offset.
  Status seek(std::uint64_t byte_offset);

  [[nodiscard]] const Record& headers() const { return headers_; }
  [[nodiscard]] bool has_headers() const { return options_.has_headers; }
  [[nodiscard]] std::uint64_t line() const { return line_; }
  [[nodiscard]] std::uint64_t byte_offset() const { return pos_; }

 private:
  struct RawField {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t tail_begin = 0;
    std::size_t tail_end = 0;
    bool quoted = false;
    bool has_tail = false;
  };

  Status parse_record(Record& out, bool is_header, std::size_t record_start);
  void consume_terminator();
  void skip_empty_lines();
  Result<std::string_view> materialize(const RawField& raw);

  std::span<const char> data_;
  ReaderOptions options_;
  std::size_t pos_ = 0;
  std::uint64_t line_ = 1;
  bool primed_ = false;
  bool eof_ = false;
  bool has_bom_ = false;
  std::uint64_t record_index_ = 0;  // records parsed, header included
  std::uint64_t data_records_ = 0;  // data records consumed, header excluded
  std::size_t expected_fields_ = 0;
  Record headers_;
  std::vector<RawField> raw_;
  std::vector<char> scratch_;
};

struct WriterOptions {
  char delimiter = ',';
  std::string_view terminator = "\n";
  char quote = '"';
  bool double_quote = true;
  char escape = '\\';
};

// RFC 4180 writer with the reference tool's "necessary" quoting rule. A record
// consisting of one empty field is written as `""` so it is not confused with
// a blank line.
class Writer {
 public:
  Writer(std::ostream& out, WriterOptions options = {});

  Status write_record(std::span<const std::string_view> fields);
  Status flush();

 private:
  std::ostream& out_;
  WriterOptions options_;
  std::string buffer_;
};

}  // namespace xsv::csv
