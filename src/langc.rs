//! The C plugin: units, functions, a recording harness, and the corpora that make
//! a C port checkable.
//!
//! C is where a C++ port should be almost mechanical, so the harness is strict: it
//! links the original with renamed symbols, forks per case (a segfault is a case,
//! not a lost run), and writes the same wire format Python recordings use.

use std::path::{Path, PathBuf};

use crate::inventory::{Function, Param, Unit};
use crate::json::Json;
use crate::types::Ty;
use crate::util;

#[derive(Clone, Debug)]
pub struct CFunction {
    pub name: String,
    pub line: u32,
    pub ret: Ty,
    pub ret_c: String,
    pub params: Vec<(Ty, String)>,
    pub params_c: Vec<(String, String)>, // (C type, name)
}

pub struct Analysis {
    pub functions: Vec<CFunction>,
    pub skipped: Vec<(String, String)>,
    pub lines: u32,
}

/// A deliberately small C parser: top-level definitions with a supported signature.
/// Anything else is reported as skipped, not guessed.
pub fn analyze_file(path: &Path) -> Result<Analysis, String> {
    let source = util::read(path)?;
    let lines = source.lines().count() as u32;
    let stripped = strip_comments(&source);
    let mut functions = Vec::new();
    let mut skipped = Vec::new();
    let mut offset = 0usize;
    let bytes = stripped.as_bytes();
    while offset < bytes.len() {
        // Find a `name(` at the start of a line-ish position with a preceding type.
        let Some(open) = stripped[offset..].find('(') else {
            break;
        };
        let open = offset + open;
        let line_start = stripped[..open]
            .rfind('\n')
            .map(|index| index + 1)
            .unwrap_or(0);
        let head = stripped[line_start..open].trim();
        // The close paren must be followed by `{` (a definition, not a prototype).
        let Some(close_rel) = stripped[open..].find(')') else {
            break;
        };
        let close = open + close_rel;
        let after = stripped[close + 1..].trim_start();
        if !after.starts_with('{') {
            offset = close + 1;
            continue;
        }
        let (ret_c, name) = match split_head(head) {
            Some(parts) => parts,
            None => {
                offset = close + 1;
                continue;
            }
        };
        let params_text = stripped[open + 1..close].trim();
        match map_signature(&ret_c, params_text) {
            Ok((ret, params, params_c)) => functions.push(CFunction {
                name,
                line: stripped[..line_start].lines().count() as u32 + 1,
                ret,
                ret_c,
                params,
                params_c,
            }),
            Err(reason) => skipped.push((name, reason)),
        }
        // Skip the body: find the matching brace (strings are rare in signatures).
        let Some(brace) = stripped[close..].find('{') else {
            break;
        };
        let mut depth = 0i32;
        let mut index = close + brace;
        while index < bytes.len() {
            match bytes[index] {
                b'{' => depth += 1,
                b'}' => {
                    depth -= 1;
                    if depth == 0 {
                        break;
                    }
                }
                _ => {}
            }
            index += 1;
        }
        offset = index + 1;
    }
    Ok(Analysis {
        functions,
        skipped,
        lines,
    })
}

fn split_head(head: &str) -> Option<(String, String)> {
    let mut parts = head.rsplitn(2, char::is_whitespace);
    let name = parts.next()?.trim().to_string();
    let ret = parts.next()?.trim().to_string();
    if name.is_empty() || ret.is_empty() || name.contains('*') {
        return None;
    }
    if matches!(
        ret.as_str(),
        "if" | "for" | "while" | "switch" | "return" | "else" | "do"
    ) {
        return None;
    }
    Some((ret, name))
}

type Signature = (Ty, Vec<(Ty, String)>, Vec<(String, String)>);

