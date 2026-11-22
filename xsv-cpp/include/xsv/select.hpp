#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/common.hpp"
#include "xsv/csv.hpp"

namespace xsv {

// xsv's column selection language: comma-separated selectors, inclusive
// ranges, reverse ranges, open-ended ranges, `!` inversion, 1-based indices,
// header names, duplicate-name disambiguation with `Name[1]`, and quoted names.
class Selection {
 public:
  Selection() = default;
  explicit Selection(std::vector<std::size_t> indices)
      : indices_(std::move(indices)) {}

  static Result<Selection> parse(std::string_view spec,
                                 std::span<const std::string_view> headers,
                                 bool use_names);

  [[nodiscard]] const std::vector<std::size_t>& indices() const {
    return indices_;
  }
  [[nodiscard]] std::size_t size() const { return indices_.size(); }
  [[nodiscard]] bool empty() const { return indices_.empty(); }

  // Applies the selection to one record. Indices are validated when the
  // Selection is built, so this is a pure projection.
  [[nodiscard]] std::vector<std::string_view> apply(
      const csv::Record& record) const;

 private:
  std::vector<std::size_t> indices_;
};

}  // namespace xsv
