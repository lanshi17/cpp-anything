#include "xsv/csv.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <utility>

#include "xsv/simd.hpp"

namespace xsv::csv {
namespace {

bool is_terminator(char c) { return c == '\n' || c == '\r'; }

std::string record_error(std::uint64_t record, std::uint64_t line,
                         std::uint64_t byte, std::size_t found,
                         std::size_t expected) {
  return "CSV error: record " + std::to_string(record) +
         " (line: " + std::to_string(line) +
         ", byte: " + std::to_string(byte) + "): found record with " +
         std::to_string(found) + " fields, but the previous record has " +
         std::to_string(expected) + " fields";
}

}  // namespace

Reader::Reader(std::span<const char> data, ReaderOptions options)
    : data_(data), options_(options) {
  // The reference reader strips a UTF-8 BOM before the first record.
  if (data_.size() >= 3 && static_cast<unsigned char>(data_[0]) == 0xEF &&
      static_cast<unsigned char>(data_[1]) == 0xBB &&
      static_cast<unsigned char>(data_[2]) == 0xBF) {
    has_bom_ = true;
    pos_ = 3;
  }
}

void Reader::consume_terminator() {
  if (pos_ >= data_.size()) return;
  // The reference parser treats CR and LF as independent terminators. A CRLF
  // therefore ends the record at the CR; the following LF is skipped as an
  // empty line before the next record. This is observable in the index: the
  // next record's byte offset points at the LF, not past it.
  ++pos_;
  ++line_;
}

void Reader::skip_empty_lines() {
  while (pos_ < data_.size() && is_terminator(data_[pos_])) {
    if (data_[pos_] == '\n' && pos_ > 0 && data_[pos_ - 1] == '\r') {
      ++pos_;  // paired LF of CRLF: the line was already counted at the CR
      continue;
    }
    consume_terminator();
  }
}

Status Reader::parse_record(Record& out, bool is_header, std::size_t record_start) {
  out.fields.clear();
  out.byte_offset = (record_index_ == 0 && has_bom_) ? 0 : record_start;
  out.line = line_;
  raw_.clear();

  if (pos_ >= data_.size()) return success();

  for (;;) {
    RawField raw;
    if (options_.quoting && pos_ < data_.size() &&
        data_[pos_] == options_.quote) {
      raw.quoted = true;
      ++pos_;
      raw.begin = pos_;
      bool closed = false;
      while (pos_ < data_.size()) {
        const char c = data_[pos_];
        if (options_.escape && c == *options_.escape) {
          if (pos_ + 1 < data_.size()) {
            if (data_[pos_ + 1] == '\n') ++line_;
            pos_ += 2;
          } else {
            ++pos_;
          }
          continue;
        }
        if (c == options_.quote) {
          if (options_.double_quote && pos_ + 1 < data_.size() &&
              data_[pos_ + 1] == options_.quote) {
            pos_ += 2;
            continue;
          }
          raw.end = pos_;
          ++pos_;
          closed = true;
          break;
        }
        if (c == '\n') {
          ++line_;
        } else if (c == '\r' &&
                   !(pos_ + 1 < data_.size() && data_[pos_ + 1] == '\n')) {
          ++line_;
        }
        ++pos_;
      }
      if (!closed) {
        return fail("CSV error: EOF inside quoted field");
      }
      raw.tail_begin = pos_;
      const std::size_t tail = simd::find_field_end(
          data_.subspan(pos_), options_.delimiter);
      pos_ = tail == npos ? data_.size() : pos_ + tail;
      raw.tail_end = pos_;
      raw.has_tail = raw.tail_end > raw.tail_begin;
    } else {
      raw.begin = pos_;
      const std::size_t end = simd::find_field_end(
          data_.subspan(pos_), options_.delimiter);
      pos_ = end == npos ? data_.size() : pos_ + end;
      raw.end = pos_;
    }
    raw_.push_back(raw);

    if (pos_ >= data_.size()) break;
    if (data_[pos_] == options_.delimiter) {
      ++pos_;
      continue;
    }
    consume_terminator();
    break;
  }

  if (!options_.flexible) {
    if (is_header) {
      expected_fields_ = raw_.size();
    } else if (expected_fields_ == 0) {
      expected_fields_ = raw_.size();
    } else if (expected_fields_ != raw_.size()) {
      return fail(record_error(data_records_ + 1, out.line, out.byte_offset,
                               raw_.size(), expected_fields_));
    }
  }

  std::size_t needed = 0;
  for (const RawField& raw : raw_) {
    if (!raw.quoted) continue;
    needed += raw.end - raw.begin;
    if (raw.has_tail) needed += raw.tail_end - raw.tail_begin;
  }
  scratch_.clear();
  if (needed > 0) scratch_.reserve(needed);

  out.fields.reserve(raw_.size());
  for (const RawField& raw : raw_) {
    if (!raw.quoted) {
      out.fields.emplace_back(data_.data() + raw.begin, raw.end - raw.begin);
      continue;
    }
    const std::size_t start = scratch_.size();
    std::size_t at = raw.begin;
    while (at < raw.end) {
      const char c = data_[at];
      if (options_.escape && c == *options_.escape && at + 1 < raw.end) {
        scratch_.push_back(data_[at + 1]);
        at += 2;
      } else if (options_.double_quote && c == options_.quote &&
                 at + 1 < raw.end && data_[at + 1] == options_.quote) {
        scratch_.push_back(options_.quote);
        at += 2;
      } else {
        scratch_.push_back(c);
        ++at;
      }
    }
    if (raw.has_tail) {
      scratch_.insert(scratch_.end(), data_.begin() + raw.tail_begin,
                      data_.begin() + raw.tail_end);
    }
    const std::size_t length = scratch_.size() - start;
    if (length == 0) {
      out.fields.emplace_back(data_.data() + raw.tail_end, 0);
    } else {
      out.fields.emplace_back(scratch_.data() + start, length);
    }
  }
  return success();
}

Status Reader::seek(std::uint64_t byte_offset) {
  pos_ = static_cast<std::size_t>(byte_offset);
  line_ = 1;
  eof_ = false;
  primed_ = true;
  data_records_ = 0;
  record_index_ = options_.has_headers ? 1 : 0;
  expected_fields_ = options_.has_headers ? headers_.fields.size() : 0;
  raw_.clear();
  scratch_.clear();
  return success();
}

Status Reader::prime() {
  if (primed_) return success();
  primed_ = true;
  if (!options_.has_headers) return success();

  const std::size_t record_start = pos_;
  skip_empty_lines();
  if (pos_ >= data_.size()) {
    headers_.clear();
    eof_ = true;
    return success();
  }
  Status status = parse_record(headers_, true, record_start);
  if (!status) return status;
  ++record_index_;
  return success();
}

Result<bool> Reader::next(Record& out) {
  if (!primed_) {
    if (Status status = prime(); !status) return std::unexpected(status.error());
  }
  if (eof_) return false;
  const std::size_t record_start = pos_;
  skip_empty_lines();
  if (pos_ >= data_.size()) {
    eof_ = true;
    return false;
  }
  Status status = parse_record(out, false, record_start);
  if (!status) return std::unexpected(status.error());
  ++record_index_;
  ++data_records_;
  return true;
}

Writer::Writer(std::ostream& out, WriterOptions options)
    : out_(out), options_(options) {}

Status Writer::write_record(std::span<const std::string_view> fields) {
  buffer_.clear();
  if (fields.empty()) {
    // The reference writer represents a zero-field record as one quoted empty
    // field, so it is distinguishable from a blank line.
    buffer_.push_back(options_.quote);
    buffer_.push_back(options_.quote);
    buffer_.append(options_.terminator);
    out_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    if (!out_) return fail("failed to write CSV output");
    return success();
  }
  const bool single_empty = fields.size() == 1 && fields.front().empty();
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) buffer_.push_back(options_.delimiter);
    const std::string_view field = fields[i];
    bool quote = single_empty;
    if (!quote) {
      for (const char c : field) {
        if (c == options_.delimiter || c == options_.quote || c == '\r' ||
            c == '\n' ||
            (!options_.double_quote && c == options_.escape)) {
          quote = true;
          break;
        }
      }
    }
    if (!quote) {
      buffer_.append(field);
      continue;
    }
    buffer_.push_back(options_.quote);
    for (const char c : field) {
      if (c == options_.quote) {
        if (options_.double_quote) {
          buffer_.push_back(options_.quote);
          buffer_.push_back(options_.quote);
        } else {
          buffer_.push_back(options_.escape);
          buffer_.push_back(options_.quote);
        }
      } else if (!options_.double_quote && c == options_.escape) {
        buffer_.push_back(options_.escape);
        buffer_.push_back(options_.escape);
      } else {
        buffer_.push_back(c);
      }
    }
    buffer_.push_back(options_.quote);
  }
  buffer_.append(options_.terminator);
  out_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
  if (!out_) return fail("failed to write CSV output");
  return success();
}

Status Writer::flush() {
  out_.flush();
  if (!out_) return fail("failed to flush CSV output");
  return success();
}

}  // namespace xsv::csv
