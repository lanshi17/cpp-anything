// `cli::Parsed`: the parsed command line. Kept in its own leaf module so both the
// dispatcher and every command module can use it without a module cycle.
module;
#include <cctype>
#include <cstdio>

module cppan.args;
import std;
import cppan.util;

namespace cppan::cli {

std::optional<std::string> Parsed::value(std::string_view name) const {
  if (auto found = values.find(std::string(name)); found != values.end()) return found->second;
  return std::nullopt;
}

bool Parsed::flag(std::string_view name) const { return flags.contains(std::string(name)); }

std::vector<std::string> Parsed::many(std::string_view name) const {
  if (auto found = multi.find(std::string(name)); found != multi.end()) return found->second;
  return {};
}

std::optional<std::uint64_t> Parsed::number(std::string_view name) const {
  const auto text = value(name);
  if (!text.has_value()) return std::nullopt;
  try {
    std::size_t used = 0;
    const auto number = std::stoull(*text, &used);
    if (used != text->size()) return std::nullopt;
    return number;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

Result<Parsed> parse_args(const std::vector<std::string>& argv) {
  static const std::set<std::string> kBooleanFlags = {
      "json",  "force", "clean",       "check", "strict-errors", "help",
      "list",  "bench", "refresh",     "offline", "library"};
  Parsed parsed;
  std::size_t index = 0;
  while (index < argv.size()) {
    const std::string& arg = argv[index];
    if (arg == "--") {
      parsed.trailing.assign(argv.begin() + static_cast<std::ptrdiff_t>(index + 1), argv.end());
      break;
    }
    if (arg.starts_with("--")) {
      const std::string body = arg.substr(2);
      if (const std::size_t equals = body.find('='); equals != std::string::npos) {
        const std::string key = body.substr(0, equals);
        const std::string value = body.substr(equals + 1);
        parsed.values[key] = value;
        parsed.multi[key].push_back(value);
      } else if (kBooleanFlags.contains(body)) {
        parsed.flags.insert(body);
      } else if (index + 1 < argv.size() && !argv[index + 1].starts_with("--")) {
        parsed.values[body] = argv[index + 1];
        parsed.multi[body].push_back(argv[index + 1]);
        ++index;
      } else {
        parsed.flags.insert(body);
      }
    } else if (arg == "-h") {
      parsed.flags.insert("help");
    } else if (arg == "-o") {
      if (index + 1 >= argv.size()) return std::unexpected("-o needs a file name");
      parsed.values["out"] = argv[index + 1];
      ++index;
    } else {
      parsed.positional.push_back(arg);
    }
    ++index;
  }
  return parsed;
}

}  // namespace cppan::cli
