#include "command_util.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/commands.hpp"
#include "xsv/config.hpp"
#include "xsv/csv.hpp"

namespace xsv::commands {
namespace {

constexpr std::string_view kUsage = R"(Prints the fields of the first row in the CSV data.

These names can be used in commands like 'select' to refer to columns in the
CSV data.

Note that multiple CSV files may be given to this command. This is useful with
the --intersect flag.

Usage:
    xsv headers [options] [<input>...]

headers options:
    -j, --just-names       Only show the header names (hide column index).
                           This is automatically enabled if more than one
                           input is given.
    --intersect            Shows the intersection of all headers in all of
                           the inputs given.

Common options:
    -h, --help             Display this message
    -d, --delimiter <arg>  The field delimiter for reading CSV data.
                           Must be a single character. (default: ,)
)";

std::size_t display_width(std::string_view text) {
  std::size_t width = 0;
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
      ++width;
      ++i;
      continue;
    }
    if (i + extra >= text.size()) {
      ++width;
      ++i;
      continue;
    }
    bool valid = true;
    for (std::size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) valid = false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!valid) {
      ++width;
      ++i;
      continue;
    }
    const bool combining = (cp >= 0x0300 && cp <= 0x036F) ||
                           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
                           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
                           (cp >= 0x20D0 && cp <= 0x20FF) ||
                           (cp >= 0xFE20 && cp <= 0xFE2F);
    const bool wide = (cp >= 0x1100 && cp <= 0x115F) ||
                      (cp >= 0x2E80 && cp <= 0xA4CF) ||
                      (cp >= 0xAC00 && cp <= 0xD7A3) ||
                      (cp >= 0xF900 && cp <= 0xFAFF) ||
                      (cp >= 0xFE10 && cp <= 0xFE19) ||
                      (cp >= 0xFF00 && cp <= 0xFF60) ||
                      (cp >= 0xFFE0 && cp <= 0xFFE6) ||
                      (cp >= 0x1F300 && cp <= 0x1FAFF);
    if (!combining) width += wide ? 2 : 1;
    i += extra + 1;
  }
  return width;
}

std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t at = text.find(separator, start);
    if (at == std::string_view::npos) {
      parts.emplace_back(text.substr(start));
      break;
    }
    parts.emplace_back(text.substr(start, at - start));
    start = at + 1;
  }
  return parts;
}

struct Args {
  std::vector<std::string> inputs;
  bool just_names = false;
  bool intersect = false;
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
    if (!positional_only && (arg == "-j" || arg == "--just-names")) {
      args.just_names = true;
      continue;
    }
    if (!positional_only && arg == "--intersect") {
      args.intersect = true;
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
    args.inputs.emplace_back(arg);
  }
  return args;
}

}  // namespace

Status headers(std::span<const std::string_view> argv) {
  auto parsed = parse_args(argv);
  if (!parsed) return std::unexpected(parsed.error());
  Args& args = *parsed;
  if (args.help) {
    std::cout << kUsage;
    return success();
  }

  if (args.inputs.empty()) args.inputs.emplace_back("-");
  std::size_t stdin_count = 0;
  for (const std::string& input : args.inputs) {
    if (input == "-") ++stdin_count;
  }
  if (stdin_count > 1) return fail("At most one <stdin> input is allowed.");

  const bool show_index = args.inputs.size() == 1 && !args.just_names;

  std::vector<std::string> names;
  for (const std::string& input : args.inputs) {
    auto config = Config::from_path(input);
    if (!config) return std::unexpected(config.error());
    if (args.delimiter) config->delimiter = *args.delimiter;
    auto mapping = util::open_input(*config);
    if (!mapping) return std::unexpected(mapping.error());
    csv::ReaderOptions options = util::reader_options(*config, true);
    csv::Reader reader(mapping->bytes(), options);
    if (Status status = reader.prime(); !status) {
      return std::unexpected(status.error());
    }
    for (const std::string_view field : reader.headers().fields) {
      if (args.intersect) {
        const bool seen = std::any_of(
            names.begin(), names.end(),
            [&](const std::string& name) { return name == field; });
        if (seen) continue;
      }
      names.emplace_back(field);
    }
  }

  if (args.just_names) {
    for (const std::string& name : names) std::cout << name << '\n';
    std::cout.flush();
    if (!std::cout) return fail("failed to write output");
    return success();
  }

  // Emulate tabwriter: index and name are tab-separated cells, a tab inside a
  // name starts another cell, and a newline starts another line. Column widths
  // are max(minwidth=2, widest cell) and each non-final cell is padded with
  // `padding=2` spaces.
  std::vector<std::vector<std::string>> lines;
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::vector<std::string> name_lines = split(names[i], '\n');
    for (std::size_t part = 0; part < name_lines.size(); ++part) {
      std::vector<std::string> cells;
      if (show_index && part == 0) cells.push_back(std::to_string(i + 1));
      for (std::string& cell : split(name_lines[part], '\t')) {
        cells.push_back(std::move(cell));
      }
      lines.push_back(std::move(cells));
    }
  }
  // Faithful port of tabwriter's cell_widths: a block starts at the line
  // that introduces a column and ends at the first later line that does not
  // have that column. This is why a lone tab inside a header does not align
  // with the name column of the other headers.
  std::vector<std::vector<std::size_t>> widths(lines.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].empty()) continue;
    for (std::size_t col = widths[i].size(); col + 1 < lines[i].size(); ++col) {
      std::size_t width = 2;
      std::size_t contiguous = 0;
      for (std::size_t j = i; j < lines.size(); ++j) {
        if (col + 1 >= lines[j].size()) break;
        ++contiguous;
        width = std::max(width, display_width(lines[j][col]));
      }
      for (std::size_t j = i; j < i + contiguous; ++j) {
        widths[j].push_back(width);
      }
    }
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto& line = lines[i];
    for (std::size_t c = 0; c < line.size(); ++c) {
      std::cout << line[c];
      if (c < widths[i].size()) {
        const std::size_t pad = widths[i][c] - display_width(line[c]) + 2;
        std::cout << std::string(pad, ' ');
      }
    }
    std::cout << '\n';
  }
  std::cout.flush();
  if (!std::cout) return fail("failed to write output");
  return success();
}

}  // namespace xsv::commands
