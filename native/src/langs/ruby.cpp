// Ruby: top-level functions (and module functions) with arguments the wire can
// carry. The recorder runs the original with `ruby`, the shadow module calls the
// C++ port through Fiddle (no compiler needed in the Ruby process).
#include <algorithm>
#include <cctype>
#include <format>
#include <map>

#include "cppan/codegen.hpp"
#include "cppan/langs.hpp"
#include "cppan/shadow.hpp"

namespace cppan::langs::ruby {
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

std::string_view trim_start(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  return text;
}

std::string_view trim_end(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

bool is_constant(std::string_view name) {
  if (name.empty()) return false;
  const char ch = name.front();
  return std::isupper(static_cast<unsigned char>(ch)) || ch == '_';
}

bool starts_with(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }

std::optional<Json> ruby_literal(std::string_view text) {
  if (text == "nil") return Json::null();
  if (text == "true") return Json::boolean(true);
  if (text == "false") return Json::boolean(false);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    return Json::string(std::string(text.substr(1, text.size() - 2)));
  }
  if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'') {
    return Json::string(std::string(text.substr(1, text.size() - 2)));
  }
  try {
    std::size_t used = 0;
    const std::string value(text);
    const std::int64_t number = std::stoll(value, &used);
    if (used == value.size()) return Json::integer(number);
  } catch (const std::exception&) {
  }
  try {
    std::size_t used = 0;
    const std::string value(text);
    const double number = std::stod(value, &used);
    if (used == value.size()) return Json::real(number);
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

struct DefResult {
  bool is_self = false;
  std::string name;
  std::vector<std::string> params;
  std::vector<std::optional<Json>> defaults;
};

Result<DefResult> parse_def(std::string_view rest) {
  rest = trim_end(rest);
  std::string_view head = rest;
  std::optional<std::string_view> tail;
  if (const std::size_t open = rest.find('('); open != std::string_view::npos) {
    head = trim(rest.substr(0, open));
    tail = rest.substr(open + 1);
  } else {
    head = rest;
  }
  const bool is_self = starts_with(head, "self.");
  std::string name(trim(starts_with(head, "self.") ? head.substr(5) : head));
  if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) ||
                        name.front() == '_')) {
    return std::unexpected("\"" + name + "\" is not a plain method name");
  }
  if (name.ends_with('=')) {
    return std::unexpected("setters return the argument, not a value");
  }
  DefResult out;
  out.is_self = is_self;
  out.name = name;
  if (!tail.has_value()) return out;

  std::string_view params_text;
  const std::size_t close = tail->rfind(')');
  if (close == std::string_view::npos) {
    return std::unexpected("an unclosed parameter list");
  }
  params_text = tail->substr(0, close);
  const std::string_view after = trim(tail->substr(close + 1));
  if (!after.empty() && !after.starts_with('#')) {
    return std::unexpected("`" + std::string(after) + "` after the parameter list");
  }
  params_text = trim(params_text);
  if (params_text.empty()) return out;

  for (std::string_view part : split_top_level(params_text, ',')) {
    part = trim(part);
    if (part.empty()) continue;
    if (part.starts_with('*') || part.starts_with('&')) {
      return std::unexpected("splat, keyword and block parameters have no wire form");
    }
    if (part.find(':') != std::string_view::npos &&
        part.find("=>") == std::string_view::npos) {
      return std::unexpected("keyword parameters have no wire form");
    }
    std::string param_name;
    std::optional<Json> default_value;
    if (const std::size_t equals = part.find('='); equals != std::string_view::npos) {
      param_name = std::string(trim(part.substr(0, equals)));
      const std::string_view literal = trim(part.substr(equals + 1));
      default_value = ruby_literal(literal);
      if (!default_value.has_value()) {
        return std::unexpected("the default `" + std::string(literal) +
                               "` is not a literal the wire carries");
      }
    } else {
      param_name = std::string(part);
    }
    if (param_name.empty() || !(std::isalpha(static_cast<unsigned char>(param_name.front())) ||
                               param_name.front() == '_')) {
      return std::unexpected("\"" + param_name + "\" is not a plain parameter name");
    }
    out.params.push_back(param_name);
    out.defaults.push_back(std::move(default_value));
  }
  return out;
}

