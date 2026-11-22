#include "command_util.hpp"

#include "xsv/commands.hpp"

#include <cstdint>
#include <filesystem>
#include <system_error>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/index.hpp"
#include "xsv/mmap.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Prints a count of the number of records in the CSV data.

Note that the count will not include the header row (unless --no-headers is
given).

Usage:
    xsv count [options] [<input>]

Common options:
    -h, --help             Display this message
    -n, --no-headers       When set, the first row will not be included in
                           the count.
    -d, --delimiter <arg>  The field delimiter for reading CSV data.
                           Must be a single character. (default: ,)
)";

struct Args {
  std::optional<std::string> input;
  bool no_headers = false;
  std::optional<char> delimiter;
  bool help = false;
};

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
    if (!positional_only && arg.starts_with("-d") && arg.size() > 2) {
      auto delimiter = parse_delimiter(arg.substr(2));
      if (!delimiter) return std::unexpected(delimiter.error());
      args.delimiter = *delimiter;
      continue;
    }
    if (!positional_only && arg.starts_with("-") && arg != "-") {
      return fail("Unknown flag: " + std::string(arg));
    }
    if (args.input.has_value()) {
      return fail("xsv count takes at most one input path.");
    }
    args.input = std::string(arg);
  }
  return args;
}

}  // namespace

Status count(std::span<const std::string_view> argv) {
  auto parsed = parse_args(argv);
  if (!parsed) return std::unexpected(parsed.error());
  Args& args = *parsed;
  if (args.help) {
    std::cout << kUsage;
    return success();
  }

  auto config = Config::from_path(args.input);
  if (!config) return std::unexpected(config.error());
  config->no_headers = toggled_no_headers(args.no_headers);
  if (args.delimiter) config->delimiter = *args.delimiter;

  std::uint64_t count = 0;

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
      auto index = Index::open(index_path);
      if (!index) return std::unexpected(index.error());
      count = index->count();
      if (!config->no_headers && count > 0) --count;
      std::cout << count << '\n';
      if (!std::cout) return fail("failed to write output");
      return success();
    }
  }

  auto mapping = config->path
                     ? Mapping::open_file(*config->path)
                     : Mapping::read_stream(std::cin);
  if (!mapping) return std::unexpected(mapping.error());

  csv::ReaderOptions options;
  options.delimiter = config->delimiter;
  options.quote = config->quote;
  options.double_quote = config->double_quote;
  options.escape = config->escape;
  options.flexible = config->flexible;
  options.has_headers = !config->no_headers;
  options.quoting = config->quoting;
  csv::Reader reader(mapping->bytes(), options);
  util::SequentialAdvice advice(*mapping);
  if (Status status = reader.prime(); !status) {
    return std::unexpected(status.error());
  }
  csv::Record record;
  while (true) {
    auto next = reader.next(record);
    if (!next) return std::unexpected(next.error());
    if (!*next) break;
    ++count;
    advice.reach(record.byte_offset);
  }

  std::cout << count << '\n';
  if (!std::cout) return fail("failed to write output");
  return success();
}

}  // namespace xsv::commands