fn map_signature(ret_c: &str, params_text: &str) -> Result<Signature, String> {
    let ret = map_ctype(ret_c)
        .ok_or_else(|| format!("return type {ret_c:?} has no supported mapping"))?;
    if params_text.is_empty() || params_text == "void" {
        return Ok((ret, Vec::new(), Vec::new()));
    }
    let mut params = Vec::new();
    let mut params_c = Vec::new();
    for (index, part) in params_text.split(',').enumerate() {
        let part = part.trim();
        if part == "..." {
            return Err("variadic function: the port would need a C++ overload set".into());
        }
        let (type_text, name) = split_param(part);
        let ty = map_ctype(&type_text)
            .ok_or_else(|| format!("parameter {part:?} has no supported mapping"))?;
        let name = name.unwrap_or_else(|| format!("arg{index}"));
        params.push((ty, name.clone()));
        params_c.push((type_text, name));
    }
    Ok((ret, params, params_c))
}

fn split_param(part: &str) -> (String, Option<String>) {
    let cleaned = part.replace('*', " * ");
    let words: Vec<&str> = cleaned.split_whitespace().collect();
    if words.is_empty() {
        return (String::new(), None);
    }
    let last = words[words.len() - 1];
    let is_name = last
        .chars()
        .next()
        .is_some_and(|ch| ch.is_ascii_alphabetic())
        && last
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || ch == '_')
        && !matches!(
            last,
            "int"
                | "char"
                | "double"
                | "float"
                | "void"
                | "long"
                | "short"
                | "bool"
                | "size_t"
                | "unsigned"
                | "signed"
                | "const"
                | "volatile"
                | "restrict"
        );
    if words.len() >= 2 && is_name {
        (words[..words.len() - 1].join(" "), Some(last.to_string()))
    } else {
        (words.join(" "), None)
    }
}

fn map_ctype(text: &str) -> Option<Ty> {
    let cleaned = text
        .replace("const", " ")
        .replace("volatile", " ")
        .replace("restrict", " ")
        .replace('*', " * ")
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ");
    match cleaned.trim() {
        "char *" | "signed char *" | "unsigned char *" => Some(Ty::Str),
        "int" | "long" | "long int" | "short" | "short int" | "signed" | "signed int" => {
            Some(Ty::Int)
        }
        "unsigned" | "unsigned int" | "unsigned long" | "unsigned long int" | "unsigned short"
        | "size_t" | "ssize_t" | "int64_t" | "uint64_t" | "int32_t" | "uint32_t" => Some(Ty::Int),
        "double" | "long double" => Some(Ty::Float),
        "float" => Some(Ty::Float),
        "bool" | "_Bool" => Some(Ty::Bool),
        "void" => Some(Ty::None),
        _ => None,
    }
}

fn strip_comments(source: &str) -> String {
    let mut out = String::with_capacity(source.len());
    let bytes = source.as_bytes();
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index..].starts_with(b"/*") {
            if let Some(end) = source[index + 2..].find("*/") {
                out.extend(std::iter::repeat_n(
                    '\n',
                    source[index..index + 2 + end].matches('\n').count(),
                ));
                index += 2 + end + 2;
                continue;
            }
            break;
        }
        if bytes[index..].starts_with(b"//") {
            while index < bytes.len() && bytes[index] != b'\n' {
                index += 1;
            }
            continue;
        }
        if bytes[index] == b'"' {
            out.push('"');
            index += 1;
            while index < bytes.len() {
                let ch = bytes[index];
                out.push(ch as char);
                index += 1;
                if ch == b'\\' && index < bytes.len() {
                    out.push(bytes[index] as char);
                    index += 1;
                } else if ch == b'"' {
                    break;
                }
            }
            continue;
        }
        out.push(bytes[index] as char);
        index += 1;
    }
    out
}