std::string doc_comment(const std::string& source, std::size_t index) {
  const auto lines = split_lines(source);
  std::vector<std::string> out;
  std::size_t at = index;
  while (at > 0) {
    --at;
    const std::string_view line = trim(lines[at]);
    if (!line.starts_with('#')) break;
    const std::string_view text = trim(line.substr(1));
    if (starts_with(text, " frozen_string_literal")) break;
    out.emplace_back(text);
  }
  std::ranges::reverse(out);
  while (!out.empty() && out.front().empty()) out.erase(out.begin());
  std::string joined;
  for (const std::string& line : out) {
    if (!joined.empty()) joined += ' ';
    joined += line;
  }
  return joined;
}

}  // namespace

Result<Analysis> analyze(const std::filesystem::path& path) {
  const auto source = read_file(path);
  if (!source.has_value()) return std::unexpected(path.string() + ": cannot read");
  Analysis analysis;
  analysis.lines = static_cast<std::uint32_t>(line_count(*source));

  struct Container {
    std::size_t indent;
    std::string name;
    bool module_function;
  };
  std::vector<Container> containers;
  const auto lines = split_lines(*source);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    const std::string_view trimmed = trim_start(line);
    const std::size_t indent = line.size() - trimmed.size();
    if (trimmed.starts_with('#') || trimmed.empty()) continue;
    if (trimmed.starts_with("require_relative ") || trimmed.starts_with("require ")) {
      std::string_view rest = trimmed.starts_with("require_relative ")
                                  ? trimmed.substr(17)
                                  : trimmed.substr(8);
      rest = trim(rest);
      while (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
        rest.remove_prefix(1);
      }
      while (!rest.empty() && (rest.back() == '"' || rest.back() == '\'')) {
        rest.remove_suffix(1);
      }
      analysis.imports.emplace_back(rest);
      continue;
    }
    // Leaving blocks: anything at this indent closes containers deeper than it.
    while (!containers.empty() && containers.back().indent >= indent) containers.pop_back();
    if (trimmed.starts_with("module ")) {
      std::string_view rest = trim(trimmed.substr(7));
      if (rest.ends_with(';')) rest.remove_suffix(1);
      rest = trim(rest);
      if (is_constant(rest)) containers.push_back({indent, std::string(rest), false});
      continue;
    }
    if (trimmed.starts_with("class ")) {
      std::string_view rest = trim(trimmed.substr(6));
      if (rest.ends_with(';')) rest.remove_suffix(1);
      rest = trim(rest);
      containers.push_back({indent, "class:" + std::string(rest), false});
      continue;
    }
    if (trimmed == "module_function") {
      if (!containers.empty()) containers.back().module_function = true;
      continue;
    }
    if (!trimmed.starts_with("def ")) continue;

    const std::string_view rest = trimmed.substr(4);
    auto parsed = parse_def(rest);
    if (!parsed) {
      std::string skipped_name(rest);
      const std::size_t stop = skipped_name.find_first_of("( ");
      if (stop != std::string::npos) skipped_name.resize(stop);
      analysis.skipped.emplace_back(skipped_name, parsed.error());
      continue;
    }
    const DefResult& def = *parsed;
    std::string call;
    if (containers.empty()) {
      if (def.is_self) {
        analysis.skipped.emplace_back(def.name, "a singleton method outside a module");
        continue;
      }
      call = "TOPLEVEL";
    } else {
      const Container& container = containers.back();
      if (container.name.starts_with("class:")) {
        analysis.skipped.emplace_back(
            def.name, "a class method: the recorder does not construct an instance");
        continue;
      }
      if (!def.is_self && !container.module_function) {
        analysis.skipped.emplace_back(
            def.name,
            "an instance method of a module: define it as `def self.` or use `module_function`");
        continue;
      }
      call = container.name;
    }

    Found found;
    found.name = def.name;
    found.line = static_cast<std::uint32_t>(index + 1);
    found.call = call;
    for (std::size_t param_index = 0; param_index < def.params.size(); ++param_index) {
      FoundParam param;
      param.name = def.params[param_index];
      param.ty = "Any";
      if (param_index < def.defaults.size()) {
        param.has_default = def.defaults[param_index].has_value();
        param.default_value = def.defaults[param_index];
      }
      found.params.push_back(std::move(param));
    }
    found.returns = "Any";
    found.doc = doc_comment(*source, index);
    analysis.functions.push_back(std::move(found));
  }
  return analysis;
}

