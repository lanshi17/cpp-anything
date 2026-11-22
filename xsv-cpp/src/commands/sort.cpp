#include "command_util.hpp"
#include "store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/commands.hpp"
#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/format.hpp"
#include "xsv/parallel.hpp"
#include "xsv/select.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Sorts CSV data lexicographically.

Note that this requires reading all of the CSV data into memory.

Usage:
    xsv sort [options] [<input>]

sort options:
    -s, --select <arg>     Select a subset of columns to sort.
                           See 'xsv select --help' for the format details.
    -N, --numeric          Compare according to string numerical value
    -R, --reverse          Reverse order

Common options:
    -h, --help             Display this message
    -o, --output <file>    Write output to <file> instead of stdout.
    -n, --no-headers       When set, the first row will not be interpreted
                           as headers. Namely, it will be sorted with the rest
                           of the rows. Otherwise, the first row will always
                           appear as the header row in the output.
    -d, --delimiter <arg>  The field delimiter for reading CSV data.
                           Must be a single character. (default: ,)
)";

struct Args {
  std::optional<std::string> input;
  std::optional<std::string> output;
  std::optional<std::string> select;
  bool numeric = false;
  bool reverse = false;
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
    if (!positional_only && (arg == "-N" || arg == "--numeric")) {
      args.numeric = true;
      continue;
    }
    if (!positional_only && (arg == "-R" || arg == "--reverse")) {
      args.reverse = true;
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
    if (args.input.has_value()) return fail("xsv sort takes at most one input path.");
    args.input = std::string(arg);
  }
  return args;
}

struct Number {
  bool valid = false;
  bool integer = false;
  std::int64_t i = 0;
  double f = 0.0;
};

Number parse_number(std::string_view field) {
  auto integer = parse_i64(field);
  if (integer) return Number{true, true, *integer, 0.0};
  auto floating = parse_f64(field);
  if (floating) return Number{true, false, 0, *floating};
  return Number{};
}

int compare_numbers(const Number& a, const Number& b) {
  if (!a.valid && !b.valid) return 0;
  if (!a.valid) return -1;
  if (!b.valid) return 1;
  if (a.integer && b.integer) {
    if (a.i < b.i) return -1;
    if (a.i > b.i) return 1;
    return 0;
  }
  const double av = a.integer ? static_cast<double>(a.i) : a.f;
  const double bv = b.integer ? static_cast<double>(b.i) : b.f;
  if (av < bv) return -1;
  if (av > bv) return 1;
  return 0;
}

int compare_records(const util::RecordStore& store, std::size_t a,
                    std::size_t b, const Selection& selection, bool numeric) {
  for (const std::size_t index : selection.indices()) {
    const std::string_view fa = store.field(a, index);
    const std::string_view fb = store.field(b, index);
    int comparison = 0;
    if (numeric) {
      const Number na = parse_number(fa);
      const Number nb = parse_number(fb);
      if (!na.valid || !nb.valid) {
        // The reference stops at the first pair where either side is not a
        // number; two unparseable fields compare equal without looking at the
        // remaining selected columns.
        if (!na.valid && !nb.valid) return 0;
        return na.valid ? 1 : -1;
      }
      comparison = compare_numbers(na, nb);
    } else if (fa < fb) {
      comparison = -1;
    } else if (fa > fb) {
      comparison = 1;
    }
    if (comparison != 0) return comparison;
  }
  return 0;
}

template <class Compare>
void stable_sort_order(std::vector<std::size_t>& order, Compare compare) {
#if XSV_USE_TBB
  const unsigned threads = parallel::configured_threads();
  const std::size_t count = order.size();
  if (threads > 1 && count > 64) {
    std::size_t chunks = std::min<std::size_t>(threads * 4, count);
    const std::size_t chunk_size = (count + chunks - 1) / chunks;
    chunks = (count + chunk_size - 1) / chunk_size;
    parallel::for_each_index(chunks, 1, [&](std::size_t chunk) {
      const std::size_t begin = chunk * chunk_size;
      const std::size_t end = std::min(begin + chunk_size, count);
      std::stable_sort(order.begin() + static_cast<std::ptrdiff_t>(begin),
                       order.begin() + static_cast<std::ptrdiff_t>(end),
                       compare);
    });

    std::vector<std::size_t> merged(count);
    std::size_t width = chunk_size;
    while (width < count) {
      const std::size_t pairs = (count + 2 * width - 1) / (2 * width);
      parallel::for_each_index(pairs, 1, [&](std::size_t pair) {
        const std::size_t begin = pair * 2 * width;
        if (begin >= count) return;
        const std::size_t middle = std::min(begin + width, count);
        const std::size_t end = std::min(begin + 2 * width, count);
        if (middle == end) {
          std::copy(order.begin() + static_cast<std::ptrdiff_t>(begin),
                    order.begin() + static_cast<std::ptrdiff_t>(end),
                    merged.begin() + static_cast<std::ptrdiff_t>(begin));
        } else {
          std::merge(order.begin() + static_cast<std::ptrdiff_t>(begin),
                     order.begin() + static_cast<std::ptrdiff_t>(middle),
                     order.begin() + static_cast<std::ptrdiff_t>(middle),
                     order.begin() + static_cast<std::ptrdiff_t>(end),
                     merged.begin() + static_cast<std::ptrdiff_t>(begin),
                     compare);
        }
      });
      order.swap(merged);
      width *= 2;
    }
    return;
  }
#endif
  std::stable_sort(order.begin(), order.end(), compare);
}

}  // namespace

Status sort(std::span<const std::string_view> argv) {
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

  util::RecordStore store;
  store.reserve(mapping->bytes().size(), mapping->bytes().size() / 12,
                mapping->bytes().size() / 256);
  std::vector<std::size_t> order;
  csv::Record record;
  while (true) {
    auto next = reader.next(record);
    if (!next) return std::unexpected(next.error());
    if (!*next) break;
    store.add(record);
    order.push_back(store.size() - 1);
  }

  auto compare = [&](std::size_t a, std::size_t b) {
    const int value = args.reverse
                          ? compare_records(store, b, a, *selection, args.numeric)
                          : compare_records(store, a, b, *selection, args.numeric);
    return value < 0;
  };
  stable_sort_order(order, compare);

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
  std::vector<std::string_view> scratch;
  for (const std::size_t index : order) {
    scratch.clear();
    const std::size_t count = store.field_count(index);
    scratch.reserve(count);
    for (std::size_t field = 0; field < count; ++field) {
      scratch.push_back(store.field(index, field));
    }
    if (Status status = writer.write_record(scratch); !status) {
      return std::unexpected(status.error());
    }
  }
  return writer.flush();
}

}  // namespace xsv::commands
