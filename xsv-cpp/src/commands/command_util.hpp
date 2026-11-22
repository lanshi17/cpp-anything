#pragma once

#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/mmap.hpp"

namespace xsv::commands::util {

inline Result<Mapping> open_input(const Config& config) {
  if (config.path) return Mapping::open_file(*config.path);
  return Mapping::read_stream(std::cin);
}

inline csv::ReaderOptions reader_options(const Config& config, bool has_headers) {
  csv::ReaderOptions options;
  options.delimiter = config.delimiter;
  options.quote = config.quote;
  options.double_quote = config.double_quote;
  options.escape = config.escape;
  options.flexible = config.flexible;
  options.has_headers = has_headers;
  options.quoting = config.quoting;
  return options;
}

// Reads the first record without consuming it from a data reader. Used when
// --no-headers is set: the reference still needs the first row for selection
// and header-width purposes, but the row also remains part of the data.
inline Result<csv::Record> peek_first(std::span<const char> bytes,
                                      csv::ReaderOptions options) {
  options.has_headers = false;
  csv::Reader reader(bytes, options);
  csv::Record record;
  auto next = reader.next(record);
  if (!next) return std::unexpected(next.error());
  if (!*next) record.clear();
  return record;
}

inline char output_delimiter(const std::optional<std::string>& output) {
  if (output.has_value() && *output != "-") {
    const std::filesystem::path path(*output);
    if (path.extension() == ".tsv") return '\t';
  }
  return ',';
}

// Drops already-consumed pages from a mapped file every 32 MiB. Commands that
// only stream forward call reach(record.byte_offset) after processing a record.
class SequentialAdvice {
 public:
  explicit SequentialAdvice(const Mapping& mapping) : mapping_(mapping) {}

  void reach(std::uint64_t offset) {
    constexpr std::uint64_t kWindow = 8 * 1024 * 1024;
    if (offset > last_ + kWindow) {
      mapping_.advise_dontneed(last_, offset);
      last_ = offset;
    }
  }

 private:
  const Mapping& mapping_;
  std::uint64_t last_ = 0;
};

inline csv::WriterOptions writer_options(char delimiter) {
  csv::WriterOptions options;
  options.delimiter = delimiter;
  return options;
}

}  // namespace xsv::commands::util
