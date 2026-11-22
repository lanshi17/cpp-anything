#include "xsv/commands.hpp"

#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/index.hpp"
#include "xsv/mmap.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Creates an index of the given CSV data, which can make other operations like
slicing, splitting and gathering statistics much faster.

Note that this does not accept CSV data on stdin. You must give a file
path. The index is created at 'path/to/input.csv.idx'. The index will be
automatically used by commands that can benefit from it. If the original CSV
data changes after the index is made, commands that try to use it will result
in an error (you have to regenerate the index before it can be used again).

Usage:
    xsv index [options] <input>
    xsv index --help

index options:
    -o, --output <file>    Write index to <file> instead of <input>.idx.
                           Generally, this is not currently useful because
                           the only way to use an index is if it is specially
                           named <input>.idx.

Common options:
    -h, --help             Display this message
    -d, --delimiter <arg>  The field delimiter for reading CSV data.
                           Must be a single character. (default: ,)
)";

struct Args {
  std::optional<std::string> input;
  std::optional<std::string> output;
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
    if (!positional_only && arg.starts_with("-") && arg != "-") {
      return fail("Unknown flag: " + std::string(arg));
    }
    if (args.input.has_value()) {
      return fail("xsv index takes exactly one input path.");
    }
    args.input = std::string(arg);
  }
  return args;
}

}  // namespace

Status index(std::span<const std::string_view> argv) {
  auto parsed = parse_args(argv);
  if (!parsed) return std::unexpected(parsed.error());
  Args& args = *parsed;
  if (args.help) {
    std::cout << kUsage;
    return success();
  }
  if (!args.input.has_value() || *args.input == "-") {
    return fail("Cannot use <stdin> here");
  }

  auto config = Config::from_path(args.input);
  if (!config) return std::unexpected(config.error());
  if (args.delimiter) config->delimiter = *args.delimiter;

  const std::filesystem::path output =
      args.output ? std::filesystem::path(*args.output) : config->index_path();

  auto mapping = Mapping::open_file(*config->path);
  if (!mapping) return std::unexpected(mapping.error());

  csv::ReaderOptions options;
  options.delimiter = config->delimiter;
  options.quote = config->quote;
  options.double_quote = config->double_quote;
  options.escape = config->escape;
  options.flexible = config->flexible;
  options.has_headers = true;
  options.quoting = config->quoting;
  csv::Reader reader(mapping->bytes(), options);

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    return fail("failed to create " + output.string());
  }
  Status status = create_index(reader, out, &*mapping);
  if (!status) return status;
  return success();
}

}  // namespace xsv::commands
