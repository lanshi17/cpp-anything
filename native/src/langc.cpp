#include "cppan/langc.hpp"

#include <algorithm>
#include <cctype>
#include <format>

#include "cppan/modern.hpp"

namespace cppan::langc {
namespace {

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

std::string collapse(std::string text) {
  std::string out;
  bool space = false;
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += ch;
  }
  return out;
}

std::string replace_all(std::string text, std::string_view needle, std::string_view with) {
  if (needle.empty()) return text;
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), with);
    at += with.size();
  }
  return text;
}

std::string strip_comments(std::string_view source) {
  std::string out;
  out.reserve(source.size());
  std::size_t index = 0;
  while (index < source.size()) {
    if (source.substr(index).starts_with("/*")) {
      const std::size_t end = source.find("*/", index + 2);
      if (end == std::string_view::npos) break;
      const std::string_view block = source.substr(index, end + 2 - index);
      for (const char ch : block) {
        if (ch == '\n') out += '\n';
      }
      index = end + 2;
      continue;
    }
    if (source.substr(index).starts_with("//")) {
      while (index < source.size() && source[index] != '\n') ++index;
      continue;
    }
    if (source[index] == '"') {
      out += '"';
      ++index;
      while (index < source.size()) {
        const char ch = source[index];
        out += ch;
        ++index;
        if (ch == '\\' && index < source.size()) {
          out += source[index];
          ++index;
        } else if (ch == '"') {
          break;
        }
      }
      continue;
    }
    out += source[index];
    ++index;
  }
  return out;
}

std::optional<std::pair<std::string, std::string>> split_head(std::string_view head) {
  std::size_t split = std::string_view::npos;
  for (std::size_t index = head.size(); index > 0; --index) {
    if (std::isspace(static_cast<unsigned char>(head[index - 1]))) {
      split = index - 1;
      break;
    }
  }
  if (split == std::string_view::npos) return std::nullopt;
  std::string name(trim(head.substr(split)));
  std::string ret(trim(head.substr(0, split)));
  if (name.empty() || ret.empty() || name.find('*') != std::string::npos) return std::nullopt;
  for (const std::string_view keyword :
       {"if", "for", "while", "switch", "return", "else", "do"}) {
    if (ret == keyword) return std::nullopt;
  }
  return std::pair{ret, name};
}

std::optional<Ty> map_ctype(std::string_view raw, Lang lang) {
  std::string cleaned = replace_all(std::string(raw), "const", " ");
  cleaned = replace_all(std::move(cleaned), "volatile", " ");
  cleaned = replace_all(std::move(cleaned), "restrict", " ");
  cleaned = replace_all(std::move(cleaned), "*", " * ");
  cleaned = collapse(std::move(cleaned));
  const std::string_view text = trim(cleaned);
  if (text == "char *" || text == "signed char *" || text == "unsigned char *") {
    return Ty{Ty::Kind::Str, {}};
  }
  for (const std::string_view candidate :
       {"int", "long", "long int", "short", "short int", "signed", "signed int", "unsigned",
        "unsigned int", "unsigned long", "unsigned long int", "unsigned short", "size_t",
        "ssize_t", "int64_t", "uint64_t", "int32_t", "uint32_t"}) {
    if (text == candidate) return Ty{Ty::Kind::Int, {}};
  }
  if (text == "double" || text == "long double" || text == "float") {
    return Ty{Ty::Kind::Float, {}};
  }
  if (text == "bool" || text == "_Bool") return Ty{Ty::Kind::Bool, {}};
  if (text == "void") return Ty{Ty::Kind::None, {}};
  if (lang == Lang::Cpp) {
    std::string cpp = replace_all(std::string(cleaned), "const", " ");
    cpp = replace_all(std::move(cpp), "&", " ");
    cpp = collapse(std::move(cpp));
    const std::string_view name = trim(cpp);
    if (name == "std::string" || name == "string" || name == "std::string_view" ||
        name == "string_view" || name == "const std::string" || name == "const string") {
      return Ty{Ty::Kind::Str, {}};
    }
    if (name == "std::int64_t" || name == "int64_t" || name == "std::int32_t" ||
        name == "int32_t" || name == "long long" || name == "long long int" ||
        name == "unsigned long long") {
      return Ty{Ty::Kind::Int, {}};
    }
    if (name == "long double") return Ty{Ty::Kind::Float, {}};
  }
  return std::nullopt;
}