/// Turns a parsed C file into the unit the rest of the tool understands.
pub fn unit(repo: &Path, relative_or_path: &str) -> Result<Unit, String> {
    let file = if Path::new(relative_or_path).is_absolute() {
        PathBuf::from(relative_or_path)
    } else {
        repo.join(relative_or_path)
    }
    .canonicalize()
    .map_err(|error| format!("{relative_or_path}: {error}"))?;
    let analysis = analyze_file(&file)?;
    if analysis.functions.is_empty() {
        return Err(format!(
            "{} has no functions with a supported C signature ({} skipped)",
            file.display(),
            analysis.skipped.len()
        ));
    }
    let module = file
        .strip_prefix(repo.canonicalize().unwrap_or_else(|_| repo.to_path_buf()))
        .unwrap_or(&file)
        .display()
        .to_string();
    let functions = analysis
        .functions
        .iter()
        .map(to_function)
        .collect::<Vec<_>>();
    let risks = vec![
        "C has no exceptions: a failure is a return code or an errno, and the port must mirror it".to_string(),
        "char* strings: null versus empty is observable, and the recorder treats a null return as null".to_string(),
        "Fixed-width C types are not Python's unbounded ints: the signature decides the width".to_string(),
        "Undefined behaviour on inputs outside the contract is a case, not a failure (the harness records the signal)".to_string(),
    ];
    Ok(Unit {
        unit: file
            .file_stem()
            .map(|stem| stem.to_string_lossy().into_owned())
            .unwrap_or_else(|| "unit".into()),
        module,
        file,
        package_root: Some(repo.canonicalize().unwrap_or_else(|_| repo.to_path_buf())),
        lines: analysis.lines,
        functions,
        skipped: analysis.skipped,
        risks,
        imports: Vec::new(),
    })
}

fn to_function(function: &CFunction) -> Function {
    Function {
        name: function.name.clone(),
        line: function.line,
        params: function
            .params
            .iter()
            .map(|(ty, name)| Param {
                name: name.clone(),
                annotation: function
                    .params_c
                    .iter()
                    .find(|(_, candidate)| candidate == name)
                    .map(|(c_type, _)| c_type.clone())
                    .unwrap_or_default(),
                ty: ty.clone(),
                default: None,
                has_default: false,
                keyword_only: false,
            })
            .collect(),
        returns: function.ret.clone(),
        returns_annotation: function.ret_c.clone(),
        doc: String::new(),
        risks: vec!["C signature: ".to_string() + &describe(function)],
        raises: false,
    }
}

fn describe(function: &CFunction) -> String {
    let params = function
        .params_c
        .iter()
        .map(|(ty, name)| format!("{ty} {name}"))
        .collect::<Vec<_>>()
        .join(", ");
    format!("{} {}({params})", function.ret_c, function.name)
}

// ---------------------------------------------------------------------------
// corpora
// ---------------------------------------------------------------------------

const STRINGS: &[&str] = &[
    "",
    " ",
    "Hello World",
    "  Mixed CASE  ",
    "café — naïve",
    "a",
    "123",
    "-",
    "trailing---",
    " leading",
    "one\ntwo",
    "Москва",
    "0",
    "a-b_c",
    "x",
];
const INTS: &[i64] = &[
    0, 1, -1, 7, 42, 12, 100, -100, 10_000, 3, 19, 255, -7, 2_000_000, 8, 11,
];
const FLOATS: &[f64] = &[
    0.0, -0.0, 1.5, -2.25, 0.1, 2.0, -0.5, 1e-9, 123456.789, -1.0, 7.0, 1e6, 3.25, -99.5, 0.25,
];

fn values_for(ty: &Ty) -> Vec<Json> {
    match ty {
        Ty::Str => STRINGS.iter().map(|text| Json::str(*text)).collect(),
        Ty::Int => INTS.iter().map(|value| Json::Int(*value)).collect(),
        Ty::Float => FLOATS.iter().map(|value| Json::Real(*value)).collect(),
        Ty::Bool => vec![Json::Bool(true), Json::Bool(false)],
        Ty::None => vec![Json::Null],
        _ => vec![Json::Null],
    }
}

/// Calls with generated arguments, before the harness has run them.
pub fn calls(unit: &Unit, count: usize) -> Vec<Json> {
    let mut out = Vec::new();
    for function in &unit.functions {
        let corpora: Vec<Vec<Json>> = function
            .params
            .iter()
            .map(|param| values_for(&param.ty))
            .collect();
        if corpora.is_empty() {
            out.push(Json::obj([
                ("id", Json::str(format!("{}:0", function.name))),
                ("fn", Json::str(function.name.clone())),
                ("args", Json::Array(Vec::new())),
            ]));
            continue;
        }
        let total = corpora
            .iter()
            .map(Vec::len)
            .max()
            .unwrap_or(0)
            .min(count.max(1));
        for index in 0..total {
            let args = corpora
                .iter()
                .map(|values| values[index % values.len()].clone())
                .collect::<Vec<_>>();
            out.push(Json::obj([
                ("id", Json::str(format!("{}:{index}", function.name))),
                ("fn", Json::str(function.name.clone())),
                ("args", Json::Array(args)),
            ]));
        }
    }
    out
}

