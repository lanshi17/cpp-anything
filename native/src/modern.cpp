#include "cppan/modern.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

#include "cppan/util.hpp"

namespace cppan::modern {
namespace {

struct Rule {
  std::string_view needle;
  /// A second token the line must contain: `char` is only a raw buffer when an
  /// array follows it.
  std::optional<std::string_view> also;
  std::string_view idiom;
  std::string_view replacement;
  std::optional<std::string_view> feature;
};

constexpr Rule kRules[] = {
    {"malloc(", std::nullopt, "malloc/free", "std::vector / std::unique_ptr / std::array",
     std::nullopt},
    {"free(", std::nullopt, "manual free", "RAII: std::vector / std::unique_ptr", std::nullopt},
    {"new ", std::nullopt, "raw new", "std::make_unique / std::make_shared", std::nullopt},
    {"delete", std::nullopt, "raw delete", "RAII: std::unique_ptr / std::shared_ptr",
     std::nullopt},
    {"NULL", std::nullopt, "NULL", "nullptr", std::nullopt},
    {"printf(", std::nullopt, "printf", "std::print / std::format", "__cpp_lib_print"},
    {"sprintf(", std::nullopt, "sprintf", "std::format", "__cpp_lib_format"},
    {"strcpy", std::nullopt, "strcpy", "std::string assignment / string_view copy",
     "__cpp_lib_string_view"},
    {"strlen", std::nullopt, "strlen in a loop",
     "std::string_view::size() / std::ranges::distance", "__cpp_lib_string_view"},
    {"memcpy", std::nullopt, "memcpy", "std::copy / std::ranges::copy, or a typed container",
     "__cpp_lib_ranges"},
    {"typedef struct", std::nullopt, "typedef struct",
     "plain struct (no typedef needed in C++)", std::nullopt},
    {"(char*)", std::nullopt, "C-style cast",
     "static_cast<char*> / reinterpret_cast where unavoidable", std::nullopt},
    {"(int)", std::nullopt, "C-style cast",
     "static_cast<int> / std::size_t with gsl::narrow semantics", std::nullopt},
    {"(double)", std::nullopt, "C-style cast", "static_cast<double>", std::nullopt},
    {"for (int ", std::nullopt, "index loop",
     "range-for / std::views on a span or container", "__cpp_lib_ranges"},
    {"char ", std::string_view("["), "raw char buffer",
     "std::string / std::array<char, N> / std::span", "__cpp_lib_span"},
};

std::string_view trim_start(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  return text;
}

}  // namespace

std::string Opportunity::describe() const {
  if (feature.has_value()) {
    return "modernize line " + std::to_string(line) + ": " + idiom + " → " + replacement +
           " (needs `" + *feature + "`)";
  }
  return "modernize line " + std::to_string(line) + ": " + idiom + " → " + replacement;
}

std::vector<Opportunity> scan(std::string_view source,
                              const std::vector<langc::CFunction>& functions) {
  std::vector<std::pair<std::uint32_t, std::string>> starts;
  for (const langc::CFunction& function : functions) {
    starts.emplace_back(std::max<std::uint32_t>(function.line, 1), function.name);
  }
  std::vector<Opportunity> out;
  std::set<std::tuple<std::string, std::uint32_t, std::string>> seen;
  const auto lines = split_lines(source);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::uint32_t number = static_cast<std::uint32_t>(index + 1);
    std::string_view code = lines[index];
    if (const std::size_t comment = code.find("//"); comment != std::string_view::npos) {
      code = code.substr(0, comment);
    }
    const std::string_view leading = trim_start(code);
    if (leading.starts_with('*') || leading.starts_with("/*")) continue;
    for (const Rule& rule : kRules) {
      if (code.find(rule.needle) == std::string_view::npos) continue;
      if (rule.also.has_value() && code.find(*rule.also) == std::string_view::npos) continue;
      std::string function_name = "<top level>";
      std::uint32_t best = 0;
      for (const auto& [start, name] : starts) {
        if (start <= number && (function_name == "<top level>" || start >= best)) {
          best = start;
          function_name = name;
        }
      }
      if (!seen.emplace(function_name, number, std::string(rule.idiom)).second) continue;
      Opportunity opportunity;
      opportunity.function = function_name;
      opportunity.line = number;
      opportunity.idiom = std::string(rule.idiom);
      opportunity.replacement = std::string(rule.replacement);
      if (rule.feature.has_value()) opportunity.feature = std::string(*rule.feature);
      out.push_back(std::move(opportunity));
    }
  }
  return out;
}

}  // namespace cppan::modern
