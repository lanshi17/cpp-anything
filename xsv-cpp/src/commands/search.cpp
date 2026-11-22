#include "command_util.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/commands.hpp"
#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/regex.hpp"
#include "xsv/select.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Filters CSV data by whether the given regex matches a row.

The regex is applied to each field in each row, and if any field matches,
then the row is written to the output. The columns to search can be limited
with the '--select' flag (but the full row is still written to the output if
there is a match).

Usage:
    xsv search [options] <regex> [<input>]
    xsv search --help

search options:
    -i, --ignore-case      Case insensitive search. This is equivalent to
                           prefixing the regex with '(?i)'.
    -s, --select <arg>     Select the columns to search. See 'xsv select -h'
                           for the full syntax.
    -v, --invert-match     Select only rows that did not match

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
  std::optional<std::string> pattern;
  std::optional<std::string> input;
  std::optional<std::string> output;
  std::optional<std::string> select;
  bool no_headers = false;
  bool ignore_case = false;
  bool invert = false;
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
    if (!positional_only && (arg == "-i" || arg == "--ignore-case")) {
      args.ignore_case = true;
      continue;
    }
    if (!positional_only && (arg == "-v" || arg == "--invert-match")) {
      args.invert = true;
      continue;
    }
    if (!positional_only && (arg == "-n" || arg == "--no-headers")) {
      args.no_headers = true;
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
    if (!args.pattern.has_value()) {
      args.pattern = std::string(arg);
    } else if (!args.input.has_value()) {
      args.input = std::string(arg);
    } else {
      return fail("xsv search takes at most one input path.");
    }
  }
  return args;
}

}  // namespace

Status search(std::span<const std::string_view> argv) {
  auto parsed = parse_args(argv);
  if (!parsed) return std::unexpected(parsed.error());
  Args& args = *parsed;
  if (args.help) {
    std::cout << kUsage;
    return success();
  }
  if (!args.pattern.has_value()) return fail("Missing <regex> argument.");

  auto config = Config::from_path(args.input.has_value()
                                      ? std::optional<std::string_view>(*args.input)
                                      : std::nullopt);
  if (!config) return std::unexpected(config.error());
  config->no_headers = toggled_no_headers(args.no_headers);
  if (args.delimiter) config->delimiter = *args.delimiter;

  auto regex = Regex::compile(*args.pattern, args.ignore_case);
  if (!regex) return std::unexpected(regex.error());

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

  std::ofstream file;
  std::ostream* stream = &std::cout;
  if (args.output.has_value() && *args.output != "-") {
    file.open(*args.output, std::ios::binary | std::ios::trunc);
    if (!file) return fail("failed to create " + *args.output);
    stream = &file;
  }
  csv::Writer writer(*stream,
                     util::writer_options(util::output_delimiter(args.output)));

  if (!config->no_headers && !headers.empty()) {
    if (Status status = writer.write_record(headers.fields); !status) {
      return std::unexpected(status.error());
    }
  }

  csv::Record record;
  util::SequentialAdvice advice(*mapping);
  while (true) {
    auto next = reader.next(record);
    if (!next) return std::unexpected(next.error());
    if (!*next) break;
    bool matched = false;
    for (const std::size_t index : selection->indices()) {
      if (regex->is_match(record.fields[index])) {
        matched = true;
        break;
      }
    }
    if (args.invert) matched = !matched;
    if (!matched) continue;
    if (Status status = writer.write_record(record.fields); !status) {
      return std::unexpected(status.error());
    }
    advice.reach(record.byte_offset);
  }
  return writer.flush();
}

}  // namespace xsv::commands
