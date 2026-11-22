#include "command_util.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>
#include <charconv>
#include <system_error>
#include <utility>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/commands.hpp"
#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/format.hpp"
#include "xsv/index.hpp"
#include "xsv/parallel.hpp"
#include "xsv/select.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Computes basic statistics on CSV data.

Basic statistics includes mean, median, mode, standard deviation, sum, max and
min values. Note that some statistics are expensive to compute, so they must
be enabled explicitly. By default, the following statistics are reported for
*every* column in the CSV data: mean, max, min and standard deviation. The
default set of statistics corresponds to statistics that can be computed
efficiently on a stream of data (i.e., constant memory).

Computing statistics on a large file can be made much faster if you create
an index for it first with 'xsv index'.

Usage:
    xsv stats [options] [<input>]

stats options:
    -s, --select <arg>     Select a subset of columns to compute stats for.
                           See 'xsv select --help' for the format details.
                           This is provided here because piping 'xsv select'
                           into 'xsv stats' will disable the use of indexing.
    --everything           Show all statistics available.
    --mode                 Show the mode.
                           This requires storing all CSV data in memory.
    --cardinality          Show the cardinality.
                           This requires storing all CSV data in memory.
    --median               Show the median.
                           This requires storing all CSV data in memory.
    --nulls                Include NULLs in the population size for computing
                           mean and standard deviation.
    -j, --jobs <arg>       The number of jobs to run in parallel.
                           This works better when the given CSV data has
                           an index already created. Note that a file handle
                           is opened for each job.
                           When set to '0', the number of jobs is set to the
                           number of CPUs detected.
                           [default: 0]

Common options:
    -h, --help             Display this message
    -o, --output <file>    Write output to <file> instead of stdout.
    -n, --no-headers       When set, the first row will NOT be interpreted
                           as column names. i.e., They will be included
                           in statistics.
    -d, --delimiter <arg>  The field delimiter for reading CSV data.
                           Must be a single character. (default: ,)
)";

struct Args {
  std::optional<std::string> input;
  std::optional<std::string> output;
  std::optional<std::string> select;
  bool everything = false;
  bool mode = false;
  bool cardinality = false;
  bool median = false;
  bool nulls = false;
  bool no_headers = false;
  unsigned jobs = 0;
  std::optional<char> delimiter;
  bool help = false;
};

Result<unsigned> parse_unsigned(std::string_view text) {
  unsigned value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return fail("invalid digit found in string");
  }
  return value;
}

Result<Args> parse_args(std::span<const std::string_view> argv) {
  Args args;
  bool positional_only = false;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string_view arg = argv[i];
    if (!positional_only && arg == "--") {
      positional_only = true;
      continue;
    }
    if (!positional_only && (arg == "-h" || arg == "--help")) {
      args.help = true;
      continue;
    }
    if (!positional_only && (arg == "-n" || arg == "--no-headers")) {
      args.no_headers = true;
      continue;
    }
    if (!positional_only && arg == "--everything") {
      args.everything = true;
      continue;
    }
    if (!positional_only && arg == "--mode") {
      args.mode = true;
      continue;
    }
    if (!positional_only && arg == "--cardinality") {
      args.cardinality = true;
      continue;
    }
    if (!positional_only && arg == "--median") {
      args.median = true;
      continue;
    }
    if (!positional_only && arg == "--nulls") {
      args.nulls = true;
      continue;
    }
    if (!positional_only && (arg == "-s" || arg == "--select")) {
      if (i + 1 >= argv.size()) {
        return fail("The argument '--select <arg>' requires a value but none "
                    "was supplied.");
      }
      args.select = std::string(argv[++i]);
      continue;
    }
    if (!positional_only && arg.starts_with("--select=")) {
      args.select = std::string(arg.substr(9));
      continue;
    }
    if (!positional_only && (arg == "-j" || arg == "--jobs")) {
      if (i + 1 >= argv.size()) {
        return fail("The argument '--jobs <arg>' requires a value but none "
                    "was supplied.");
      }
      auto value = parse_unsigned(argv[++i]);
      if (!value) return std::unexpected(value.error());
      args.jobs = *value;
      continue;
    }
    if (!positional_only && arg.starts_with("--jobs=")) {
      auto value = parse_unsigned(arg.substr(7));
      if (!value) return std::unexpected(value.error());
      args.jobs = *value;
      continue;
    }
    if (!positional_only && (arg == "-o" || arg == "--output")) {
      if (i + 1 >= argv.size()) {
        return fail("The argument '--output <file>' requires a value but none "
                    "was supplied.");
      }
      args.output = std::string(argv[++i]);
      continue;
    }
    if (!positional_only && arg.starts_with("--output=")) {
      args.output = std::string(arg.substr(9));
      continue;
    }
    if (!positional_only && (arg == "-d" || arg == "--delimiter")) {
      if (i + 1 >= argv.size()) {
        return fail("The argument '--delimiter <arg>' requires a value but "
                    "none was supplied.");
      }
      auto delimiter = parse_delimiter(argv[++i]);
      if (!delimiter) return std::unexpected(delimiter.error());
      args.delimiter = *delimiter;
      continue;
    }
    if (!positional_only && arg.starts_with("--delimiter=")) {
      auto delimiter = parse_delimiter(arg.substr(12));
      if (!delimiter) return std::unexpected(delimiter.error());
      args.delimiter = *delimiter;
      continue;
    }
    if (!positional_only && arg.starts_with("-") && arg != "-") {
      return fail("Unknown flag: " + std::string(arg));
    }
    if (args.input.has_value()) return fail("xsv stats takes at most one input path.");
    args.input = std::string(arg);
  }
  return args;
}

