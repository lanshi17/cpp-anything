//! Ruby: top-level functions (and module functions) with arguments the wire can
//! carry. The recorder runs the original with `ruby`, the shadow module calls the
//! C++ port through Fiddle (no compiler needed in the Ruby process).

use std::path::Path;

use crate::json::Json;
use crate::project::Project;
use crate::util;

use super::{Analysis, Found, FoundParam, RecordOptions};

pub fn analyze(path: &Path) -> Result<Analysis, String> {
    let source = util::read(path)?;
    let lines = source.lines().count() as u32;
    let mut functions = Vec::new();
    let mut skipped = Vec::new();
    let mut imports = Vec::new();
    let mut containers: Vec<(usize, String, bool)> = Vec::new(); // (indent, name, module_function)

    for (index, line) in source.lines().enumerate() {
        let trimmed = line.trim_start();
        let indent = line.len() - trimmed.len();
        if trimmed.starts_with('#') || trimmed.is_empty() {
            continue;
        }
        if let Some(rest) = trimmed
            .strip_prefix("require_relative ")
            .or_else(|| trimmed.strip_prefix("require "))
        {
            imports.push(
                rest.trim()
                    .trim_matches(|c| c == '"' || c == '\'')
                    .to_string(),
            );
            continue;
        }
        // Leaving blocks: anything at this indent closes containers deeper than it.
        while containers
            .last()
            .is_some_and(|(depth, _, _)| *depth >= indent)
        {
            containers.pop();
        }
        if let Some(rest) = trimmed.strip_prefix("module ") {
            let name = rest.trim().trim_end_matches(';').trim();
            if is_constant(name) {
                containers.push((indent, name.to_string(), false));
            }
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix("class ") {
            let name = rest.trim().trim_end_matches(';').trim();
            containers.push((indent, format!("class:{name}"), false));
            continue;
        }
        if trimmed == "module_function" {
            if let Some(last) = containers.last_mut() {
                last.2 = true;
            }
            continue;
        }
        let Some(rest) = trimmed.strip_prefix("def ") else {
            continue;
        };
        let (is_self, name, params, defaults) = match parse_def(rest) {
            Ok(parsed) => parsed,
            Err(reason) => {
                skipped.push((
                    rest.split(['(', ' ']).next().unwrap_or("def").to_string(),
                    reason,
                ));
                continue;
            }
        };
        let container = containers.last().cloned();
        let call = match (&container, is_self) {
            (None, false) => "TOPLEVEL".to_string(),
            (None, true) => {
                skipped.push((name, "a singleton method outside a module".into()));
                continue;
            }
            (Some((_, container_name, module_function)), self_method) => {
                if container_name.starts_with("class:") {
                    skipped.push((
                        name,
                        "a class method: the recorder does not construct an instance".into(),
                    ));
                    continue;
                }
                if !self_method && !*module_function {
                    skipped.push((
                        name,
                        "an instance method of a module: define it as `def self.` or use `module_function`".into(),
                    ));
                    continue;
                }
                container_name.clone()
            }
        };
        let doc = doc_comment(&source, index);
        functions.push(Found {
            name,
            line: index as u32 + 1,
            call,
            params: params
                .into_iter()
                .map(|param| FoundParam {
                    name: param,
                    ty: "Any".into(),
                    has_default: false,
                    default: None,
                })
                .collect::<Vec<_>>()
                .into_iter()
                .zip(defaults)
                .map(|(mut param, default)| {
                    param.has_default = default.is_some();
                    param.default = default;
                    param
                })
                .collect(),
            returns: "Any".into(),
            doc,
        });
    }
    Ok(Analysis {
        functions,
        skipped,
        imports,
        lines,
    })
}

fn is_constant(name: &str) -> bool {
    name.chars()
        .next()
        .is_some_and(|ch| ch.is_ascii_uppercase() || ch == '_')
}

/// Splits `def name(a, b = 1)` into names and default values. Anything Ruby can
/// express but the wire cannot (`*args`, keywords, blocks) is refused with a reason.
fn parse_def(rest: &str) -> Result<(bool, String, Vec<String>, Vec<Option<Json>>), String> {
    let rest = rest.trim_end();
    let (head, tail) = match rest.split_once('(') {
        Some((head, tail)) => (head.trim(), Some(tail)),
        None => (rest, None),
    };
    let self_method = head.starts_with("self.");
    let name = head.trim_start_matches("self.").trim().to_string();
    if name.is_empty()
        || !name.chars().next().unwrap().is_ascii_alphabetic()
            && name.chars().next().unwrap() != '_'
    {
        return Err(format!("{name:?} is not a plain method name"));
    }
    if name.ends_with('=') {
        return Err("setters return the argument, not a value".into());
    }
    let Some(tail) = tail else {
        return Ok((self_method, name, Vec::new(), Vec::new()));
    };
    let params_text = match tail.rsplit_once(')') {
        Some((params, after)) => {
            let after = after.trim();
            if !after.is_empty() && !after.starts_with('#') {
                return Err(format!("`{after}` after the parameter list"));
            }
            params
        }
        None => return Err("an unclosed parameter list".into()),
    };
    let params_text = params_text.trim();
    if params_text.is_empty() {
        return Ok((self_method, name, Vec::new(), Vec::new()));
    }
    let mut names = Vec::new();
    let mut defaults = Vec::new();
    for part in crate::types::split_top_level(params_text, ',') {
        let part = part.trim();
        if part.is_empty() {
            continue;
        }
        if part.starts_with('*') || part.starts_with('&') {
            return Err("splat, keyword and block parameters have no wire form".into());
        }
        if part.contains(':') && !part.contains("=>") {
            return Err("keyword parameters have no wire form".into());
        }
        let (name, default) = match part.split_once('=') {
            Some((name, value)) => (
                name.trim().to_string(),
                Some(ruby_literal(value.trim()).ok_or_else(|| {
                    format!(
                        "the default `{}` is not a literal the wire carries",
                        value.trim()
                    )
                })?),
            ),
            None => (part.to_string(), None),
        };
        if !name
            .chars()
            .next()
            .is_some_and(|ch| ch.is_ascii_alphabetic() || ch == '_')
        {
            return Err(format!("{name:?} is not a plain parameter name"));
        }
        names.push(name);
        defaults.push(default);
    }
    Ok((self_method, name, names, defaults))
}

fn ruby_literal(text: &str) -> Option<Json> {
    match text {
        "nil" => Some(Json::Null),
        "true" => Some(Json::Bool(true)),
        "false" => Some(Json::Bool(false)),
        _ => {
            if let Some(text) = text.strip_prefix('"').and_then(|t| t.strip_suffix('"')) {
                return Some(Json::str(text));
            }
            if let Some(text) = text.strip_prefix('\'').and_then(|t| t.strip_suffix('\'')) {
                return Some(Json::str(text));
            }
            if let Ok(value) = text.parse::<i64>() {
                return Some(Json::Int(value));
            }
            text.parse::<f64>().ok().map(Json::Real)
        }
    }
}

fn doc_comment(source: &str, index: usize) -> String {
    let lines: Vec<&str> = source.lines().collect();
    let mut out = Vec::new();
    let mut at = index;
    while at > 0 {
        at -= 1;
        let line = lines[at].trim();
        let Some(text) = line.strip_prefix('#') else {
            break;
        };
        if text.starts_with(" frozen_string_literal") {
            break;
        }
        out.push(text.trim().to_string());
    }
    out.reverse();
    while out.first().is_some_and(|line| line.is_empty()) {
        out.remove(0);
    }
    out.join(" ")
}

pub fn record(
    project: &Project,
    unit: &crate::inventory::Unit,
    calls: &[Json],
    _options: &RecordOptions,
) -> Result<usize, String> {
    let calls_path = project.dir.join("build").join("record-calls.jsonl");
    util::write(&calls_path, &super::cases_json(calls))?;
    let original = project.source_file.display().to_string();
    let harness = render_harness(unit, &original);
    let harness_path = project.dir.join("build").join("record.rb");
    util::write(&harness_path, &harness)?;
    let output = super::run_harness(
        project,
        "ruby",
        &[
            harness_path.display().to_string(),
            calls_path.display().to_string(),
        ],
        None,
    )?;
    super::write_cases(project, calls, &output)
}

fn render_harness(unit: &crate::inventory::Unit, original: &str) -> String {
    let calls = unit
        .functions
        .iter()
        .map(|function| {
            let call = unit_call(unit, &function.name);
            format!("  {:?} => {:?}", function.name, call)
        })
        .collect::<Vec<_>>()
        .join(",\n");
    format!(
        r#"# Generated by cpp-anything record; runs the original Ruby and prints one JSON per case.
require 'json'

CALLS = {{
{calls}
}}
SOURCE = {original:?}
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
    value.map {{ |item| cppan_encode(item) }}
  when Hash
    raise TypeError, 'hash keys must be strings' unless value.keys.all? {{ |key| key.is_a?(String) }}
    value.to_h {{ |key, item| [key, cppan_encode(item)] }}
  else
    {{ '$opaque' => value.class.name.to_s }}
  end
end

def cppan_receiver(call)
  return Object.new if call == 'TOPLEVEL'
  call.split('::').inject(Object) {{ |scope, name| scope.const_get(name) }}
end

File.readlines(ARGV[0]).each do |line|
  next if line.strip.empty?
  call = JSON.parse(line)
  fn = call['fn']
  path = CALLS[fn]
  begin
    raise NameError, "no call path for #{{fn}}" if path.nil?
    value = cppan_receiver(path).send(fn, *call['args'])
    puts JSON.generate({{ 'id' => call['id'], 'ok' => cppan_encode(value) }})
  rescue Exception => error
    puts JSON.generate({{ 'id' => call['id'], 'err' => {{ 'type' => error.class.name.split('::').last, 'msg' => error.message.to_s[0, 200] }} }})
  end
end
"#,
        calls = calls,
        original = original
    )
}

/// The analyzer's call path is not part of `Function`; re-read the source so the
/// harness can call module functions as `M.fn` and top-level ones as `TOPLEVEL`.
fn unit_call(unit: &crate::inventory::Unit, name: &str) -> String {
    let Ok(analysis) = analyze(&unit.file) else {
        return "TOPLEVEL".into();
    };
    analysis
        .functions
        .iter()
        .find(|found| found.name == name)
        .map(|found| found.call.clone())
        .unwrap_or_else(|| "TOPLEVEL".into())
}

pub fn bench(project: &Project, repeat: usize) -> Result<f64, String> {
    let cases_path = project.dir.join("cases.jsonl");
    let calls_path = project.dir.join("build").join("bench-calls.jsonl");
    let text = util::read(&cases_path)?;
    let mut first: std::collections::BTreeMap<String, Json> = std::collections::BTreeMap::new();
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)?;
        let function = doc
            .get("fn")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        first.entry(function).or_insert(doc);
    }
    let mut calls = Vec::new();
    for doc in first.values() {
        let function = doc
            .get("fn")
            .and_then(Json::as_str)
            .ok_or("a case without fn")?;
        calls.push(Json::obj([
            ("id", Json::str(function.to_string())),
            ("fn", Json::str(function.to_string())),
            ("call", Json::str(unit_call_from_project(project, function))),
            (
                "args",
                doc.get("args").cloned().unwrap_or(Json::Array(Vec::new())),
            ),
        ]));
    }
    util::write(&calls_path, &super::cases_json(&calls))?;
    let harness = format!(
        r#"require 'json'
CALLS = JSON.parse(File.read({calls:?}))
SOURCE = {source:?}
REPEAT = {repeat}
load SOURCE
def cppan_receiver(path)
  return Object.new if path == 'TOPLEVEL'
  path.split('::').inject(Object) {{ |scope, name| scope.const_get(name) }}
end
CALLS.each {{ |call| cppan_receiver(call['call']).send(call['fn'], *call['args']) }}
started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
CALLS.each do |call|
  receiver = cppan_receiver(call['call'])
  REPEAT.times {{ receiver.send(call['fn'], *call['args']) }}
end
finished = Process.clock_gettime(Process::CLOCK_MONOTONIC)
puts JSON.generate({{ 'ns' => ((finished - started) * 1_000_000_000).to_i, 'calls' => CALLS.length * REPEAT }})
"#,
        calls = calls_path.display().to_string(),
        source = project.source_file.display().to_string(),
        repeat = repeat
    );
    let harness_path = project.dir.join("build").join("bench.rb");
    util::write(&harness_path, &harness)?;
    let output = super::run_harness(project, "ruby", &[harness_path.display().to_string()], None)?;
    let doc = Json::parse(output.trim())?;
    let ns = doc.get("ns").and_then(Json::as_f64).unwrap_or(0.0);
    let calls = doc.get("calls").and_then(Json::as_f64).unwrap_or(1.0);
    if calls <= 0.0 {
        return Err("the Ruby benchmark ran no calls".into());
    }
    Ok(ns / calls)
}

