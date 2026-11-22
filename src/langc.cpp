module;
#include <cctype>
#include "cppan/harness_c_cpp.hpp"

module cppan.langc;
import cppan.json;
import cppan.types;
import cppan.util;
import cppan.inventory;
import cppan.modern;
import std;

namespace cppan::langc {
namespace {

std::string embedded_text(std::string_view data, std::size_t size) {
  return std::string(data.substr(0, size));
}

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

std::vector<modern::FunctionStart> function_starts(const Analysis& analysis) {
  std::vector<modern::FunctionStart> starts;
  for (const CFunction& function : analysis.functions) {
    starts.push_back(modern::FunctionStart{function.line, function.name});
  }
  return starts;
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
    const auto opportunities = modern::scan(source, function_starts(*analysis));
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
                                     function_starts(*analysis));
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

namespace {

constexpr std::string_view kStrings[] = {
    "",          " ",          "Hello World", "  Mixed CASE  ", "café — naïve",
    "a",         "123",        "-",           "trailing---",    " leading",
    "one\ntwo",  "Москва",     "0",           "a-b_c",          "x",
};
constexpr std::int64_t kInts[] = {0, 1, -1, 7, 42, 12, 100, -100, 10'000, 3, 19, 255, -7, 2'000'000, 8, 11};
constexpr double kFloats[] = {0.0, -0.0, 1.5, -2.25, 0.1, 2.0, -0.5, 1e-9, 123456.789, -1.0, 7.0, 1e6, 3.25, -99.5, 0.25};

std::vector<Json> values_for(const Ty& ty) {
  switch (ty.kind) {
    case Ty::Kind::Str: {
      std::vector<Json> out;
      for (const std::string_view text : kStrings) out.push_back(Json::string(std::string(text)));
      return out;
    }
    case Ty::Kind::Int: {
      std::vector<Json> out;
      for (const std::int64_t value : kInts) out.push_back(Json::integer(value));
      return out;
    }
    case Ty::Kind::Float: {
      std::vector<Json> out;
      for (const double value : kFloats) out.push_back(Json::real(value));
      return out;
    }
    case Ty::Kind::Bool: return {Json::boolean(true), Json::boolean(false)};
    case Ty::Kind::None: return {Json::null()};
    default: return {Json::null()};
  }
}

std::filesystem::path harness_dir(const Project& project) { return project.dir / "harness"; }

std::filesystem::path harness_exe(const Project& project) {
#if defined(_WIN32)
  return harness_dir(project) / "build" / "cppan_harness.exe";
#else
  return harness_dir(project) / "build" / "cppan_harness";
#endif
}

}  // namespace

std::vector<Json> calls(const Unit& unit, std::size_t count) {
  std::vector<Json> out;
  for (const Function& function : unit.functions) {
    std::vector<std::vector<Json>> corpora;
    for (const Param& param : function.params) corpora.push_back(values_for(param.ty));
    if (corpora.empty()) {
      out.push_back(Json::object({
          {"id", Json::string(function.name + ":0")},
          {"fn", Json::string(function.name)},
          {"args", Json::array()},
      }));
      continue;
    }
    std::size_t total = 0;
    for (const auto& values : corpora) total = std::max(total, values.size());
    total = std::min(total, std::max<std::size_t>(count, 1));
    for (std::size_t index = 0; index < total; ++index) {
      Json::Array args;
      for (const auto& values : corpora) args.push_back(values[index % values.size()]);
      out.push_back(Json::object({
          {"id", Json::string(function.name + ":" + std::to_string(index))},
          {"fn", Json::string(function.name)},
          {"args", Json::array(std::move(args))},
      }));
    }
  }
  return out;
}

std::string harness_source(const std::vector<CFunction>& functions, Lang lang) {
  std::string decls;
  for (const CFunction& function : functions) {
    std::string params;
    for (const auto& [type, name] : function.params_c) {
      if (!params.empty()) params += ", ";
      params += type + " " + name;
    }
    if (params.empty()) params = "void";
    decls += std::format("  {} cppan_original_{}({});\n", function.ret_c, function.name, params);
  }
  std::string dispatch;
  for (const CFunction& function : functions) {
    dispatch += std::format("    if (name == \"{}\") {{\n", function.name);
    for (std::size_t index = 0; index < function.params.size(); ++index) {
      const Ty& ty = function.params[index].first;
      const std::string& original =
          index < function.params_c.size() ? function.params_c[index].first : std::string();
      if (ty.kind == Ty::Kind::Str && original.find('*') != std::string::npos) {
        dispatch += std::format(
            "      const std::string arg{}_text = args.at({}).as_string();\n      const char* "
            "arg{} = arg{}_text.c_str();\n",
            index, index, index, index);
      } else if (ty.kind == Ty::Kind::Str) {
        dispatch += std::format(
            "      const std::string arg{}_text = args.at({}).as_string();\n      const "
            "std::string& arg{} = arg{}_text;\n",
            index, index, index, index);
      } else if (ty.kind == Ty::Kind::Int) {
        dispatch += std::format(
            "      const auto arg{} = static_cast<long long>(args.at({}).as_int());\n", index,
            index);
      } else if (ty.kind == Ty::Kind::Float) {
        dispatch += std::format("      const double arg{} = args.at({}).as_number();\n", index,
                                index);
      } else if (ty.kind == Ty::Kind::Bool) {
        dispatch += std::format("      const bool arg{} = args.at({}).as_bool();\n", index, index);
      } else {
        dispatch += std::format("      const Json& arg{} = args.at({});\n", index, index);
      }
    }
    std::string arguments;
    for (std::size_t index = 0; index < function.params.size(); ++index) {
      if (index > 0) arguments += ", ";
      arguments += "arg" + std::to_string(index);
    }
    const std::string call = "cppan_original_" + function.name + "(" + arguments + ")";
    if (function.ret.kind == Ty::Kind::Str && function.ret_c.find('*') != std::string::npos) {
      dispatch += std::format(
          "      char* result = {};\n      return ok_json(result != nullptr ? "
          "Json::string(std::string(result)) : Json::null());\n",
          call);
    } else if (function.ret.kind == Ty::Kind::Str) {
      dispatch += std::format(
          "      const std::string result = {};\n      return ok_json(Json::string(result));\n",
          call);
    } else if (function.ret.kind == Ty::Kind::Int) {
      dispatch += std::format(
          "      return ok_json(Json::integer(static_cast<std::int64_t>({})));\n", call);
    } else if (function.ret.kind == Ty::Kind::Float) {
      dispatch += std::format("      return ok_json(Json::real(static_cast<double>({})));\n",
                              call);
    } else if (function.ret.kind == Ty::Kind::Bool) {
      dispatch += std::format("      return ok_json(Json::boolean({} != 0));\n", call);
    } else {
      dispatch += std::format("      {};\n      return ok_json(Json::null());\n", call);
    }
    dispatch += "    }\n";
  }
  const auto [linkage_open, linkage_close] = linkage(lang);
  std::string text = embedded_text(data::harness_c_cpp, data::harness_c_cpp_size);
  text = replace_all(std::move(text), "{{LINKAGE_OPEN}}", linkage_open);
  text = replace_all(std::move(text), "{{LINKAGE_CLOSE}}", linkage_close);
  text = replace_all(std::move(text), "{{DECLS}}", decls);
  text = replace_all(std::move(text), "{{DISPATCH}}", dispatch);
  return text;
}

Result<std::filesystem::path> build_harness(const Project& project, const Unit& unit, Lang lang) {
  auto analysis = analyze_file(project.source_file, lang);
  if (!analysis) return std::unexpected(analysis.error());
  const std::filesystem::path dir = harness_dir(project);
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  if (error) return std::unexpected(dir.string() + ": " + error.message());
  {
    auto written = write_file(dir / "main.cpp", harness_source(analysis->functions, lang));
    if (!written) return std::unexpected(written.error());
  }
  std::string definitions;
  for (const CFunction& function : analysis->functions) {
    if (!definitions.empty()) definitions += ";";
    definitions += function.name + "=cppan_original_" + function.name;
  }
  std::string cmake = std::format(
      R"(cmake_minimum_required(VERSION 3.28)
project(cppan_harness LANGUAGES {})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(cppan_harness main.cpp "{}")
target_include_directories(cppan_harness PRIVATE "{}")
target_compile_definitions(cppan_harness PRIVATE {})
if(MSVC)
  target_compile_options(cppan_harness PRIVATE /W3 /utf-8)
else()
  target_compile_options(cppan_harness PRIVATE -Wall)
endif()
)",
      cmake_languages(lang), project.source_file.string(),
      (project.dir / "include").string(), definitions);
  {
    auto written = write_file(dir / "CMakeLists.txt", cmake);
    if (!written) return std::unexpected(written.error());
  }
  (void)unit;
  const Run configured = run_command("cmake",
                                     {"-S", dir.string(), "-B", (dir / "build").string(),
                                      "-DCMAKE_BUILD_TYPE=Release"},
                                     dir);
  if (!configured.ok) {
    return std::unexpected("harness configure failed: " + configured.failure() + "\n" +
                           indent_text(configured.err, 2));
  }
  const Run built = run_command("cmake",
                                {"--build", (dir / "build").string(), "-j", "4"}, dir);
  if (!built.ok) {
    return std::unexpected("harness build failed: " + built.failure() + "\n" +
                           indent_text(built.err, 2));
  }
  const auto exe = harness_exe(project);
  if (!std::filesystem::exists(exe)) {
    return std::unexpected("harness build succeeded but " + exe.string() + " is missing");
  }
  return exe;
}

Result<Recorded> record(const Project& project, const Unit& unit, std::size_t count, Lang lang) {
  auto exe = build_harness(project, unit, lang);
  if (!exe) return std::unexpected(exe.error());
  const std::vector<Json> generated = calls(unit, count);
  std::string text;
  for (const Json& call : generated) {
    text += dump(call);
    text += '\n';
  }
  const std::filesystem::path calls_path = project.dir / kMetaDir / "calls.jsonl";
  const std::filesystem::path results_path = project.dir / kMetaDir / "original-results.jsonl";
  {
    auto written = write_file(calls_path, text);
    if (!written) return std::unexpected(written.error());
  }
  const Run run = run_command(exe->string(),
                              {"--cases", calls_path.string(), "--out", results_path.string()},
                              project.dir);
  if (!run.ok) {
    return std::unexpected("the harness failed: " + run.failure() + "\n" +
                           indent_text(run.err, 2));
  }
  const auto results = read_file(results_path);
  if (!results.has_value()) return std::unexpected(results_path.string() + ": cannot read");
  std::map<std::string, std::pair<std::string, Json>> by_id;
  for (const Json& call : generated) {
    const Json* id = call.find("id");
    const Json* function = call.find("fn");
    const Json* args = call.find("args");
    if (id != nullptr && id->is_string() && function != nullptr && function->is_string()) {
      by_id[id->as_string()] = {function->as_string(),
                                args != nullptr ? *args : Json::array()};
    }
  }
  std::string cases;
  std::size_t crashes = 0;
  std::size_t count_cases = 0;
  for (const std::string_view line : split_lines(*results)) {
    bool empty = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
    }
    if (empty) continue;
    Json doc;
    try {
      doc = parse(line);
    } catch (const JsonError& error) {
      return std::unexpected(std::string("harness output: ") + error.what());
    }
    const Json* id_field = doc.find("id");
    const std::string id = id_field != nullptr && id_field->is_string() ? id_field->as_string() : "";
    const auto found = by_id.find(id);
    if (found == by_id.end()) continue;
    const Json* result_field = doc.find("result");
    const Json result = result_field != nullptr ? *result_field : Json::null();
    Json expect = Json::null();
    if (const Json* error = result.find("err"); error != nullptr) {
      const Json* kind_field = error->find("type");
      const Json* message_field = error->find("msg");
      const std::string kind =
          kind_field != nullptr && kind_field->is_string() ? kind_field->as_string() : "";
      const std::string message =
          message_field != nullptr && message_field->is_string() ? message_field->as_string() : "";
      if (kind == "Crash") {
        ++crashes;
        expect = Json::object({{"crash", Json::string(message)}});
      } else {
        expect = Json::object({
            {"err", Json::object({{"type", Json::string(kind.empty() ? "CError" : kind)},
                                  {"msg", Json::string(message)}})},
        });
      }
    } else {
      const Json* ok = result.find("ok");
      expect = Json::object({{"ok", ok != nullptr ? *ok : Json::null()}});
    }
    const Json entry = Json::object({
        {"id", Json::string(id)},
        {"fn", Json::string(found->second.first)},
        {"args", found->second.second},
        {"expect", expect},
    });
    cases += dump(entry);
    cases += '\n';
    ++count_cases;
  }
  {
    auto written = write_file(project.dir / "cases.jsonl", cases);
    if (!written) return std::unexpected(written.error());
  }
  const Json recorded_from = Json::object({
      {"schema", Json::string("cpp-anything/recorded-from@1")},
      {"source_hash", Json::string(hash64(read_file(project.source_file).value_or(std::string())))},
      {"from_tests", Json::object({{"calls", Json::integer(static_cast<std::int64_t>(count_cases))},
                                   {"at", Json::string(now_iso8601())}})},
  });
  {
    auto written = write_file(project.dir / kMetaDir / "recorded-from.json",
                              dump(recorded_from, true));
    if (!written) return std::unexpected(written.error());
  }
  return Recorded{count_cases, crashes};
}

Result<BenchMeasurement> bench_original(const Project& project, const Unit& unit,
                                        std::size_t repeat, Lang lang) {
  auto exe = build_harness(project, unit, lang);
  if (!exe) return std::unexpected(exe.error());
  const std::vector<Json> generated = calls(unit, 64);
  std::string text;
  for (const Json& call : generated) {
    text += dump(call);
    text += '\n';
  }
  const std::filesystem::path calls_path = project.dir / kMetaDir / "bench-calls.jsonl";
  {
    auto written = write_file(calls_path, text);
    if (!written) return std::unexpected(written.error());
  }
  const Run run = run_command(exe->string(),
                              {"--cases", calls_path.string(), "--bench",
                               std::to_string(std::max<std::size_t>(repeat, 1))},
                              project.dir);
  if (!run.ok) return std::unexpected("the harness bench failed: " + run.failure());
  std::string payload = run.out;
  {
    std::string_view leading = payload;
    while (!leading.empty() && std::isspace(static_cast<unsigned char>(leading.front()))) {
      leading.remove_prefix(1);
    }
    if (!leading.starts_with('{')) payload = run.err;
  }
  std::string last;
  for (const std::string_view line : split_lines(payload)) {
    std::string_view trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
      trimmed.remove_prefix(1);
    }
    if (trimmed.starts_with('{')) last = line;
  }
  Json doc;
  try {
    doc = parse(last);
  } catch (const JsonError& error) {
    return std::unexpected(std::string("harness bench output: ") + error.what());
  }
  const Json* nanos = doc.find("ns_per_call");
  if (nanos == nullptr || !nanos->is_number()) {
    return std::unexpected("the harness printed no ns_per_call");
  }
  return BenchMeasurement{nanos->as_number(), run.rss_kb};
}

}  // namespace cppan::langc