// ---------------------------------------------------------------------------
// the harness
// ---------------------------------------------------------------------------

const HARNESS: &str = include_str!("cpp_runtime/harness_c.cpp.in");

pub fn harness_source(_file: &Path, functions: &[CFunction]) -> String {
    let mut decls = String::new();
    for function in functions {
        let params = function
            .params_c
            .iter()
            .map(|(ty, name)| format!("{ty} {name}"))
            .collect::<Vec<_>>()
            .join(", ");
        let params = if params.is_empty() {
            "void".to_string()
        } else {
            params
        };
        decls.push_str(&format!(
            "  {} cppan_original_{}({params});\n",
            function.ret_c, function.name
        ));
    }
    let mut dispatch = String::new();
    for function in functions {
        dispatch.push_str(&format!("    if (name == \"{}\") {{\n", function.name));
        for (index, (ty, _)) in function.params.iter().enumerate() {
            let decode = match ty {
                Ty::Str => format!(
                    "      const std::string arg{index}_text = args.at({index}).as_string();\n      const char* arg{index} = arg{index}_text.c_str();\n"
                ),
                Ty::Int => format!(
                    "      const auto arg{index} = static_cast<long long>(args.at({index}).as_int());\n"
                ),
                Ty::Float => {
                    format!("      const double arg{index} = args.at({index}).as_number();\n")
                }
                Ty::Bool => format!("      const bool arg{index} = args.at({index}).as_bool();\n"),
                _ => format!("      const Json& arg{index} = args.at({index});\n"),
            };
            dispatch.push_str(&decode);
        }
        let arguments = (0..function.params.len())
            .map(|index| format!("arg{index}"))
            .collect::<Vec<_>>()
            .join(", ");
        let call = format!("cppan_original_{}({arguments})", function.name);
        let encode = match function.ret {
            Ty::Str => format!(
                "      char* result = {call};\n      return ok_json(result != nullptr ? Json::string(std::string(result)) : Json::null());\n"
            ),
            Ty::Int => format!("      return ok_json(Json::integer(static_cast<std::int64_t>({call})));\n"),
            Ty::Float => format!("      return ok_json(Json::real(static_cast<double>({call})));\n"),
            Ty::Bool => format!("      return ok_json(Json::boolean({call} != 0));\n"),
            _ => format!("      {call};\n      return ok_json(Json::null());\n"),
        };
        dispatch.push_str(&encode);
        dispatch.push_str("    }\n");
    }
    HARNESS
        .replace("{{DECLS}}", &decls)
        .replace("{{DISPATCH}}", &dispatch)
}

// ---------------------------------------------------------------------------
// repository analysis, recording and timing
// ---------------------------------------------------------------------------

pub fn analyze_repo(repo: &Path) -> Result<Json, String> {
    let mut modules = Vec::new();
    let mut stack = vec![(repo.to_path_buf(), 0usize)];
    while let Some((directory, depth)) = stack.pop() {
        if depth > 4 {
            continue;
        }
        let entries = std::fs::read_dir(&directory)
            .map_err(|error| format!("{}: {error}", directory.display()))?;
        for entry in entries.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            if path.is_dir() {
                if matches!(
                    name.as_str(),
                    "build" | "node_modules" | "target" | "vendor" | ".git" | "cpp" | "harness"
                ) {
                    continue;
                }
                stack.push((path, depth + 1));
                continue;
            }
            if path.extension().and_then(|ext| ext.to_str()) != Some("c") {
                continue;
            }
            let Ok(analysis) = analyze_file(&path) else {
                continue;
            };
            if analysis.functions.is_empty() {
                continue;
            }
            let relative = path
                .strip_prefix(repo)
                .unwrap_or(&path)
                .display()
                .to_string();
            modules.push(Json::obj([
                ("module", Json::str(relative.clone())),
                ("path", Json::str(relative)),
                ("lines", Json::Int(i64::from(analysis.lines))),
                (
                    "functions",
                    Json::Array(
                        analysis
                            .functions
                            .iter()
                            .map(|function| {
                                Json::obj([
                                    ("name", Json::str(function.name.clone())),
                                    ("params", Json::Int(function.params.len() as i64)),
                                    ("typed", Json::Int(function.params.len() as i64)),
                                    ("returns", Json::str(function.ret_c.clone())),
                                ])
                            })
                            .collect(),
                    ),
                ),
                (
                    "risks",
                    Json::Array(vec![
                        Json::str("C return codes and errno are the failure contract"),
                        Json::str("char* may be null; the port must keep the difference from \"\""),
                        Json::str("undefined behaviour is recorded as a crash, not as a failure"),
                    ]),
                ),
                ("imports", Json::Array(Vec::new())),
                ("entry_point", Json::Bool(false)),
            ]));
        }
    }
    Ok(Json::obj([
        ("root", Json::str(repo.display().to_string())),
        ("modules", Json::Array(modules)),
        ("dependencies", Json::Array(Vec::new())),
    ]))
}