pub fn shadow_files(project: &Project, ns: &str) -> Vec<(String, String)> {
    let library = crate::codegen::shadow_library(project);
    let log = crate::shadow::log_path(project);
    let calls = project
        .functions
        .iter()
        .map(|function| {
            format!(
                "  {:?} => {:?}",
                function.name,
                unit_call_from_project(project, &function.name)
            )
        })
        .collect::<Vec<_>>()
        .join(",\n");
    let methods = project
        .functions
        .iter()
        .map(|function| {
            format!(
                "  def self.{name}(*args)\n    cppan_shadow({name:?}, args) {{ cppan_original({name:?}, args) }}\n  end\n",
                name = function.name
            )
        })
        .collect::<Vec<_>>()
        .join("\n");
    let module = pascal(ns);
    let text = format!(
        r#"# Generated by cpp-anything: the original Ruby runs, the C++ port runs second,
# and the caller always sees the original's result. Disagreements go to the log.
require 'fiddle'
require 'json'
require 'time'

module {module}Shadow
  PROJECT = {project:?}
  LIBRARY = {library:?}
  LOG = ENV['CPPAN_SHADOW_LOG'] || {log:?}
  TOLERANCE = {tolerance:?}
  CALLS = {{
{calls}
  }}
  SOURCE = {source:?}

  load SOURCE

  def self.cppan_receiver(path)
    return Object.new if path == 'TOPLEVEL'
    path.split('::').inject(Object) {{ |scope, name| scope.const_get(name) }}
  end

  def self.cppan_original(fn, args)
    cppan_receiver(CALLS.fetch(fn)).send(fn, *args)
  end

  def self.cppan_encode(value)
    case value
    when nil, true, false, String, Integer
      value
    when Float
      value.finite? ? value : {{ '$opaque' => 'Float' }}
    when Array
      value.map {{ |item| cppan_encode(item) }}
    when Hash
      if value.keys.all? {{ |key| key.is_a?(String) }}
        value.to_h {{ |key, item| [key, cppan_encode(item)] }}
      else
        {{ '$opaque' => 'Hash' }}
      end
    else
      {{ '$opaque' => value.class.name.to_s }}
    end
  end

  # The magnus extension, when built, is the transport; Fiddle is the zero-build
  # fallback on the same C ABI.
  begin
    require_relative '{ns}_shadow_ext'
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
      return {{ 'err' => {{ 'type' => 'ShadowError', 'msg' => 'no result' }} }} if pointer.null?
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
      return "error type: expected #{{expected_err['type']}}, got #{{got_err['type']}}"
    end
    left = expected.key?('ok') ? expected['ok'] : expected
    right = got.key?('ok') ? got['ok'] : got
    return nil if left == right
    'values differ'
  end

  def self.cppan_log(fn, args, expected, got, reason)
    File.open(LOG, 'a') do |file|
      file.puts(JSON.generate({{
        'at' => Time.now.utc.iso8601,
        'fn' => fn,
        'args' => args,
        'expected' => expected,
        'got' => got,
        'reason' => reason
      }}))
    end
  rescue StandardError
    nil
  end

  def self.cppan_shadow(fn, args)
    value = yield
    return value if ENV['CPPAN_SHADOW'] == 'off'
    expected = {{ 'ok' => cppan_encode(value) }}
    begin
      got = cppan_native(fn, args)
    rescue StandardError => error
      got = {{ 'err' => {{ 'type' => error.class.name, 'msg' => error.message.to_s }} }}
    end
    reason = cppan_compare(expected, got)
    cppan_log(fn, args, expected, got, reason) if reason
    value
  rescue StandardError
    raise
  end

{methods}
end
"#,
        module = module,
        ns = ns,
        project = project.dir.display().to_string(),
        library = library.display().to_string(),
        log = log.display().to_string(),
        tolerance = project.tolerance,
        calls = calls,
        source = project.source_file.display().to_string(),
        methods = methods
    );
    let mut files = vec![(format!("shadow/{ns}_shadow.rb"), text)];
    files.extend(magnus_files(project, ns));
    files
}