namespace {

std::string unit_call_from_project(const Project& project, std::string_view name) {
  const auto analysis = analyze(project.source_file);
  if (!analysis) return "TOPLEVEL";
  for (const Found& found : analysis->functions) {
    if (found.name == name) return found.call;
  }
  return "TOPLEVEL";
}

std::string pascal(std::string_view ns) {
  std::string out;
  std::size_t start = 0;
  while (start <= ns.size()) {
    const std::size_t underscore = ns.find('_', start);
    const std::string_view part = underscore == std::string_view::npos
                                      ? ns.substr(start)
                                      : ns.substr(start, underscore - start);
    if (!part.empty()) {
      out += static_cast<char>(std::toupper(static_cast<unsigned char>(part.front())));
      out += part.substr(1);
    }
    if (underscore == std::string_view::npos) break;
    start = underscore + 1;
  }
  return out;
}

std::string replace_all(std::string text, std::string_view needle, std::string_view value) {
  std::size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    text.replace(at, needle.size(), value);
    at += value.size();
  }
  return text;
}

std::string debug_string(std::string_view text) { return quote_debug(text); }

std::vector<std::pair<std::string, std::string>> magnus_files(const Project& project,
                                                              std::string_view ns) {
  const std::string build_dir = (project.dir / "build").string();
  const std::string lib_name = std::string(ns) + "_shadow_ext";
  const std::string cargo = std::format(
      "[package]\nname = \"{}\"\nversion = \"0.1.0\"\nedition = \"2021\"\npublish = false\n\n[lib]\ncrate-type = [\"cdylib\"]\nname = \"{}\"\n\n[dependencies]\nmagnus = \"0.9\"\n",
      lib_name, lib_name);
  const std::string build = std::format(
      "fn main() {{\n    println!(\"cargo:rustc-link-search=native={}\");\n    "
      "println!(\"cargo:rustc-link-lib=dylib=cppan_shadow\");\n    "
      "println!(\"cargo:rustc-link-arg=-Wl,-rpath,{}\");\n}}\n",
      build_dir, build_dir);
  constexpr std::string_view lib = R"CPPAN(use magnus::{function, prelude::*, Error, RString};

unsafe extern "C" {
    fn cppan_shadow_call(name: *const std::ffi::c_char, args: *const std::ffi::c_char) -> *mut std::ffi::c_char;
    fn cppan_shadow_free(value: *mut std::ffi::c_char);
}

fn call(name: RString, args: RString) -> Result<String, Error> {
    let name = std::ffi::CString::new(name.to_string()?)
        .map_err(|error| Error::new(magnus::exception::runtime_error(), error.to_string()))?;
    let args = std::ffi::CString::new(args.to_string()?)
        .map_err(|error| Error::new(magnus::exception::runtime_error(), error.to_string()))?;
    // SAFETY: both pointers are NUL-terminated and live for the call; the result is
    // the C ABI's allocation and is freed exactly once.
    unsafe {
        let out = cppan_shadow_call(name.as_ptr(), args.as_ptr());
        if out.is_null() {
            return Err(Error::new(magnus::exception::runtime_error(), "no result"));
        }
        let text = std::ffi::CStr::from_ptr(out).to_string_lossy().into_owned();
        cppan_shadow_free(out);
        Ok(text)
    }
}

#[magnus::init]
fn init() -> Result<(), Error> {
    let module = magnus::define_module("CppanShadowNative")?;
    module.define_singleton_method("call", function!(call, 2))?;
    Ok(())
}
)CPPAN";
  const std::string script = std::format(
      "#!/bin/sh\n# Build the magnus transport for {}; needs cargo and Ruby headers (ruby-dev).\n"
      "set -eu\ncd \"$(dirname \"$0\")\"\ncargo build --release\ncp "
      "\"target/release/lib{}.so\" \"../{}.so\"\necho \"built ../{}.so: the Ruby shadow now uses "
      "it instead of Fiddle\"\n",
      ns, lib_name, lib_name, lib_name);
  std::vector<std::pair<std::string, std::string>> files;
  files.emplace_back("shadow/magnus/Cargo.toml", cargo);
  files.emplace_back("shadow/magnus/build.rs", build);
  files.emplace_back("shadow/magnus/src/lib.rs", std::string(lib));
  files.emplace_back("shadow/magnus/build.sh", script);
  files.emplace_back("shadow/magnus/README.md",
                     std::format("# magnus transport for `{}`\n\n`./build.sh` builds the Ruby native "
                                 "extension (`magnus`) over the same\nC ABI the Fiddle path uses and "
                                 "copies it next to the shadow wrapper; the\nwrapper prefers it when "
                                 "`{}_shadow_ext.so` is present. It needs cargo and a Ruby\nwith "
                                 "headers (`ruby-dev` / `xcode-select --install`). Without them, the "
                                 "wrapper\nkeeps working through Fiddle.\n",
                                 ns, ns));
  return files;
}

}  // namespace

