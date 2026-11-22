#include "command_util.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "xsv/commands.hpp"
#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/index.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Returns the rows in the range specified (starting at 0, half-open interval).
The range does not include headers.

If the start of the range isn't specified, then the slice starts from the first
record in the CSV data.

If the end of the range isn't specified, then the slice continues to the last
record in the CSV data.

This operation can be made much faster by creating an index with 'xsv index'
first. Namely, a slice on an index requires parsing just the rows that are
sliced. Without an index, all rows up to the first row in the slice must be
parsed.

Usage:
    xsv slice [options] [<input>]

slice options:
    -s, --start <arg>      The index of the record to slice from.
    -e, --end <arg>        The index of the record to slice to.
    -l, --len <arg>        The length of the slice (can be used instead
                           of --end).
    -i, --index <arg>      Slice a single record (shortcut for -s N -l 1).

Common options:
    -h, --help             Display this message
    -o, --output <file>    Write output to <file> instead of stdout.
    -n, --no-headers       When set, the first row will not be interpreted
                           as headers. Otherwise, the first row will always
                           appear in the output as the header row.
    -d, --delimiter <arg>  The field delimiter for reading CSV data.
                           Must be a single character. (default: ,)
)";

struct Args {
  std::optional<std::string> input;
  std::optional<std::string> output;
  std::optional<std::size_t> start;
  std::optional<std::size_t> end;
  std::optional<std::size_t> len;
  std::optional<std::size_t> index;
  bool no_headers = false;
  std::optional<char> delimiter;
  bool help = false;
};

Result<std::size_t> parse_size(std::string_view text) {
  if (text.empty()) return fail("invalid digit found in string");
  std::size_t value = 0;
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
  auto value_for = [&](std::size_t& i, std::string_view flag)
      -> Result<std::string_view> {
    if (i + 1 >= argv.size()) {
      return fail("The argument '" + std::string(flag) +
                  "' requires a value but none was supplied.");
    }
    return argv[++i];
  };

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
    if (!positional_only && (arg == "-s" || arg == "--start" ||
                             arg == "-e" || arg == "--end" ||
                             arg == "-l" || arg == "--len" ||
                             arg == "-i" || arg == "--index")) {
      auto value = value_for(i, arg);
      if (!value) return std::unexpected(value.error());
      auto parsed = parse_size(*value);
      if (!parsed) return std::unexpected(parsed.error());
      if (arg == "-s" || arg == "--start") args.start = *parsed;
      else if (arg == "-e" || arg == "--end") args.end = *parsed;
      else if (arg == "-l" || arg == "--len") args.len = *parsed;
      else args.index = *parsed;
      continue;
    }
    if (!positional_only && (arg == "-o" || arg == "--output")) {
      auto value = value_for(i, arg);
      if (!value) return std::unexpected(value.error());
      args.output = std::string(*value);
      continue;
    }
    if (!positional_only && arg.starts_with("--output=")) {
      args.output = std::string(arg.substr(9));
      continue;
    }
    if (!positional_only && (arg == "-d" || arg == "--delimiter")) {
      auto value = value_for(i, arg);
      if (!value) return std::unexpected(value.error());
      auto delimiter = parse_delimiter(*value);
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
    if (args.input.has_value()) return fail("xsv slice takes at most one input path.");
    args.input = std::string(arg);
  }
  return args;
}

Result<std::pair<std::size_t, std::size_t>> range_of(const Args& args) {
  if (args.index.has_value() &&
      (args.start.has_value() || args.end.has_value() || args.len.has_value())) {
    return fail("--index cannot be used with --start, --end or --len");
  }
  if (args.index.has_value()) {
    return std::pair<std::size_t, std::size_t>{*args.index, *args.index + 1};
  }
  if (args.end.has_value() && args.len.has_value()) {
    return fail("--end and --len cannot be used at the same time.");
  }
  const std::size_t start = args.start.value_or(0);
  if (args.end.has_value()) {
    const std::size_t end = *args.end;
    if (start > end) {
      return fail("The end of the range (" + std::to_string(end) +
                  ") must be greater than or\nequal to the start of the range (" +
                  std::to_string(start) + ").");
    }
    return std::pair<std::size_t, std::size_t>{start, end};
  }
  if (args.len.has_value()) {
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    const std::size_t end = *args.len > max - start ? max : start + *args.len;
    return std::pair<std::size_t, std::size_t>{start, end};
  }
  return std::pair<std::size_t, std::size_t>{
      start, std::numeric_limits<std::size_t>::max()};
}

}  // namespace

Status slice(std::span<const std::string_view> argv) {
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

  auto mapping = util::open_input(*config);
  if (!mapping) return std::unexpected(mapping.error());

  csv::ReaderOptions options =
      util::reader_options(*config, !config->no_headers);
  csv::Reader reader(mapping->bytes(), options);
  if (!config->no_headers) {
    if (Status status = reader.prime(); !status) {
      return std::unexpected(status.error());
    }
  }

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

  std::ofstream file;
  std::ostream* stream = &std::cout;
  if (args.output.has_value() && *args.output != "-") {
    file.open(*args.output, std::ios::binary | std::ios::trunc);
    if (!file) return fail("failed to create " + *args.output);
    stream = &file;
  }
  csv::Writer writer(*stream,
                     util::writer_options(util::output_delimiter(args.output)));

  if (!config->no_headers && !reader.headers().empty()) {
    if (Status status = writer.write_record(reader.headers().fields); !status) {
      return std::unexpected(status.error());
    }
  }

  auto range = range_of(args);
  if (!range) return std::unexpected(range.error());
  const std::size_t start = range->first;
  const std::size_t end = range->second;
  if (end - start == 0) return writer.flush();

  bool used_index = false;
  if (index.has_value()) {
    const std::uint64_t total = index->count();
    const std::uint64_t data_count =
        (!config->no_headers && total > 0) ? total - 1 : total;
    if (start >= data_count) {
      return fail("invalid record index " + std::to_string(start) +
                  " (there are " + std::to_string(data_count) + " records)");
    }
    const std::uint64_t record = start + (config->no_headers ? 0 : 1);
    auto offset = index->offset(record);
    if (!offset) return std::unexpected(offset.error());
    if (Status status = reader.seek(*offset); !status) {
      return std::unexpected(status.error());
    }
    used_index = true;
  }

  csv::Record record;
  util::SequentialAdvice advice(*mapping);
  if (!used_index) {
    for (std::size_t skipped = 0; skipped < start; ++skipped) {
      auto next = reader.next(record);
      if (!next) return std::unexpected(next.error());
      if (!*next) return writer.flush();
      advice.reach(record.byte_offset);
    }
  }

  std::size_t written = 0;
  while (written < end - start) {
    auto next = reader.next(record);
    if (!next) return std::unexpected(next.error());
    if (!*next) break;
    if (Status status = writer.write_record(record.fields); !status) {
      return std::unexpected(status.error());
    }
    advice.reach(record.byte_offset);
    ++written;
  }
  return writer.flush();
}

}  // namespace xsv::commands
