#include "command_util.hpp"

#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/commands.hpp"
#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/select.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Select columns from CSV data efficiently.

This command lets you manipulate the columns in CSV data. You can re-order
them, duplicate them or drop them. Columns can be referenced by index or by
name if there is a header row (duplicate column names can be disambiguated with
more indexing). Finally, column ranges can be specified.

  Select the first and fourth columns:
  $ xsv select 1,4

  Select the first 4 columns (by index and by name):
  $ xsv select 1-4
  $ xsv select Header1-Header4

  Ignore the first 2 columns (by range and by omission):
  $ xsv select 3-
  $ xsv select '!1-2'

  Select the third column named 'Foo':
  $ xsv select 'Foo[2]'

  Re-order and duplicate columns arbitrarily:
  $ xsv select 3-1,Header3-Header1,Header1,Foo[2],Header1

  Quote column names that conflict with selector syntax:
  $ xsv select '"Date - Opening","Date - Actual Closing"'

Usage:
    xsv select [options] [--] <selection> [<input>]
    xsv select --help

Common options:
    -h, --help             Display this message
    -o, --output <file>    Write output to <file> instead of stdout.
    -n, --no-headers       When set, the first row will not be interpreted
                           as headers. (i.e., They are not searched, analyzed,
                           sliced, etc.)
    -d, --delimiter <arg>  The field delimiter for reading CSV data.
                           Must be a single character. (default: ,)
)";

struct Args {
  std::optional<std::string> selection;
  std::optional<std::string> input;
  std::optional<std::string> output;
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
    if (!args.selection.has_value()) {
      args.selection = std::string(arg);
    } else if (!args.input.has_value()) {
      args.input = std::string(arg);
    } else {
      return fail("xsv select takes at most one input path.");
    }
  }
  return args;
}

}  // namespace

Status select(std::span<const std::string_view> argv) {
  auto parsed = parse_args(argv);
  if (!parsed) return std::unexpected(parsed.error());
  Args& args = *parsed;
  if (args.help) {
    std::cout << kUsage;
    return success();
  }
  if (!args.selection.has_value()) {
    return fail("Missing <selection> argument.");
  }

  auto config = Config::from_path(args.input.has_value()
                                      ? std::optional<std::string_view>(*args.input)
                                      : std::nullopt);
  if (!config) return std::unexpected(config.error());
  config->no_headers = toggled_no_headers(args.no_headers);
  if (args.delimiter) config->delimiter = *args.delimiter;

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

  auto selection = Selection::parse(*args.selection, headers.fields,
                                    !config->no_headers);
  if (!selection) return std::unexpected(selection.error());

  std::ofstream file;
  std::ostream* stream = &std::cout;
  if (args.output.has_value() && *args.output != "-") {
    file.open(*args.output, std::ios::binary | std::ios::trunc);
    if (!file) return fail("failed to create " + *args.output);
    stream = &file;
  }
  csv::Writer writer(*stream,
                     util::writer_options(util::output_delimiter(args.output)));

  if (!config->no_headers) {
    std::vector<std::string_view> projected = selection->apply(headers);
    if (Status status = writer.write_record(projected); !status) {
      return std::unexpected(status.error());
    }
  }

  csv::Record record;
  util::SequentialAdvice advice(*mapping);
  std::vector<std::string_view> projected;
  while (true) {
    auto next = reader.next(record);
    if (!next) return std::unexpected(next.error());
    if (!*next) break;
    projected = selection->apply(record);
    if (Status status = writer.write_record(projected); !status) {
      return std::unexpected(status.error());
    }
    advice.reach(record.byte_offset);
  }
  return writer.flush();
}

}  // namespace xsv::commands