Result<std::size_t> record(const Project& project, const Unit& unit,
                           const std::vector<Json>& calls) {
  const std::filesystem::path calls_path = project.dir / "build" / "record-calls.jsonl";
  {
    auto written = write_file(calls_path, cases_json(calls));
    if (!written) return std::unexpected(written.error());
  }
  std::string mapping;
  for (std::size_t index = 0; index < unit.functions.size(); ++index) {
    const Function& function = unit.functions[index];
    if (index > 0) mapping += ",\n";
    mapping += "  " + debug_string(function.name) + " => " +
               debug_string(unit_call_from_project(project, function.name));
  }
  std::string harness = R"CPPAN(# Generated by cpp-anything record; runs the original Ruby and prints one JSON per case.
require 'json'

CALLS = {
@CALLS@
}
SOURCE = @SOURCE@
LOG = []

load SOURCE

def cppan_encode(value)
  case value
  when nil, true, false, String, Integer
    value
  when Float
    raise RangeError, 'non-finite float' unless value.finite?
    value
  when Array
    value.map { |item| cppan_encode(item) }
  when Hash
    raise TypeError, 'hash keys must be strings' unless value.keys.all? { |key| key.is_a?(String) }
    value.to_h { |key, item| [key, cppan_encode(item)] }
  else
    { '$opaque' => value.class.name.to_s }
  end
end

def cppan_receiver(call)
  return Object.new if call == 'TOPLEVEL'
  call.split('::').inject(Object) { |scope, name| scope.const_get(name) }
end

File.readlines(ARGV[0]).each do |line|
  next if line.strip.empty?
  call = JSON.parse(line)
  fn = call['fn']
  path = CALLS[fn]
  begin
    raise NameError, "no call path for #{fn}" if path.nil?
    value = cppan_receiver(path).send(fn, *call['args'])
    puts JSON.generate({ 'id' => call['id'], 'ok' => cppan_encode(value) })
  rescue Exception => error
    puts JSON.generate({ 'id' => call['id'], 'err' => { 'type' => error.class.name.split('::').last, 'msg' => error.message.to_s[0, 200] } })
  end
end
)CPPAN";
  harness = replace_all(std::move(harness), "@CALLS@", mapping);
  harness = replace_all(std::move(harness), "@SOURCE@", debug_string(project.source_file.string()));
  const std::filesystem::path harness_path = project.dir / "build" / "record.rb";
  {
    auto written = write_file(harness_path, harness);
    if (!written) return std::unexpected(written.error());
  }
  auto output = run_harness(project, "ruby",
                            {harness_path.string(), calls_path.string()}, std::nullopt);
  if (!output) return std::unexpected(output.error());
  return write_cases(project, calls, *output);
}

Result<double> bench(const Project& project, std::size_t repeat) {
  const std::filesystem::path cases_path = project.dir / "cases.jsonl";
  const std::filesystem::path calls_path = project.dir / "build" / "bench-calls.jsonl";
  const auto text = read_file(cases_path);
  if (!text.has_value()) return std::unexpected(cases_path.string() + ": cannot read");
  std::map<std::string, Json> first;
  for (const std::string_view line : split_lines(*text)) {
    bool empty = true;
    for (const char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) empty = false;
    }
    if (empty) continue;
    Json doc;
    try {
      doc = cppan::parse(line);
    } catch (const JsonError& error) {
      return std::unexpected(error.what());
    }
    const Json* fn = doc.find("fn");
    if (fn != nullptr && fn->is_string()) first.try_emplace(fn->as_string(), doc);
  }
  std::vector<Json> calls;
  for (const auto& [function, doc] : first) {
    Json args = Json::array();
    if (const Json* value = doc.find("args")) args = *value;
    calls.push_back(Json::object({
        {"id", Json::string(function)},
        {"fn", Json::string(function)},
        {"call", Json::string(unit_call_from_project(project, function))},
        {"args", args},
    }));
  }
  {
    auto written = write_file(calls_path, cases_json(calls));
    if (!written) return std::unexpected(written.error());
  }
  std::string harness = R"CPPAN(require 'json'