/// The optional magnus extension: same C ABI, built as a Ruby native extension.
/// `shadow/magnus/build.sh` runs cargo and copies the .so next to the wrapper;
/// without it the wrapper uses Fiddle. Needs a Rust toolchain and Ruby headers.
fn magnus_files(project: &Project, ns: &str) -> Vec<(String, String)> {
    let build_dir = project.dir.join("build");
    let lib_name = format!("{ns}_shadow_ext");
    let cargo = format!(
        "[package]\nname = \"{lib_name}\"\nversion = \"0.1.0\"\nedition = \"2021\"\npublish = false\n\n[lib]\ncrate-type = [\"cdylib\"]\nname = \"{lib_name}\"\n\n[dependencies]\nmagnus = \"0.9\"\n"
    );
    let build = format!(
        "fn main() {{\n    println!(\"cargo:rustc-link-search=native={build}\");\n    println!(\"cargo:rustc-link-lib=dylib=cppan_shadow\");\n    println!(\"cargo:rustc-link-arg=-Wl,-rpath,{build}\");\n}}\n",
        build = build_dir.display()
    );
    let lib = r#"use magnus::{function, prelude::*, Error, RString};

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
"#;
    let script = format!(
        "#!/bin/sh\n# Build the magnus transport for {ns}; needs cargo and Ruby headers (ruby-dev).\nset -eu\ncd \"$(dirname \"$0\")\"\ncargo build --release\ncp \"target/release/lib{lib_name}.so\" \"../{lib_name}.so\"\necho \"built ../{lib_name}.so: the Ruby shadow now uses it instead of Fiddle\"\n"
    );
    vec![
        ("shadow/magnus/Cargo.toml".to_string(), cargo),
        ("shadow/magnus/build.rs".to_string(), build),
        ("shadow/magnus/src/lib.rs".to_string(), lib.to_string()),
        ("shadow/magnus/build.sh".to_string(), script),
        (
            "shadow/magnus/README.md".to_string(),
            format!(
                "# magnus transport for `{ns}`\n\n`./build.sh` builds the Ruby native extension (`magnus`) over the same\nC ABI the Fiddle path uses and copies it next to the shadow wrapper; the\nwrapper prefers it when `{lib_name}.so` is present. It needs cargo and a Ruby\nwith headers (`ruby-dev` / `xcode-select --install`). Without them, the wrapper\nkeeps working through Fiddle.\n"
            ),
        ),
    ]
}

fn unit_call_from_project(project: &Project, name: &str) -> String {
    let Ok(analysis) = analyze(&project.source_file) else {
        return "TOPLEVEL".into();
    };
    analysis
        .functions
        .iter()
        .find(|found| found.name == name)
        .map(|found| found.call.clone())
        .unwrap_or_else(|| "TOPLEVEL".into())
}

fn pascal(ns: &str) -> String {
    ns.split('_')
        .map(|part| {
            let mut chars = part.chars();
            match chars.next() {
                Some(first) => format!("{}{}", first.to_ascii_uppercase(), chars.as_str()),
                None => String::new(),
            }
        })
        .collect()
}
