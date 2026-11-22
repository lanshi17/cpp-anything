#include "command_util.hpp"
#include "store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsv/commands.hpp"
#include "xsv/config.hpp"
#include "xsv/csv.hpp"
#include "xsv/hash.hpp"
#include "xsv/select.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Joins two sets of CSV data on the specified columns.

The default join operation is an 'inner' join. This corresponds to the
intersection of rows on the keys specified.

Joins are always done by ignoring leading and trailing whitespace. By default,
joins are done case sensitively, but this can be disabled with the --no-case
flag.

The columns arguments specify the columns to join for each input. Columns can
be referenced by name or index, starting at 1. Specify multiple columns by
separating them with a comma. Specify a range of columns with `-`. Both
columns1 and columns2 must specify exactly the same number of columns.
(See 'xsv select --help' for the full syntax.)

Usage:
    xsv join [options] <columns1> <input1> <columns2> <input2>
    xsv join --help

join options:
    --no-case              When set, joins are done case insensitively.
    --left                 Do a 'left outer' join. This returns all rows in
                           first CSV data set, including rows with no
                           corresponding row in the second data set. When no
                           corresponding row exists, it is padded out with
                           empty fields.
    --right                Do a 'right outer' join. This returns all rows in
                           second CSV data set, including rows with no
                           corresponding row in the first data set. When no
                           corresponding row exists, it is padded out with
                           empty fields. (This is the reverse of 'outer left'.)
    --full                 Do a 'full outer' join. This returns all rows in
                           both data sets with matching records joined. If
                           there is no match, the missing side will be padded
                           out with empty fields. (This is the combination of
                           'outer left' and 'outer right'.)
    --cross                USE WITH CAUTION.
                           This returns the cartesian product of the CSV
                           data sets given. The number of rows return is
                           equal to N * M, where N and M correspond to the
                           number of rows in the given data sets, respectively.
    --nulls                When set, joins will work on empty fields.
                           Otherwise, empty fields are completely ignored.
                           (In fact, any row that has an empty field in the
                           key specified is ignored.)

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
  std::optional<std::string> columns1;
  std::optional<std::string> input1;
  std::optional<std::string> columns2;
  std::optional<std::string> input2;
  std::optional<std::string> output;
  bool no_headers = false;
  bool no_case = false;
  bool nulls = false;
  bool left = false;
  bool right = false;
  bool full = false;
  bool cross = false;
  std::optional<char> delimiter;
  bool help = false;
};

Result<Args> parse_args(std::span<const std::string_view> argv) {
  Args args;
  std::vector<std::string> positional;
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
    if (!positional_only && arg == "--no-case") {
      args.no_case = true;
      continue;
    }
    if (!positional_only && arg == "--nulls") {
      args.nulls = true;
      continue;
    }
    if (!positional_only && arg == "--left") {
      args.left = true;
      continue;
    }
    if (!positional_only && arg == "--right") {
      args.right = true;
      continue;
    }
    if (!positional_only && arg == "--full") {
      args.full = true;
      continue;
    }
    if (!positional_only && arg == "--cross") {
      args.cross = true;
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
    positional.emplace_back(arg);
  }
  if (positional.size() != 4 && !args.help) return fail("Invalid arguments.");
  if (positional.size() >= 1) args.columns1 = positional[0];
  if (positional.size() >= 2) args.input1 = positional[1];
  if (positional.size() >= 3) args.columns2 = positional[2];
  if (positional.size() >= 4) args.input2 = positional[3];
  return args;
}

bool is_whitespace_codepoint(std::uint32_t cp) {
  return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 ||
         cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
         cp == 0x3000;
}

bool decode_utf8(std::string_view text, std::vector<std::uint32_t>& out) {
  out.clear();
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::uint32_t cp = 0;
    std::size_t extra = 0;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= text.size()) return false;
    for (std::size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    out.push_back(cp);
    i += extra + 1;
  }
  return true;
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::uint32_t lower_first(std::uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 32;
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 32;
  if (cp == 0x130) return 'i';
  if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 32;
  if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
  if (cp >= 0x400 && cp <= 0x40F) return cp + 80;
  return cp;
}