CALLS = JSON.parse(File.read(@CALLS@))
SOURCE = @SOURCE@
REPEAT = @REPEAT@
load SOURCE
def cppan_receiver(path)
  return Object.new if path == 'TOPLEVEL'
  path.split('::').inject(Object) { |scope, name| scope.const_get(name) }
end
CALLS.each { |call| cppan_receiver(call['call']).send(call['fn'], *call['args']) }
started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
CALLS.each do |call|
  receiver = cppan_receiver(call['call'])
  REPEAT.times { receiver.send(call['fn'], *call['args']) }
end
finished = Process.clock_gettime(Process::CLOCK_MONOTONIC)
puts JSON.generate({ 'ns' => ((finished - started) * 1_000_000_000).to_i, 'calls' => CALLS.length * REPEAT })
)CPPAN";
  harness = replace_all(std::move(harness), "@CALLS@", debug_string(calls_path.string()));
  harness = replace_all(std::move(harness), "@SOURCE@",
                        debug_string(project.source_file.string()));
  harness = replace_all(std::move(harness), "@REPEAT@", std::to_string(repeat));
  const std::filesystem::path harness_path = project.dir / "build" / "bench.rb";
  {
    auto written = write_file(harness_path, harness);
    if (!written) return std::unexpected(written.error());
  }
  auto output = run_harness(project, "ruby", {harness_path.string()}, std::nullopt);
  if (!output) return std::unexpected(output.error());
  Json doc;
  try {
    doc = cppan::parse(*output);
  } catch (const JsonError& error) {
    return std::unexpected(error.what());
  }
  const Json* nanos = doc.find("ns");
  const Json* calls_field = doc.find("calls");
  const double ns = nanos != nullptr && nanos->is_number() ? nanos->as_number() : 0.0;
  const double total = calls_field != nullptr && calls_field->is_number()
                           ? calls_field->as_number()
                           : 1.0;
  if (total <= 0.0) return std::unexpected("the Ruby benchmark ran no calls");
  return ns / total;
}