std::pair<std::string, std::optional<std::string>> split_param(std::string_view part) {
  std::string cleaned = replace_all(std::string(part), "*", " * ");
  std::vector<std::string> words;
  {
    std::string current;
    for (const char ch : cleaned) {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!current.empty()) {
          words.push_back(current);
          current.clear();
        }
      } else {
        current += ch;
      }
    }
    if (!current.empty()) words.push_back(current);
  }
  if (words.empty()) return {std::string(), std::nullopt};
  const std::string& last = words.back();
  bool is_name = std::isalpha(static_cast<unsigned char>(last.front())) != 0;
  for (const char ch : last) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_')) is_name = false;
  }
  for (const std::string_view keyword :
       {"int", "char", "double", "float", "void", "long", "short", "bool", "size_t", "unsigned",
        "signed", "const", "volatile", "restrict"}) {
    if (last == keyword) is_name = false;
  }
  if (words.size() >= 2 && is_name) {
    std::string type;
    for (std::size_t index = 0; index + 1 < words.size(); ++index) {
      if (index > 0) type += ' ';
      type += words[index];
    }
    return {type, last};
  }
  std::string joined;
  for (std::size_t index = 0; index < words.size(); ++index) {
    if (index > 0) joined += ' ';
    joined += words[index];
  }
  return {joined, std::nullopt};
}

using Signature = std::tuple<Ty, std::vector<std::pair<Ty, std::string>>,
                             std::vector<std::pair<std::string, std::string>>>;

Result<Signature> map_signature(std::string_view ret_c, std::string_view params_text, Lang lang) {
  const std::string_view ret_raw = trim(ret_c);
  const auto ret = map_ctype(ret_raw, lang);
  if (!ret.has_value()) {
    return std::unexpected("return type \"" + std::string(ret_raw) + "\" has no supported "
                                                                       "mapping");
  }
  const std::string_view trimmed_params = trim(params_text);
  if (trimmed_params.empty() || trimmed_params == "void") {
    return Signature{*ret, {}, {}};
  }
  std::vector<std::pair<Ty, std::string>> params;
  std::vector<std::pair<std::string, std::string>> params_c;
  std::size_t start = 0;
  std::size_t index = 0;
  std::size_t part_index = 0;
  while (index <= trimmed_params.size()) {
    if (index == trimmed_params.size() || trimmed_params[index] == ',') {
      const std::string_view raw_part = trim(trimmed_params.substr(start, index - start));
      const std::string_view part = trim(raw_part);
      if (part == "...") {
        return std::unexpected(
            "variadic function: the port would need a C++ overload set");
      }
      auto [type_text, parameter_name] = split_param(part);
      const std::string_view type_view = trim(type_text);
      const auto ty = map_ctype(type_view, lang);
      if (!ty.has_value()) {
        return std::unexpected("parameter \"" + std::string(part) + "\" has no supported "
                                                                     "mapping");
      }
      const std::string name = parameter_name.value_or("arg" + std::to_string(part_index));
      params.emplace_back(*ty, name);
      params_c.emplace_back(type_text, name);
      ++part_index;
      start = index + 1;
    }
    ++index;
  }
  return Signature{*ret, std::move(params), std::move(params_c)};
}

std::string describe(const CFunction& function) {
  std::string params;
  for (const auto& [type, name] : function.params_c) {
    if (!params.empty()) params += ", ";
    params += type + " " + name;
  }
  return function.ret_c + " " + function.name + "(" + params + ")";
}

Function to_function(const CFunction& function) {
  Function out;
  out.name = function.name;
  out.line = function.line;
  for (const auto& [ty, name] : function.params) {
    Param param;
    param.name = name;
    for (const auto& [c_type, candidate] : function.params_c) {
      if (candidate == name) {
        param.annotation = c_type;
        break;
      }
    }
    param.ty = ty;
    out.params.push_back(std::move(param));
  }
  out.returns = function.ret;
  out.returns_annotation = function.ret_c;
  out.risks = {"C signature: " + describe(function)};
  return out;
}

}  // namespace

std::string_view id(Lang lang) { return lang == Lang::C ? "c" : "cpp"; }

std::string_view label(Lang lang) { return lang == Lang::C ? "C" : "C-style C++"; }

std::pair<std::string_view, std::string_view> linkage(Lang lang) {
  return lang == Lang::C ? std::pair<std::string_view, std::string_view>{"extern \"C\" {", "}"}
                         : std::pair<std::string_view, std::string_view>{"", ""};
}