fn harness_dir(project: &crate::project::Project) -> std::path::PathBuf {
    project.dir.join("harness")
}

fn harness_exe(project: &crate::project::Project) -> std::path::PathBuf {
    harness_dir(project).join("build").join(if cfg!(windows) {
        "cppan_harness.exe"
    } else {
        "cppan_harness"
    })
}

/// Writes the harness project and builds it: the original compiled as C with its
/// exported names prefixed, plus the JSON driver around it.
pub fn build_harness(
    project: &crate::project::Project,
    unit: &Unit,
) -> Result<std::path::PathBuf, String> {
    let analysis = analyze_file(&project.source_file)?;
    let dir = harness_dir(project);
    std::fs::create_dir_all(&dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    util::write(
        &dir.join("main.cpp"),
        &harness_source(&project.source_file, &analysis.functions),
    )?;
    let definitions = analysis
        .functions
        .iter()
        .map(|function| format!("{name}=cppan_original_{name}", name = function.name))
        .collect::<Vec<_>>()
        .join(";");
    let cmake = format!(
        r#"cmake_minimum_required(VERSION 3.28)
project(cppan_harness LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(cppan_harness main.cpp "{source}")
target_include_directories(cppan_harness PRIVATE "{include}")
target_compile_definitions(cppan_harness PRIVATE {definitions})
if(MSVC)
  target_compile_options(cppan_harness PRIVATE /W3 /utf-8)
else()
  target_compile_options(cppan_harness PRIVATE -Wall)
endif()
"#,
        source = project.source_file.display(),
        include = project.dir.join("include").display(),
        definitions = definitions
    );
    util::write(&dir.join("CMakeLists.txt"), &cmake)?;
    let _ = unit;
    let configure = util::run(
        "cmake",
        &[
            "-S".to_string(),
            dir.display().to_string(),
            "-B".to_string(),
            dir.join("build").display().to_string(),
            "-DCMAKE_BUILD_TYPE=Release".to_string(),
        ],
        Some(&dir),
    );
    if !configure.ok {
        return Err(format!(
            "harness configure failed: {}\n{}",
            configure.failure(),
            util::indent(&configure.stderr, 2)
        ));
    }
    let build = util::run(
        "cmake",
        &[
            "--build".to_string(),
            dir.join("build").display().to_string(),
            "-j".to_string(),
            "4".to_string(),
        ],
        Some(&dir),
    );
    if !build.ok {
        return Err(format!(
            "harness build failed: {}\n{}",
            build.failure(),
            util::indent(&build.stderr, 2)
        ));
    }
    let exe = harness_exe(project);
    if !exe.exists() {
        return Err(format!(
            "harness build succeeded but {} is missing",
            exe.display()
        ));
    }
    Ok(exe)
}

pub struct Recorded {
    pub cases: usize,
    pub crashes: usize,
}

/// Records the original: generated calls in, the harness's results out.
pub fn record(
    project: &crate::project::Project,
    unit: &Unit,
    count: usize,
) -> Result<Recorded, String> {
    let exe = build_harness(project, unit)?;
    let calls = calls(unit, count);
    let mut text = String::new();
    for call in &calls {
        text.push_str(&call.to_string_compact());
        text.push('\n');
    }
    let calls_path = project.dir.join(".cppan").join("calls.jsonl");
    let results_path = project.dir.join(".cppan").join("original-results.jsonl");
    util::write(&calls_path, &text)?;
    let run = util::run(
        &exe.display().to_string(),
        &[
            "--cases".to_string(),
            calls_path.display().to_string(),
            "--out".to_string(),
            results_path.display().to_string(),
        ],
        Some(&project.dir),
    );
    if !run.ok {
        return Err(format!(
            "the harness failed: {}\n{}",
            run.failure(),
            util::indent(&run.stderr, 2)
        ));
    }
    let results = util::read(&results_path)?;
    let mut cases = String::new();
    let mut by_id: std::collections::BTreeMap<String, (String, Json)> =
        std::collections::BTreeMap::new();
    for call in &calls {
        let id = call
            .get("id")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let function = call
            .get("fn")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        by_id.insert(
            id,
            (
                function,
                call.get("args").cloned().unwrap_or(Json::Array(Vec::new())),
            ),
        );
    }
    let mut crashes = 0usize;
    let mut count_cases = 0usize;
    for line in results.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)?;
        let id = doc
            .get("id")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let Some((function, args)) = by_id.get(&id) else {
            continue;
        };
        let result = doc.get("result").cloned().unwrap_or(Json::Null);
        let expect = if let Some(error) = result.get("err") {
            let kind = error.get("type").and_then(Json::as_str).unwrap_or("");
            let message = error.get("msg").and_then(Json::as_str).unwrap_or("");
            if kind == "Crash" {
                crashes += 1;
                Json::obj([("crash", Json::str(message.to_string()))])
            } else {
                Json::obj([(
                    "err",
                    Json::obj([
                        (
                            "type",
                            Json::str(if kind.is_empty() { "CError" } else { kind }),
                        ),
                        ("msg", Json::str(message)),
                    ]),
                )])
            }
        } else {
            Json::obj([("ok", result.get("ok").cloned().unwrap_or(Json::Null))])
        };
        let entry = Json::obj([
            ("id", Json::str(id)),
            ("fn", Json::str(function.clone())),
            ("args", args.clone()),
            ("expect", expect),
        ]);
        cases.push_str(&entry.to_string_compact());
        cases.push('\n');
        count_cases += 1;
    }
    util::write(&project.dir.join("cases.jsonl"), &cases)?;
    util::write(
        &project
            .dir
            .join(crate::project::META_DIR)
            .join("recorded-from.json"),
        &Json::obj([
            ("schema", Json::str("cpp-anything/recorded-from@1")),
            (
                "source_hash",
                Json::str(util::hash64(
                    &util::read(&project.source_file).unwrap_or_default(),
                )),
            ),
            (
                "from_tests",
                Json::obj([
                    ("calls", Json::Int(count_cases as i64)),
                    ("at", Json::str(util::now_iso8601())),
                ]),
            ),
        ])
        .to_string_pretty(),
    )?;
    Ok(Recorded {
        cases: count_cases,
        crashes,
    })
}

pub fn bench_original(
    project: &crate::project::Project,
    unit: &Unit,
    repeat: usize,
) -> Result<f64, String> {
    let exe = build_harness(project, unit)?;
    let calls = calls(unit, 64);
    let mut text = String::new();
    for call in &calls {
        text.push_str(&call.to_string_compact());
        text.push('\n');
    }
    let calls_path = project.dir.join(".cppan").join("bench-calls.jsonl");
    util::write(&calls_path, &text)?;
    let run = util::run(
        &exe.display().to_string(),
        &[
            "--cases".to_string(),
            calls_path.display().to_string(),
            "--bench".to_string(),
            repeat.max(1).to_string(),
        ],
        Some(&project.dir),
    );
    if !run.ok {
        return Err(format!("the harness bench failed: {}", run.failure()));
    }
    let doc = Json::parse(run.stdout.trim())?;
    doc.get("ns_per_call")
        .and_then(Json::as_f64)
        .ok_or_else(|| "the harness printed no ns_per_call".to_string())
}