bool is_valid_utf8(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    std::size_t extra = 0;
    unsigned codepoint = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      codepoint = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      codepoint = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      codepoint = c & 0x07;
    } else {
      return false;
    }
    if (i + extra >= text.size()) return false;
    for (std::size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (cc & 0x3F);
    }
    if ((extra == 1 && codepoint < 0x80) ||
        (extra == 2 && codepoint < 0x800) ||
        (extra == 3 && codepoint < 0x10000) ||
        codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

enum class FieldType { kUnknown, kNull, kUnicode, kFloat, kInteger };

FieldType merge_type(FieldType left, FieldType right) {
  if (left == FieldType::kUnknown || right == FieldType::kUnknown) {
    return FieldType::kUnknown;
  }
  if (left == FieldType::kNull) return right;
  if (right == FieldType::kNull) return left;
  if (left == FieldType::kUnicode || right == FieldType::kUnicode) {
    return FieldType::kUnicode;
  }
  if (left == FieldType::kFloat || right == FieldType::kFloat) {
    return FieldType::kFloat;
  }
  return FieldType::kInteger;
}

FieldType type_of(std::string_view sample) {
  if (sample.empty()) return FieldType::kNull;
  if (!is_valid_utf8(sample)) return FieldType::kUnknown;
  if (parse_i64(sample).has_value()) return FieldType::kInteger;
  if (parse_f64(sample).has_value()) return FieldType::kFloat;
  return FieldType::kUnicode;
}

bool is_number(FieldType type) {
  return type == FieldType::kFloat || type == FieldType::kInteger;
}

std::string type_name(FieldType type) {
  switch (type) {
    case FieldType::kUnknown:
      return "Unknown";
    case FieldType::kNull:
      return "NULL";
    case FieldType::kUnicode:
      return "Unicode";
    case FieldType::kFloat:
      return "Float";
    case FieldType::kInteger:
      return "Integer";
  }
  return "Unknown";
}

std::string lossy_utf8(std::string_view bytes) {
  // The reference uses String::from_utf8_lossy. Valid UTF-8 is returned as-is;
  // invalid sequences become U+FFFD. This simple decoder handles the common
  // replacement cases without pulling in a Unicode library.
  if (is_valid_utf8(bytes)) return std::string(bytes);
  std::string out;
  std::size_t i = 0;
  while (i < bytes.size()) {
    const unsigned char c = static_cast<unsigned char>(bytes[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    std::size_t extra = 0;
    if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;
    bool valid = extra != 0 && i + extra < bytes.size();
    if (valid) {
      for (std::size_t k = 1; k <= extra; ++k) {
        if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80) {
          valid = false;
          break;
        }
      }
    }
    if (valid) {
      out.append(bytes.substr(i, extra + 1));
      i += extra + 1;
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  return out;
}

std::int64_t saturating_i64(double value) {
  if (std::isnan(value)) return 0;
  if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(value);
}

class Accumulator {
 public:
  Accumulator(bool nulls, bool want_mode, bool want_cardinality,
              bool want_median)
      : include_nulls_(nulls),
        want_mode_(want_mode),
        want_cardinality_(want_cardinality),
        want_median_(want_median) {}

  void add(std::string_view sample) {
    const FieldType sample_type = type_of(sample);
    type_ = merge_type(type_, sample_type);

    add_sum(sample);
    add_minmax(sample);

    if (want_mode_) mode_values_.emplace_back(sample);
    if (want_cardinality_) ++counts_[std::string(sample)];
    if (want_median_ && is_number(type_) && !sample.empty()) {
      auto value = parse_f64(sample);
      if (value) medians_.push_back(*value);
    }

    if (type_ == FieldType::kNull) {
      if (include_nulls_) add_online(0.0);
      return;
    }
    if (type_ == FieldType::kFloat || type_ == FieldType::kInteger) {
      if (sample.empty()) {
        if (include_nulls_) add_online(0.0);
      } else {
        auto value = parse_f64(sample);
        if (value) add_online(*value);
      }
    }
  }

  void merge(const Accumulator& other) {
    type_ = merge_type(type_, other.type_);

    if (other.has_float_) {
      if (!has_float_) {
        has_float_ = true;
        sum_float_ = static_cast<double>(std::bit_cast<std::int64_t>(sum_bits_)) +
                     other.sum_float_;
      } else {
        sum_float_ += other.sum_float_;
      }
    } else if (other.sum_bits_ != 0) {
      if (has_float_) {
        sum_float_ += static_cast<double>(std::bit_cast<std::int64_t>(other.sum_bits_));
      } else {
        sum_bits_ += other.sum_bits_;
      }
    }

    if (other.has_length_) {
      if (!has_length_) {
        has_length_ = true;
        min_length_ = other.min_length_;
        max_length_ = other.max_length_;
      } else {
        min_length_ = std::min(min_length_, other.min_length_);
        max_length_ = std::max(max_length_, other.max_length_);
      }
    }
    if (other.has_strings_) {
      if (!has_strings_) {
        has_strings_ = true;
        min_string_ = other.min_string_;
        max_string_ = other.max_string_;
      } else {
        min_string_ = std::min(min_string_, other.min_string_);
        max_string_ = std::max(max_string_, other.max_string_);
      }
    }
    if (other.has_int_minmax_) {
      if (!has_int_minmax_) {
        has_int_minmax_ = true;
        min_int_ = other.min_int_;
        max_int_ = other.max_int_;
      } else {
        min_int_ = std::min(min_int_, other.min_int_);
        max_int_ = std::max(max_int_, other.max_int_);
      }
    }
    if (other.has_float_minmax_) {
      if (!has_float_minmax_) {
        has_float_minmax_ = true;
        min_float_ = other.min_float_;
        max_float_ = other.max_float_;
      } else {
        if (other.min_float_ < min_float_) min_float_ = other.min_float_;
        if (other.max_float_ > max_float_) max_float_ = other.max_float_;
      }
    }

    if (other.count_ != 0) {
      if (count_ == 0) {
        count_ = other.count_;
        mean_ = other.mean_;
        variance_ = other.variance_;
      } else {
        const double s1 = static_cast<double>(count_);
        const double s2 = static_cast<double>(other.count_);
        const double mean = (s1 * mean_ + s2 * other.mean_) / (s1 + s2);
        const double delta = mean_ - other.mean_;
        const double variance =
            (s1 * variance_ + s2 * other.variance_) / (s1 + s2) +
            (s1 * s2 * delta * delta) / ((s1 + s2) * (s1 + s2));
        count_ += other.count_;
        mean_ = mean;
        variance_ = variance;
      }
    }

    for (const auto& [value, count] : other.counts_) {
      counts_[value] += count;
    }
    mode_values_.insert(mode_values_.end(), other.mode_values_.begin(),
                        other.mode_values_.end());
    medians_.insert(medians_.end(), other.medians_.begin(), other.medians_.end());
  }

  [[nodiscard]]std::vector<std::string> record() const {
    std::vector<std::string> out;
    out.push_back(type_name(type_));
    out.push_back(sum_text());
    const auto minmax = minmax_text();
    out.push_back(minmax.first);
    out.push_back(minmax.second);
    const auto lengths = length_text();
    out.push_back(lengths.first);
    out.push_back(lengths.second);
    if (is_number(type_)) {
      out.push_back(format_f64(mean_));
      out.push_back(format_f64(std::sqrt(variance_)));
    } else {
      out.push_back("");
      out.push_back("");
    }
    if (want_median_) {
      out.push_back(median_text());
    }
    if (want_mode_) {
      out.push_back(mode_text());
    }
    if (want_cardinality_) {
      out.push_back(std::to_string(counts_.size()));
    }
    return out;
  }

 private:
  void add_sum(std::string_view sample) {
    if (sample.empty()) return;
    if (type_ == FieldType::kFloat) {
      auto value = parse_f64(sample);
      if (!value) return;
      if (!has_float_) {
        has_float_ = true;
        sum_float_ = static_cast<double>(std::bit_cast<std::int64_t>(sum_bits_)) +
                     *value;
      } else {
        sum_float_ += *value;
      }
    } else if (type_ == FieldType::kInteger) {
      auto value = parse_i64(sample);
      if (!value) return;
      if (has_float_) {
        sum_float_ += static_cast<double>(*value);
      } else {
        sum_bits_ += static_cast<std::uint64_t>(*value);
      }
    }
  }

  void add_minmax(std::string_view sample) {
    const std::size_t length = sample.size();
    if (!has_length_) {
      has_length_ = true;
      min_length_ = max_length_ = length;
    } else {
      min_length_ = std::min(min_length_, length);
      max_length_ = std::max(max_length_, length);
    }
    if (sample.empty()) return;
    if (!has_strings_) {
      has_strings_ = true;
      min_string_ = max_string_ = std::string(sample);
    } else {
      if (sample < min_string_) min_string_ = std::string(sample);
      if (sample > max_string_) max_string_ = std::string(sample);
    }
    if (type_ == FieldType::kFloat) {
      auto value = parse_f64(sample);
      if (!value) return;
      add_float_minmax(*value);
      add_int_minmax(saturating_i64(*value));
    } else if (type_ == FieldType::kInteger) {
      auto value = parse_i64(sample);
      if (!value) return;
      add_int_minmax(*value);
      add_float_minmax(static_cast<double>(*value));
    }
  }

  void add_int_minmax(std::int64_t value) {
    if (!has_int_minmax_) {
      has_int_minmax_ = true;
      min_int_ = max_int_ = value;
    } else {
      min_int_ = std::min(min_int_, value);
      max_int_ = std::max(max_int_, value);
    }
  }

  void add_float_minmax(double value) {
    if (!has_float_minmax_) {
      has_float_minmax_ = true;
      min_float_ = max_float_ = value;
    } else {
      if (value < min_float_) min_float_ = value;
      if (value > max_float_) max_float_ = value;
    }
  }

  void add_online(double value) {
    const double old_mean = mean_;
    const double previous = variance_ * static_cast<double>(count_);
    ++count_;
    mean_ += (value - old_mean) / static_cast<double>(count_);
    variance_ = (previous + (value - old_mean) * (value - mean_)) /
                static_cast<double>(count_);
  }

  [[nodiscard]]std::string sum_text() const {
    if (type_ == FieldType::kInteger) {
      return format_i64(std::bit_cast<std::int64_t>(sum_bits_));
    }
    if (type_ == FieldType::kFloat) return format_f64(sum_float_);
    return "";
  }

  [[nodiscard]]std::pair<std::string, std::string> minmax_text() const {
    if (type_ == FieldType::kNull) return {"", ""};
    if (type_ == FieldType::kUnicode || type_ == FieldType::kUnknown) {
      if (!has_strings_) return {"", ""};
      return {lossy_utf8(min_string_), lossy_utf8(max_string_)};
    }
    if (type_ == FieldType::kInteger) {
      if (!has_int_minmax_) return {"", ""};
      return {format_i64(min_int_), format_i64(max_int_)};
    }
    if (type_ == FieldType::kFloat) {
      if (!has_float_minmax_) return {"", ""};
      return {format_f64(min_float_), format_f64(max_float_)};
    }
    return {"", ""};
  }

  [[nodiscard]]std::pair<std::string, std::string> length_text() const {
    if (!has_length_) return {"", ""};
    return {std::to_string(min_length_), std::to_string(max_length_)};
  }

  [[nodiscard]]std::string median_text() const {
    if (medians_.empty()) return "";
    std::vector<double> values = medians_;
    std::sort(values.begin(), values.end());
    const std::size_t size = values.size();
    if (size % 2 == 1) return format_f64(values[size / 2]);
    return format_f64((values[size / 2 - 1] + values[size / 2]) / 2.0);
  }

  [[nodiscard]]std::string mode_text() const {
    if (mode_values_.empty()) return "N/A";
    std::vector<std::string> values = mode_values_;
    std::sort(values.begin(), values.end());
    // Faithful port of stats::mode_on_sorted. The reference algorithm is not a
    // frequency-table mode: ties reset the candidate, and the count it compares
    // is a run counter. Reproducing it is required for byte-identical output.
    std::optional<std::size_t> mode;
    std::optional<std::size_t> next;
    std::size_t mode_count = 0;
    std::size_t next_count = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (mode.has_value() && values[*mode] == values[i]) {
        ++mode_count;
      } else if (next.has_value() && values[*next] == values[i]) {
        ++next_count;
      } else {
        next = i;
        next_count = 0;
      }
      if (next_count > mode_count) {
        mode = next;
        mode_count = next_count;
        next.reset();
        next_count = 0;
      } else if (next_count == mode_count) {
        mode.reset();
        mode_count = 0;
      }
    }
    if (!mode.has_value()) return "N/A";
    return lossy_utf8(values[*mode]);
  }

  FieldType type_ = FieldType::kNull;
  bool include_nulls_;
  bool want_mode_;
  bool want_cardinality_;
  bool want_median_;

  std::uint64_t sum_bits_ = 0;
  bool has_float_ = false;
  double sum_float_ = 0.0;

  bool has_length_ = false;
  std::size_t min_length_ = 0;
  std::size_t max_length_ = 0;
  bool has_strings_ = false;
  std::string min_string_;
  std::string max_string_;
  bool has_int_minmax_ = false;
  std::int64_t min_int_ = 0;
  std::int64_t max_int_ = 0;
  bool has_float_minmax_ = false;
  double min_float_ = 0.0;
  double max_float_ = 0.0;

  std::uint64_t count_ = 0;
  double mean_ = 0.0;
  double variance_ = 0.0;

  std::map<std::string, std::uint64_t> counts_;
  std::vector<std::string> mode_values_;
  std::vector<double> medians_;
};

}  // namespace

Status stats(std::span<const std::string_view> argv) {
  auto parsed = parse_args(argv);
  if (!parsed) return std::unexpected(parsed.error());
  Args& args = *parsed;
  if (args.help) {
    std::cout << kUsage;
    return success();
  }

  auto config = Config::from_path(args.input.has_value()
                                      ? std::optional<std::string_view>(*args.input)
                                      : std::nullopt);
  if (!config) return std::unexpected(config.error());
  config->no_headers = toggled_no_headers(args.no_headers);
  if (args.delimiter) config->delimiter = *args.delimiter;

  std::optional<Index> index;
  if (config->path) {
    const auto index_path = config->index_path();
    std::error_code ignored;
    if (std::filesystem::exists(index_path, ignored)) {
      auto data_time = mtime_seconds(*config->path);
      if (!data_time) return std::unexpected(data_time.error());
      auto index_time = mtime_seconds(index_path);
      if (!index_time) return std::unexpected(index_time.error());
      if (*data_time > *index_time) {
        return fail("The CSV file was modified after the index file. "
                    "Please re-create the index.");
      }
      auto opened = Index::open(index_path);
      if (!opened) return std::unexpected(opened.error());
      index = std::move(*opened);
    }
  }

  auto mapping = util::open_input(*config);
  if (!mapping) return std::unexpected(mapping.error());

  csv::ReaderOptions options = util::reader_options(*config, !config->no_headers);
  csv::Reader reader(mapping->bytes(), options);
  csv::Record headers;
  if (!config->no_headers) {
    if (Status status = reader.prime(); !status) {
      return std::unexpected(status.error());
    }
    headers = reader.headers();
  } else {
    auto first = util::peek_first(mapping->bytes(), options);
    if (!first) return std::unexpected(first.error());
    headers = std::move(*first);
  }

  auto selection = Selection::parse(args.select.value_or(""), headers.fields,
                                    !config->no_headers);
  if (!selection) return std::unexpected(selection.error());

  const bool want_mode = args.mode || args.everything;
  const bool want_cardinality = args.cardinality || args.everything;
  const bool want_median = args.median || args.everything;
  std::vector<Accumulator> accumulators;
  accumulators.reserve(selection->size());
  for (std::size_t i = 0; i < selection->size(); ++i) {
    accumulators.emplace_back(args.nulls, want_mode, want_cardinality,
                              want_median);
  }

  const unsigned workers =
      args.jobs == 0 ? parallel::configured_threads() : args.jobs;
  bool computed = false;
  if (index.has_value() && workers > 1) {
    std::uint64_t total = index->count();
    if (!config->no_headers && total > 0) --total;
    if (total > 0) {
      const std::size_t chunk_size =
          total < workers ? static_cast<std::size_t>(total)
                          : static_cast<std::size_t>(total / workers);
      const std::size_t chunks =
          (static_cast<std::size_t>(total) + chunk_size - 1) / chunk_size;
      if (args.jobs > 1) parallel::set_threads(args.jobs);
      std::vector<std::vector<Accumulator>> partial(chunks);
      for (auto& column_stats : partial) {
        column_stats.reserve(selection->size());
        for (std::size_t i = 0; i < selection->size(); ++i) {
          column_stats.emplace_back(args.nulls, want_mode, want_cardinality,
                                    want_median);
        }
      }
      std::atomic<bool> failed{false};
      std::mutex error_mutex;
      std::string error_message;
      auto record_error = [&](const Error& error) {
        if (!failed.exchange(true)) {
          const std::lock_guard<std::mutex> lock(error_mutex);
          error_message = error.message;
        }
      };
      parallel::for_each_index(chunks, 1, [&](std::size_t chunk) {
        if (failed.load(std::memory_order_relaxed)) return;
        const std::size_t start = chunk * chunk_size;
        const std::size_t take =
            std::min(chunk_size, static_cast<std::size_t>(total) - start);
        csv::Reader local(mapping->bytes(), options);
        if (!config->no_headers) {
          if (Status status = local.prime(); !status) {
            record_error(status.error());
            return;
          }
        }
        const std::uint64_t record_index =
            static_cast<std::uint64_t>(start) +
            (config->no_headers ? 0 : 1);
        auto offset = index->offset(record_index);
        if (!offset) {
          record_error(offset.error());
          return;
        }
        if (Status status = local.seek(*offset); !status) {
          record_error(status.error());
          return;
        }
        csv::Record row;
        std::uint64_t last_advised = *offset;
        for (std::size_t i = 0; i < take; ++i) {
          auto next = local.next(row);
          if (!next) {
            record_error(next.error());
            return;
          }
          if (!*next) return;
          for (std::size_t column = 0; column < selection->size(); ++column) {
            partial[chunk][column].add(
                row.fields[selection->indices()[column]]);
          }
          if (row.byte_offset > last_advised + 8 * 1024 * 1024) {
            mapping->advise_dontneed(last_advised, row.byte_offset);
            last_advised = row.byte_offset;
          }
        }
      });
      if (failed.load()) return fail(error_message);
      for (const auto& column_stats : partial) {
        for (std::size_t column = 0; column < selection->size(); ++column) {
          accumulators[column].merge(column_stats[column]);
        }
      }
      computed = true;
    }
  }

  if (!computed) {
    csv::Record record;
    util::SequentialAdvice advice(*mapping);
    while (true) {
      auto next = reader.next(record);
      if (!next) return std::unexpected(next.error());
      if (!*next) break;
      for (std::size_t i = 0; i < selection->size(); ++i) {
        accumulators[i].add(record.fields[selection->indices()[i]]);
      }
      advice.reach(record.byte_offset);
    }
  }

  std::ofstream file;
  std::ostream* stream = &std::cout;
  if (args.output.has_value() && *args.output != "-") {
    file.open(*args.output, std::ios::binary | std::ios::trunc);
    if (!file) return fail("failed to create " + *args.output);
    stream = &file;
  }
  csv::Writer writer(*stream,
                     util::writer_options(util::output_delimiter(args.output)));

  std::vector<std::string_view> header_row = {
      "field", "type", "sum", "min", "max", "min_length", "max_length",
      "mean", "stddev"};
  if (want_median) header_row.push_back("median");
  if (want_mode) header_row.push_back("mode");
  if (want_cardinality) header_row.push_back("cardinality");
  if (Status status = writer.write_record(header_row); !status) {
    return std::unexpected(status.error());
  }

  for (std::size_t i = 0; i < selection->size(); ++i) {
    std::string field_name;
    if (config->no_headers) {
      field_name = std::to_string(i);
    } else {
      field_name = std::string(headers.fields[selection->indices()[i]]);
    }
    std::vector<std::string> cells = accumulators[i].record();
    std::vector<std::string_view> row;
    row.reserve(cells.size() + 1);
    row.push_back(field_name);
    for (const std::string& cell : cells) row.push_back(cell);
    if (Status status = writer.write_record(row); !status) {
      return std::unexpected(status.error());
    }
  }
  return writer.flush();
}

}  // namespace xsv::commands