std::string_view cmake_languages(Lang lang) { return lang == Lang::C ? "C CXX" : "CXX"; }

std::optional<Lang> parse_lang(std::string_view name) {
  if (name == "c") return Lang::C;
  if (name == "cpp" || name == "c++" || name == "cxx" || name == "c-style" ||
      name == "cstyle" || name == "modernize" || name == "modern") {
    return Lang::Cpp;
  }
  return std::nullopt;
}

Result<Analysis> analyze_file(const std::filesystem::path& path, Lang lang) {
  const auto source = read_file(path);
  if (!source.has_value()) return std::unexpected(path.string() + ": cannot read");
  Analysis analysis;
  analysis.lines = static_cast<std::uint32_t>(line_count(*source));
  const std::string stripped = strip_comments(*source);
  std::size_t offset = 0;
  while (offset < stripped.size()) {
    const std::size_t open_rel = stripped.find('(', offset);
    if (open_rel == std::string::npos) break;
    const std::size_t open = open_rel;
    const std::size_t newline = stripped.rfind('\n', open);
    const std::size_t line_start = newline == std::string::npos ? 0 : newline + 1;
    const std::string_view head = trim(std::string_view(stripped).substr(
        line_start, open - line_start));
    const std::size_t close_rel = stripped.find(')', open);
    if (close_rel == std::string::npos) break;
    const std::size_t close = close_rel;
    const std::string_view after = trim(std::string_view(stripped).substr(close + 1));
    if (!after.starts_with('{')) {
      offset = close + 1;
      continue;
    }
    const auto split = split_head(head);
    if (!split.has_value()) {
      offset = close + 1;
      continue;
    }
    const std::string_view params_text =
        trim(std::string_view(stripped).substr(open + 1, close - open - 1));
    auto signature = map_signature(split->first, params_text, lang);
    if (signature) {
      auto& [ret, params, params_c] = *signature;
      std::uint32_t line = 1;
      for (std::size_t byte = 0; byte < line_start; ++byte) {
        if (stripped[byte] == '\n') ++line;
      }
      analysis.functions.push_back(CFunction{split->second, line, ret, split->first,
                                             std::move(params), std::move(params_c)});
    } else {
      analysis.skipped.emplace_back(split->second, signature.error());
    }
    const std::size_t brace_rel = stripped.find('{', close);
    if (brace_rel == std::string::npos) break;
    int depth = 0;
    std::size_t index = brace_rel;
    while (index < stripped.size()) {
      if (stripped[index] == '{') {
        ++depth;
      } else if (stripped[index] == '}') {
        --depth;
        if (depth == 0) break;
      }
      ++index;
    }
    offset = index + 1;
  }
  return analysis;
}

Result<Unit> unit(const std::filesystem::path& repo, std::string_view relative_or_path,
                  Lang lang) {
  std::error_code error;
  const std::filesystem::path raw = std::filesystem::path(relative_or_path).is_absolute()
                                        ? std::filesystem::path(relative_or_path)
                                        : repo / relative_or_path;
  const auto file = std::filesystem::canonical(raw, error);
  if (error) return std::unexpected(std::string(relative_or_path) + ": " + error.message());
  auto analysis = analyze_file(file, lang);
  if (!analysis) return std::unexpected(analysis.error());
  if (analysis->functions.empty()) {
    return std::unexpected(std::format(
        "{} has no functions with a supported C signature ({} skipped)", file.string(),
        analysis->skipped.size()));
  }
  const auto repo_abs = std::filesystem::canonical(repo, error);
  std::string module;
  {
    const auto relative = std::filesystem::relative(file, error ? repo : repo_abs, error);
    module = (error ? file : relative).string();
  }
  Unit out;
  out.unit = file.stem().string();
  if (out.unit.empty()) out.unit = "unit";
  out.module = module;
  out.file = file;
  out.package_root = repo_abs;
  out.lines = analysis->lines;
  for (const CFunction& function : analysis->functions) out.functions.push_back(to_function(function));
  out.skipped = analysis->skipped;
  if (lang == Lang::C) {
    out.risks = {
        "C has no exceptions: a failure is a return code or an errno, and the port must mirror "
        "it",
        "char* strings: null versus empty is observable, and the recorder treats a null return "
        "as null",
        "Fixed-width C types are not Python's unbounded ints: the signature decides the width",
        "Undefined behaviour on inputs outside the contract is a case, not a failure (the "
        "harness records the signal)",
    };
  } else {
    out.risks = {
        "C-style C++ ports to modern C++: the behaviour must survive the idioms (span, "
        "string_view, ranges, format)",
        "char* and raw arrays: every offset must be re-expressed on a modern container without "
        "changing results",
        "printf/iostream formatting differs from std::format for padding, precision and floats: "
        "record the text",
        "Undefined behaviour (out-of-bounds, dangling pointers) is recorded as a crash and "
        "accepts any port behaviour",
    };
    const auto source = read_file(file).value_or(std::string());
    const auto opportunities = modern::scan(source, analysis->functions);
    for (std::size_t index = 0; index < opportunities.size() && index < 20; ++index) {
      out.risks.push_back(opportunities[index].function + ": " + opportunities[index].describe());
    }
    if (opportunities.size() > 20) {
      out.risks.push_back(std::format("… {} more modernization opportunities; see `analyze "
                                      "--lang cpp`",
                                      opportunities.size() - 20));
    }
  }
  return out;
}

