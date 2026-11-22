#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "xsv/csv.hpp"
#include "xsv/memory.hpp"

namespace xsv::commands::util {

// Packed row storage for commands that must hold the whole input (sort, join).
// Field bytes live in one contiguous buffer; offsets are stable even when the
// buffer grows.
class RecordStore {
 public:
  void reserve(std::size_t bytes, std::size_t fields, std::size_t records) {
    bytes_.reserve(bytes);
    starts_.reserve(fields);
    record_begin_.reserve(records);
    record_end_.reserve(records);
  }

  void add(const csv::Record& record) {
    record_begin_.push_back(starts_.size());
    for (const std::string_view field : record.fields) {
      starts_.push_back(bytes_.size());
      bytes_.insert(bytes_.end(), field.begin(), field.end());
    }
    record_end_.push_back(bytes_.size());
    if (starts_.size() > record_begin_.back()) {
      // The final field's end is the record end; intermediate ends are the
      // next field's start.
    }
  }

  [[nodiscard]] std::size_t size() const { return record_begin_.size(); }

  [[nodiscard]] std::size_t field_count(std::size_t record) const {
    return (record + 1 < record_begin_.size() ? record_begin_[record + 1]
                                              : starts_.size()) -
           record_begin_[record];
  }

  [[nodiscard]] std::string_view field(std::size_t record,
                                       std::size_t index) const {
    const std::size_t slot = record_begin_[record] + index;
    const std::size_t begin = starts_[slot];
    const std::size_t end =
        (index + 1 < field_count(record)) ? starts_[slot + 1] : record_end_[record];
    return std::string_view(bytes_.data() + begin, end - begin);
  }

  void write(csv::Writer& writer, std::size_t record,
             std::vector<std::string_view>& scratch) const {
    scratch.clear();
    const std::size_t count = field_count(record);
    scratch.reserve(count);
    for (std::size_t i = 0; i < count; ++i) scratch.push_back(field(record, i));
    writer.write_record(scratch);
  }

 private:
#if XSV_USE_PMR
  memory::Arena arena_;
  using Bytes = std::pmr::vector<char>;
  using Offsets = std::vector<std::size_t>;
  Bytes bytes_{arena_.resource()};
  Offsets starts_;
  Offsets record_begin_;
  Offsets record_end_;
#else
  using Bytes = std::vector<char>;
  using Offsets = std::vector<std::size_t>;
  Bytes bytes_;
  Offsets starts_;
  Offsets record_begin_;
  Offsets record_end_;
#endif
};

}  // namespace xsv::commands::util