std::string transform_field(std::string_view field, bool case_insensitive) {
  std::vector<std::uint32_t> codepoints;
  if (!decode_utf8(field, codepoints)) return std::string(field);
  std::size_t begin = 0;
  std::size_t end = codepoints.size();
  while (begin < end && is_whitespace_codepoint(codepoints[begin])) ++begin;
  while (end > begin && is_whitespace_codepoint(codepoints[end - 1])) --end;
  std::string out;
  out.reserve(field.size());
  for (std::size_t i = begin; i < end; ++i) {
    const std::uint32_t cp =
        case_insensitive ? lower_first(codepoints[i]) : codepoints[i];
    append_utf8(out, cp);
  }
  return out;
}

std::string make_key(std::span<const std::string_view> fields,
                     const Selection& selection, bool case_insensitive,
                     bool nulls, bool& usable) {
  std::string key;
  usable = true;
  for (const std::size_t index : selection.indices()) {
    if (index >= fields.size()) {
      usable = false;
      return key;
    }
    const std::string field = transform_field(fields[index], case_insensitive);
    if (!nulls && field.empty()) usable = false;
    const std::uint32_t size = static_cast<std::uint32_t>(field.size());
    key.append(reinterpret_cast<const char*>(&size), sizeof(size));
    key.append(field);
  }
  return key;
}

std::string make_key(const util::RecordStore& store, std::size_t row,
                     const Selection& selection, bool case_insensitive,
                     bool nulls, bool& usable) {
  std::string key;
  usable = true;
  for (const std::size_t index : selection.indices()) {
    const std::string field =
        transform_field(store.field(row, index), case_insensitive);
    if (!nulls && field.empty()) usable = false;
    const std::uint32_t size = static_cast<std::uint32_t>(field.size());
    key.append(reinterpret_cast<const char*>(&size), sizeof(size));
    key.append(field);
  }
  return key;
}

}  // namespace