Result<Json> analyze_repo(const std::filesystem::path& repo, Lang lang) {
  std::vector<Json> modules;
  std::vector<std::pair<std::filesystem::path, std::size_t>> stack = {{repo, 0}};
  while (!stack.empty()) {
    auto [directory, depth] = stack.back();
    stack.pop_back();
    if (depth > 4) continue;
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) return std::unexpected(directory.string() + ": " + error.message());
    for (const auto& entry : iterator) {
      const std::string name = entry.path().filename().string();
      if (entry.is_directory(error)) {
        if (name == "build" || name == "node_modules" || name == "target" || name == "vendor" ||
            name == ".git" || name == "cpp" || name == "harness") {
          continue;
        }
        stack.emplace_back(entry.path(), depth + 1);
        continue;
      }
      const std::string extension = entry.path().extension().string();
      const std::string_view bare = extension.empty() ? std::string_view()
                                                      : std::string_view(extension).substr(1);
      const bool wanted = lang == Lang::C ? bare == "c"
                                          : (bare == "cpp" || bare == "cc" || bare == "cxx" ||
                                             bare == "c++");
      if (!wanted) continue;
      auto analysis = analyze_file(entry.path(), lang);
      if (!analysis || analysis->functions.empty()) continue;
      const auto relative = std::filesystem::relative(entry.path(), repo, error);
      const std::string relative_text = (error ? entry.path() : relative).string();
      std::vector<modern::Opportunity> modernization;
      if (lang == Lang::Cpp) {
        modernization = modern::scan(read_file(entry.path()).value_or(std::string()),
                                     analysis->functions);
      }
      Json::Array functions;
      for (const CFunction& function : analysis->functions) {
        functions.push_back(Json::object({
            {"name", Json::string(function.name)},
            {"params", Json::integer(static_cast<std::int64_t>(function.params.size()))},
            {"typed", Json::integer(static_cast<std::int64_t>(function.params.size()))},
            {"returns", Json::string(function.ret_c)},
        }));
      }
      Json::Array risks;
      if (lang == Lang::C) {
        risks = {Json::string("C return codes and errno are the failure contract"),
                 Json::string("char* may be null; the port must keep the difference from \"\""),
                 Json::string("undefined behaviour is recorded as a crash, not as a failure")};
      } else {
        risks = {Json::string("C-style C++: the port replaces raw loops, buffers and printf with "
                              "modern facilities"),
                 Json::string("char* and fixed arrays must become string_view/span without "
                              "changing offsets"),
                 Json::string("undefined behaviour is recorded as a crash, not as a failure")};
        for (std::size_t index = 0; index < modernization.size() && index < 30; ++index) {
          risks.push_back(Json::string(modernization[index].function + ": " +
                                       modernization[index].describe()));
        }
      }
      modules.push_back(Json::object({
          {"module", Json::string(relative_text)},
          {"path", Json::string(relative_text)},
          {"lines", Json::integer(analysis->lines)},
          {"functions", Json::array(std::move(functions))},
          {"risks", Json::array(std::move(risks))},
          {"imports", Json::array()},
          {"entry_point", Json::boolean(false)},
      }));
    }
  }
  return Json::object({
      {"root", Json::string(repo.string())},
      {"language", Json::string(std::string(id(lang)))},
      {"modules", Json::array(std::move(modules))},
      {"dependencies", Json::array()},
  });
}

}  // namespace cppan::langc