std::vector<std::pair<std::string, std::string>> shadow_files(const Project& project,
                                                              std::string_view ns) {
  const std::string library = codegen::shadow_library(project).string();
  const std::string log = shadow::log_path(project).string();
  std::string calls;
  for (std::size_t index = 0; index < project.functions.size(); ++index) {
    const Function& function = project.functions[index];
    if (index > 0) calls += ",\n";
    calls += "  " + debug_string(function.name) + " => " +
             debug_string(unit_call_from_project(project, function.name));
  }
  std::string methods;
  for (const Function& function : project.functions) {
    if (!methods.empty()) methods += '\n';
    methods += "  def self." + function.name + "(*args)\n    cppan_shadow(" +
               debug_string(function.name) + ", args) { cppan_original(" +
               debug_string(function.name) + ", args) }\n  end\n";
  }
  const std::string module = pascal(ns);
  std::string text = R"CPPAN(# Generated by cpp-anything: the original Ruby runs, the C++ port runs second,
# and the caller always sees the original's result. Disagreements go to the log.
require 'fiddle'
require 'json'
require 'time'

module @MODULE@Shadow
  PROJECT = @PROJECT@
  LIBRARY = @LIBRARY@
  LOG = ENV['CPPAN_SHADOW_LOG'] || @LOG@
  TOLERANCE = @TOLERANCE@
  CALLS = {
@CALLS@
  }
  SOURCE = @SOURCE@

  load SOURCE

  def self.cppan_receiver(path)
    return Object.new if path == 'TOPLEVEL'
    path.split('::').inject(Object) { |scope, name| scope.const_get(name) }
  end

  def self.cppan_original(fn, args)
    cppan_receiver(CALLS.fetch(fn)).send(fn, *args)
  end

  def self.cppan_encode(value)
    case value
    when nil, true, false, String, Integer
      value
    when Float
      value.finite? ? value : { '$opaque' => 'Float' }
    when Array
      value.map { |item| cppan_encode(item) }
    when Hash
      if value.keys.all? { |key| key.is_a?(String) }
        value.to_h { |key, item| [key, cppan_encode(item)] }
      else
        { '$opaque' => 'Hash' }
      end
    else
      { '$opaque' => value.class.name.to_s }
    end
  end

  # The magnus extension, when built, is the transport; Fiddle is the zero-build
  # fallback on the same C ABI.
  begin
    require_relative '@NS@_shadow_ext'
  rescue LoadError
    nil
  end

  if defined?(CppanShadowNative)
    def self.cppan_native(fn, args)
      JSON.parse(CppanShadowNative.call(fn, JSON.generate(args)))
    end
  else
    LIB = Fiddle.dlopen(LIBRARY)
    CALL = Fiddle::Function.new(LIB['cppan_shadow_call'], [Fiddle::TYPE_VOIDP, Fiddle::TYPE_VOIDP], Fiddle::TYPE_VOIDP)
    FREE = Fiddle::Function.new(LIB['cppan_shadow_free'], [Fiddle::TYPE_VOIDP], Fiddle::TYPE_VOID)

    def self.cppan_native(fn, args)
      pointer = CALL.call(fn, JSON.generate(args))
      return { 'err' => { 'type' => 'ShadowError', 'msg' => 'no result' } } if pointer.null?
      text = pointer.to_s
      FREE.call(pointer)
      JSON.parse(text)
    end
  end

  def self.cppan_compare(expected, got)
    expected_err = expected['err']
    got_err = got['err']
    if expected_err || got_err
      return nil if expected_err && got_err && expected_err['type'] == got_err['type']
      return 'the original returns a value, the port raises' if expected_err.nil?
      return 'the original raises, the port returns a value' if got_err.nil?
      return "error type: expected #{expected_err['type']}, got #{got_err['type']}"
    end
    left = expected.key?('ok') ? expected['ok'] : expected
    right = got.key?('ok') ? got['ok'] : got
    return nil if left == right
    'values differ'
  end

  def self.cppan_log(fn, args, expected, got, reason)
    File.open(LOG, 'a') do |file|
      file.puts(JSON.generate({
        'at' => Time.now.utc.iso8601,
        'fn' => fn,
        'args' => args,
        'expected' => expected,
        'got' => got,
        'reason' => reason
      }))
    end
  rescue StandardError
    nil
  end

  def self.cppan_shadow(fn, args)
    value = yield
    return value if ENV['CPPAN_SHADOW'] == 'off'
    expected = { 'ok' => cppan_encode(value) }
    begin
      got = cppan_native(fn, args)
    rescue StandardError => error
      got = { 'err' => { 'type' => error.class.name, 'msg' => error.message.to_s } }
    end
    reason = cppan_compare(expected, got)
    cppan_log(fn, args, expected, got, reason) if reason
    value
  rescue StandardError
    raise
  end

@METHODS@
end
)CPPAN";
  text = replace_all(std::move(text), "@MODULE@", module);
  text = replace_all(std::move(text), "@NS@", ns);
  text = replace_all(std::move(text), "@PROJECT@", debug_string(project.dir.string()));
  text = replace_all(std::move(text), "@LIBRARY@", debug_string(library));
  text = replace_all(std::move(text), "@LOG@", debug_string(log));
  text = replace_all(std::move(text), "@TOLERANCE@", cppan::format_real(project.tolerance));
  text = replace_all(std::move(text), "@CALLS@", calls);
  text = replace_all(std::move(text), "@SOURCE@", debug_string(project.source_file.string()));
  text = replace_all(std::move(text), "@METHODS@", methods);
  std::vector<std::pair<std::string, std::string>> files;
  files.emplace_back("shadow/" + std::string(ns) + "_shadow.rb", text);
  for (auto& file : magnus_files(project, ns)) files.push_back(std::move(file));
  return files;
}

}  // namespace cppan::langs::ruby