Status join(std::span<const std::string_view> argv) {
  auto parsed = parse_args(argv);
  if (!parsed) return std::unexpected(parsed.error());
  Args& args = *parsed;
  if (args.help) {
    std::cout << kUsage;
    return success();
  }
  if (!args.columns1 || !args.input1 || !args.columns2 || !args.input2) {
    return fail("Invalid arguments.");
  }
  const int operations = static_cast<int>(args.left) + static_cast<int>(args.right) +
                         static_cast<int>(args.full) + static_cast<int>(args.cross);
  if (operations > 1) return fail("Please pick exactly one join operation.");

  auto config1 = Config::from_path(std::string_view(*args.input1));
  auto config2 = Config::from_path(std::string_view(*args.input2));
  if (!config1) return std::unexpected(config1.error());
  if (!config2) return std::unexpected(config2.error());
  if (args.delimiter) {
    config1->delimiter = *args.delimiter;
    config2->delimiter = *args.delimiter;
  }
  config1->no_headers = toggled_no_headers(args.no_headers);
  config2->no_headers = config1->no_headers;
  if (!config1->path || !config2->path) {
    return fail("Cannot use <stdin> here");
  }

  auto mapping1 = Mapping::open_file(*config1->path);
  if (!mapping1) return std::unexpected(mapping1.error());
  auto mapping2 = Mapping::open_file(*config2->path);
  if (!mapping2) return std::unexpected(mapping2.error());

  csv::ReaderOptions options1 =
      util::reader_options(*config1, !config1->no_headers);
  csv::Reader reader1(mapping1->bytes(), options1);
  csv::Record headers1;
  if (!config1->no_headers) {
    if (Status status = reader1.prime(); !status) {
      return std::unexpected(status.error());
    }
    headers1 = reader1.headers();
  } else {
    auto first = util::peek_first(mapping1->bytes(), options1);
    if (!first) return std::unexpected(first.error());
    headers1 = std::move(*first);
  }

  csv::ReaderOptions options2 =
      util::reader_options(*config2, !config2->no_headers);
  csv::Reader reader2(mapping2->bytes(), options2);
  csv::Record headers2;
  if (!config2->no_headers) {
    if (Status status = reader2.prime(); !status) {
      return std::unexpected(status.error());
    }
    headers2 = reader2.headers();
  } else {
    auto first = util::peek_first(mapping2->bytes(), options2);
    if (!first) return std::unexpected(first.error());
    headers2 = std::move(*first);
  }

  auto sel1 = Selection::parse(*args.columns1, headers1.fields,
                               !config1->no_headers);
  auto sel2 = Selection::parse(*args.columns2, headers2.fields,
                               !config2->no_headers);
  if (!sel1) return std::unexpected(sel1.error());
  if (!sel2) return std::unexpected(sel2.error());
  if (sel1->size() != sel2->size()) {
    return fail("Column selections must have the same number of columns, "
                "but found column selections with " +
                std::to_string(sel1->size()) + " and " +
                std::to_string(sel2->size()) + " columns.");
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

  if (!config1->no_headers) {
    std::vector<std::string_view> header;
    header.reserve(headers1.size() + headers2.size());
    for (const auto field : headers1.fields) header.push_back(field);
    for (const auto field : headers2.fields) header.push_back(field);
    if (Status status = writer.write_record(header); !status) {
      return std::unexpected(status.error());
    }
  }

  const std::size_t width1 = headers1.size();
  const std::size_t width2 = headers2.size();
  const std::string_view empty;

  // Build side. For right joins the left input is the build side.
  const bool build_left = args.right;
  util::RecordStore build_store;
  util::RecordStore probe_store;
  std::deque<std::string> keys;
  hash::MultiMap<std::size_t> index;
  std::vector<bool> written;

  if (build_left) {
    build_store.reserve(mapping1->bytes().size(),
                        mapping1->bytes().size() / 12,
                        mapping1->bytes().size() / 128);
    probe_store.reserve(mapping2->bytes().size(),
                        mapping2->bytes().size() / 12,
                        mapping2->bytes().size() / 128);
  } else {
    probe_store.reserve(mapping2->bytes().size(),
                        mapping2->bytes().size() / 12,
                        mapping2->bytes().size() / 128);
  }

  auto build_from_reader = [&](csv::Reader& reader, const Selection& selection,
                               util::RecordStore& store) -> Status {
    csv::Record record;
    while (true) {
      auto next = reader.next(record);
      if (!next) return std::unexpected(next.error());
      if (!*next) break;
      store.add(record);
    }
    return success();
  };

  auto build_from_store = [&](const util::RecordStore& store,
                              const Selection& selection) {
    for (std::size_t row = 0; row < store.size(); ++row) {
      bool usable = false;
      std::string key = make_key(store, row, selection, args.no_case,
                                 args.nulls, usable);
      if (usable) {
        keys.push_back(std::move(key));
        index.insert(keys.back(), row);
      }
    }
  };

  if (build_left) {
    // Right join: store both sides; first row of each side remains data with
    // --no-headers, matching the reference.
    if (Status status = build_from_reader(reader1, *sel1, build_store);
        !status) {
      return status;
    }
    if (Status status = build_from_reader(reader2, *sel2, probe_store);
        !status) {
      return status;
    }
    build_from_store(build_store, *sel1);
  } else {
    if (Status status = build_from_reader(reader2, *sel2, probe_store);
        !status) {
      return status;
    }
    build_from_store(probe_store, *sel2);
  }

  std::vector<std::string_view> row;
  auto emit = [&](const csv::Record& left, const csv::Record& right) -> Status {
    row.clear();
    for (const auto field : left.fields) row.push_back(field);
    for (const auto field : right.fields) row.push_back(field);
    return writer.write_record(row);
  };
  auto emit_padded_left = [&](const csv::Record& right) -> Status {
    row.clear();
    for (std::size_t i = 0; i < width1; ++i) row.push_back(empty);
    for (const auto field : right.fields) row.push_back(field);
    return writer.write_record(row);
  };
  auto emit_padded_right = [&](const csv::Record& left) -> Status {
    row.clear();
    for (const auto field : left.fields) row.push_back(field);
    for (std::size_t i = 0; i < width2; ++i) row.push_back(empty);
    return writer.write_record(row);
  };
  auto emit_store_padded_left = [&](const util::RecordStore& store,
                                    std::size_t index_row) -> Status {
    row.clear();
    for (std::size_t i = 0; i < width1; ++i) row.push_back(empty);
    for (std::size_t i = 0; i < store.field_count(index_row); ++i) {
      row.push_back(store.field(index_row, i));
    }
    return writer.write_record(row);
  };
  auto emit_store = [&](const util::RecordStore& store, std::size_t index_row,
                        const csv::Record* right) -> Status {
    row.clear();
    for (std::size_t i = 0; i < store.field_count(index_row); ++i) {
      row.push_back(store.field(index_row, i));
    }
    for (const auto field : right->fields) row.push_back(field);
    return writer.write_record(row);
  };

  if (args.cross) {
    csv::Record left;
    while (true) {
      auto next = reader1.next(left);
      if (!next) return std::unexpected(next.error());
      if (!*next) break;
      for (std::size_t r = 0; r < probe_store.size(); ++r) {
        row.clear();
        for (const auto field : left.fields) row.push_back(field);
        for (std::size_t i = 0; i < probe_store.field_count(r); ++i) {
          row.push_back(probe_store.field(r, i));
        }
        if (Status status = writer.write_record(row); !status) {
          return std::unexpected(status.error());
        }
      }
    }
    return writer.flush();
  }

  if (args.right) {
    for (std::size_t right_row = 0; right_row < probe_store.size(); ++right_row) {
      row.clear();
      for (std::size_t i = 0; i < probe_store.field_count(right_row); ++i) {
        row.push_back(probe_store.field(right_row, i));
      }
      bool usable = false;
      std::string key = make_key(probe_store, right_row, *sel2, args.no_case,
                                 args.nulls, usable);
      bool matched = false;
      if (usable) {
        index.for_each_match(key, [&](std::size_t left_row) {
          matched = true;
          std::vector<std::string_view> full;
          for (std::size_t i = 0; i < build_store.field_count(left_row); ++i) {
            full.push_back(build_store.field(left_row, i));
          }
          for (std::size_t i = 0; i < probe_store.field_count(right_row); ++i) {
            full.push_back(probe_store.field(right_row, i));
          }
          if (Status status = writer.write_record(full); !status) {
            return;
          }
        });
      }
      if (!matched) {
        row.clear();
        for (std::size_t i = 0; i < width1; ++i) row.push_back(empty);
        for (std::size_t i = 0; i < probe_store.field_count(right_row); ++i) {
          row.push_back(probe_store.field(right_row, i));
        }
        if (Status status = writer.write_record(row); !status) {
          return std::unexpected(status.error());
        }
      }
    }
    return writer.flush();
  }

  if (args.full) written.assign(probe_store.size(), false);
  csv::Record left;
  while (true) {
    auto next = reader1.next(left);
    if (!next) return std::unexpected(next.error());
    if (!*next) break;
    bool usable = false;
    std::string key = make_key(left.fields, *sel1, args.no_case, args.nulls,
                               usable);
    bool matched = false;
    if (usable) {
      index.for_each_match(key, [&](std::size_t right_row) {
        matched = true;
        if (args.full) written[right_row] = true;
        row.clear();
        for (const auto field : left.fields) row.push_back(field);
        for (std::size_t i = 0; i < probe_store.field_count(right_row); ++i) {
          row.push_back(probe_store.field(right_row, i));
        }
        if (Status status = writer.write_record(row); !status) {
          return;
        }
      });
    }
    if (!matched && (args.left || args.full)) {
      if (Status status = emit_padded_right(left); !status) {
        return std::unexpected(status.error());
      }
    }
  }

  if (args.full) {
    for (std::size_t right_row = 0; right_row < probe_store.size(); ++right_row) {
      if (written[right_row]) continue;
      if (Status status = emit_store_padded_left(probe_store, right_row);
          !status) {
        return std::unexpected(status.error());
      }
    }
  }
  return writer.flush();
}

}  // namespace xsv::commands
