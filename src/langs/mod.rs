//! The scripting-language plugins: the analyzer, the recorder and the binding
//! that each source language needs. The loop around them (wire format, shadow
//! log, app, map, gate) is the same code Python and C already use.
//!
//! Supported signatures are a deliberate subset: plain module-level functions
//! with values the wire can carry. Everything else is reported as skipped with
//! a reason, never guessed.

use std::path::{Path, PathBuf};

use crate::inventory::{Function, Param, Unit};
use crate::json::Json;
use crate::project::Project;
use crate::types::Ty;
use crate::util;

pub mod csharp;
pub mod go;
pub mod java;
pub mod php;
pub mod ruby;
pub mod ts;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Lang {
    Ts,
    Js,
    Go,
    Ruby,
    Java,
    Php,
    Csharp,
}

impl Lang {
    pub fn id(self) -> &'static str {
        match self {
            Lang::Ts => "ts",
            Lang::Js => "js",
            Lang::Go => "go",
            Lang::Ruby => "ruby",
            Lang::Java => "java",
            Lang::Php => "php",
            Lang::Csharp => "csharp",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Lang::Ts => "TypeScript",
            Lang::Js => "JavaScript",
            Lang::Go => "Go",
            Lang::Ruby => "Ruby",
            Lang::Java => "Java",
            Lang::Php => "PHP",
            Lang::Csharp => "C#",
        }
    }

    /// PHP and C# are whole-app only: the app loop, not per-unit ports.
    pub fn whole_app_only(self) -> bool {
        matches!(self, Lang::Php | Lang::Csharp)
    }

    pub fn extensions(self) -> &'static [&'static str] {
        match self {
            Lang::Ts => &["ts", "mts", "cts"],
            Lang::Js => &["js", "mjs", "cjs", "jsx"],
            Lang::Go => &["go"],
            Lang::Ruby => &["rb"],
            Lang::Java => &["java"],
            Lang::Php => &["php"],
            Lang::Csharp => &["cs"],
        }
    }

    /// A unit is a file, except in Go where it is a package (a folder).
    pub fn unit_is_directory(self) -> bool {
        matches!(self, Lang::Go)
    }

    pub fn is_ts_family(self) -> bool {
        matches!(self, Lang::Ts | Lang::Js)
    }
}

pub fn parse(name: &str) -> Option<Lang> {
    match name {
        "ts" | "typescript" => Some(Lang::Ts),
        "js" | "javascript" | "node" => Some(Lang::Js),
        "go" | "golang" => Some(Lang::Go),
        "rb" | "ruby" => Some(Lang::Ruby),
        "java" => Some(Lang::Java),
        "php" => Some(Lang::Php),
        "cs" | "c#" | "csharp" | "dotnet" => Some(Lang::Csharp),
        _ => None,
    }
}

/// The repository's language from its manifest, else from the most common source
/// extension. `None` means "not one of the plugins": the caller keeps Python.
pub fn detect(repo: &Path) -> Option<Lang> {
    let has = |name: &str| repo.join(name).is_file();
    let manifest = if has("go.mod") {
        Some(Lang::Go)
    } else if has("pom.xml") || has("build.gradle") || has("build.gradle.kts") {
        Some(Lang::Java)
    } else if has("composer.json") {
        Some(Lang::Php)
    } else if has("Directory.Build.props") {
        Some(Lang::Csharp)
    } else if has("Gemfile") {
        Some(Lang::Ruby)
    } else if has("package.json") {
        Some(if has("tsconfig.json") {
            Lang::Ts
        } else {
            Lang::Js
        })
    } else {
        None
    };
    if let Some(lang) = manifest {
        // A manifest decides unless the repository is mostly Python, where a
        // package.json is usually the frontend or a dev tool.
        let py = count_files(repo, &["py"]);
        let own = count_files(repo, lang.extensions());
        return if py > own { None } else { Some(lang) };
    }
    let mut counts = std::collections::BTreeMap::new();
    let mut stack = vec![(repo.to_path_buf(), 0usize)];
    while let Some((directory, depth)) = stack.pop() {
        if depth > 3 {
            continue;
        }
        let Ok(entries) = std::fs::read_dir(&directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            if path.is_dir() {
                if !matches!(
                    name.as_str(),
                    "build" | "node_modules" | "target" | "vendor" | ".git" | "cpp" | "dist"
                ) {
                    stack.push((path, depth + 1));
                }
                continue;
            }
            let extension = path.extension().and_then(|ext| ext.to_str()).unwrap_or("");
            for lang in [Lang::Ts, Lang::Js, Lang::Go, Lang::Ruby, Lang::Java] {
                if lang.extensions().contains(&extension) {
                    *counts.entry(lang).or_insert(0usize) += 1;
                }
            }
        }
    }
    let best = counts
        .into_iter()
        .max_by_key(|(lang, count)| (*count, std::cmp::Reverse(lang.id())));
    match best {
        Some((lang, count)) if count > 0 => Some(lang),
        _ => None,
    }
}

fn count_files(repo: &Path, extensions: &[&str]) -> usize {
    let mut count = 0usize;
    let mut stack = vec![(repo.to_path_buf(), 0usize)];
    while let Some((directory, depth)) = stack.pop() {
        if depth > 3 {
            continue;
        }
        let Ok(entries) = std::fs::read_dir(&directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            if path.is_dir() {
                if !matches!(
                    name.as_str(),
                    "build" | "node_modules" | "target" | "vendor" | ".git" | "cpp" | "dist"
                ) {
                    stack.push((path, depth + 1));
                }
                continue;
            }
            if path
                .extension()
                .and_then(|ext| ext.to_str())
                .is_some_and(|ext| extensions.contains(&ext))
            {
                count += 1;
            }
        }
    }
    count
}

/// Which plugin owns a unit, from its path: a file extension, or a `.go` package
/// directory. `--lang` overrides this.
pub fn infer(repo: &Path, unit: &str) -> Option<Lang> {
    let path = if Path::new(unit).is_absolute() {
        PathBuf::from(unit)
    } else {
        repo.join(unit)
    };
    if path.is_dir() {
        let is_go = std::fs::read_dir(&path)
            .ok()?
            .flatten()
            .any(|entry| entry.path().extension().and_then(|ext| ext.to_str()) == Some("go"));
        return if is_go { Some(Lang::Go) } else { None };
    }
    let extension = path.extension().and_then(|ext| ext.to_str())?;
    match extension {
        "ts" | "mts" | "cts" => Some(Lang::Ts),
        "js" | "mjs" | "cjs" | "jsx" => Some(Lang::Js),
        "go" => Some(Lang::Go),
        "rb" => Some(Lang::Ruby),
        "java" => Some(Lang::Java),
        "php" => Some(Lang::Php),
        "cs" => Some(Lang::Csharp),
        _ => None,
    }
}

pub struct FoundParam {
    pub name: String,
    pub ty: String,
    pub has_default: bool,
    pub default: Option<Json>,
}

pub struct Found {
    pub name: String,
    pub line: u32,
    /// How the harness calls the original (`TOPLEVEL`, `Pkg.Fn`, `Class.method`).
    pub call: String,
    pub params: Vec<FoundParam>,
    pub returns: String,
    pub doc: String,
}

pub struct Analysis {
    pub functions: Vec<Found>,
    pub skipped: Vec<(String, String)>,
    pub imports: Vec<String>,
    pub lines: u32,
}

impl Analysis {
    pub fn to_function(&self, found: &Found) -> Function {
        Function {
            name: found.name.clone(),
            line: found.line,
            params: found
                .params
                .iter()
                .map(|param| Param {
                    name: param.name.clone(),
                    annotation: param.ty.clone(),
                    ty: Ty::parse(&param.ty),
                    default: param.default.clone(),
                    has_default: param.has_default,
                    keyword_only: false,
                })
                .collect(),
            returns: Ty::parse(&found.returns),
            returns_annotation: found.returns.clone(),
            doc: found.doc.clone(),
            risks: Vec::new(),
            raises: false,
        }
    }
}

pub struct RecordOptions {
    pub inputs: Option<PathBuf>,
    /// Import the original from this module instead of the source file (TS/JS).
    pub module: Option<PathBuf>,
    pub resolve_from: Option<PathBuf>,
    pub node_args: Vec<String>,
}

impl Default for RecordOptions {
    fn default() -> Self {
        RecordOptions {
            inputs: None,
            module: None,
            resolve_from: None,
            node_args: Vec::new(),
        }
    }
}

// ---------------------------------------------------------------------------
// repo plans
// ---------------------------------------------------------------------------

pub fn analyze_repo(repo: &Path, lang: Lang) -> Result<Json, String> {
    let mut modules = Vec::new();
    let mut stack = vec![(repo.to_path_buf(), 0usize)];
    let mut seen_dirs: Vec<PathBuf> = Vec::new();
    while let Some((directory, depth)) = stack.pop() {
        if depth > 5 {
            continue;
        }
        let entries = std::fs::read_dir(&directory)
            .map_err(|error| format!("{}: {error}", directory.display()))?;
        let mut files: Vec<PathBuf> = Vec::new();
        for entry in entries.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            if path.is_dir() {
                if matches!(
                    name.as_str(),
                    "build"
                        | "node_modules"
                        | "target"
                        | "vendor"
                        | ".git"
                        | "cpp"
                        | "dist"
                        | "testdata"
                ) {
                    continue;
                }
                stack.push((path, depth + 1));
                continue;
            }
            let extension = path.extension().and_then(|ext| ext.to_str()).unwrap_or("");
            if lang.extensions().contains(&extension) {
                files.push(path);
            }
        }
        files.sort();
        if lang.unit_is_directory() {
            if files.is_empty() || seen_dirs.contains(&directory) {
                continue;
            }
            seen_dirs.push(directory.clone());
            if let Ok(analysis) = go::analyze_package(&directory) {
                if analysis.functions.is_empty() {
                    continue;
                }
                modules.push(module_json(lang, &directory, repo, &analysis));
            }
            continue;
        }
        for file in files {
            let Ok(analysis) = analyze_file(&file, lang) else {
                continue;
            };
            if analysis.functions.is_empty() {
                continue;
            }
            modules.push(module_json(lang, &file, repo, &analysis));
        }
    }
    modules.sort_by_key(|module| {
        module
            .get("module")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string()
    });
    Ok(Json::obj([
        ("language", Json::str(lang.id())),
        ("modules", Json::Array(modules)),
    ]))
}

fn module_json(lang: Lang, path: &Path, repo: &Path, analysis: &Analysis) -> Json {
    let relative = path.strip_prefix(repo).unwrap_or(path);
    let module = module_name(relative);
    let functions = analysis
        .functions
        .iter()
        .map(|found| {
            Json::obj([
                ("name", Json::str(found.name.clone())),
                ("line", Json::Int(i64::from(found.line))),
                ("params", Json::Int(found.params.len() as i64)),
                ("returns", Json::str(found.returns.clone())),
                ("doc", Json::str(found.doc.clone())),
            ])
        })
        .collect::<Vec<_>>();
    Json::obj([
        ("module", Json::str(module)),
        ("unit", Json::str(relative.display().to_string())),
        ("file", Json::str(relative.display().to_string())),
        ("lines", Json::Int(i64::from(analysis.lines))),
        ("functions", Json::Array(functions)),
        (
            "imports",
            Json::Array(
                analysis
                    .imports
                    .iter()
                    .map(|import| Json::str(import.clone()))
                    .collect(),
            ),
        ),
        (
            "risks",
            Json::Array(
                language_risks(lang)
                    .iter()
                    .map(|risk| Json::str(*risk))
                    .collect(),
            ),
        ),
        ("language", Json::str(lang.id())),
    ])
}

pub fn language_risks(lang: Lang) -> &'static [&'static str] {
    match lang {
        Lang::Ts | Lang::Js => &[
            "JavaScript numbers are doubles: integers beyond 2^53 are not int64, and the wire marks them",
            "Strings are UTF-16: an offset is not a byte offset, and emoji are two units",
            "undefined, null and a missing key are three different things in JavaScript, two in the wire",
            "Modules with side effects on import make recording observe more than the functions",
        ],
        Lang::Go => &[
            "Go ints are 64-bit on this platform but 32-bit elsewhere: the signature decides the width",
            "Strings are UTF-8 bytes: len() is bytes, runes are code points, and the port must choose",
            "nil slices and empty slices print the same, marshal differently, and compare against nil",
            "Named result parameters and deferred writes: the recorder observes the final value",
        ],
        Lang::Ruby => &[
            "Ruby integers are unbounded: values beyond int64 cannot cross the wire and are recorded as errors",
            "Strings are bytes with an encoding: a substring is not a character slice unless told so",
            "Symbols, ranges and custom objects do not cross the wire; the recorder reports them as errors",
            "Hash order is insertion order, and the wire preserves object key order",
        ],
        Lang::Java => &[
            "Java ints are 32-bit, longs 64-bit, and the wire carries int64: the signature decides the width",
            "char is UTF-16 code units, String is UTF-16: offsets and lengths are not bytes",
            "null and an empty string are different values, and both cross the wire",
            "Checked exceptions are part of the original's contract; the port mirrors the type names",
        ],
        Lang::Php => &["PHP is whole-app only in this tool: unit-level ports arrive with a per-unit harness"],
        Lang::Csharp => {
            &["C# is whole-app only in this tool: unit-level ports arrive with a per-unit harness"]
        }
    }
}

pub fn module_name(relative: &Path) -> String {
    let mut parts: Vec<String> = relative
        .with_extension("")
        .components()
        .map(|part| part.as_os_str().to_string_lossy().into_owned())
        .collect();
    if parts.last().map(String::as_str) == Some("index") {
        parts.pop();
    }
    if parts.is_empty() {
        return "unit".to_string();
    }
    parts.join(".")
}

// ---------------------------------------------------------------------------
// units
// ---------------------------------------------------------------------------

pub fn analyze_file(file: &Path, lang: Lang) -> Result<Analysis, String> {
    match lang {
        Lang::Ruby => ruby::analyze(file),
        Lang::Go => go::analyze_package(
            file.parent()
                .ok_or_else(|| format!("{}: no parent directory", file.display()))?,
        ),
        Lang::Java => java::analyze(file),
        Lang::Ts | Lang::Js => ts::analyze(file, lang),
        Lang::Php => php::analyze(file),
        Lang::Csharp => csharp::analyze(file),
    }
}

/// Resolves the unit argument to a file (or a Go package directory) and builds
/// the inventory the rest of the tool consumes.
pub fn inventory(repo: &Path, unit: &str, lang: Lang) -> Result<Unit, String> {
    let repo_abs = repo
        .canonicalize()
        .map_err(|error| format!("{}: {error}", repo.display()))?;
    let path = if Path::new(unit).is_absolute() {
        PathBuf::from(unit)
    } else {
        repo_abs.join(unit)
    };
    let path = path
        .canonicalize()
        .map_err(|error| format!("{}: {error}", path.display()))?;
    let (source_file, analysis) = if lang.unit_is_directory() {
        if !path.is_dir() {
            return Err(format!(
                "{}: a Go unit is a package directory, not a file",
                path.display()
            ));
        }
        (path.clone(), go::analyze_package(&path)?)
    } else {
        if !path.is_file() {
            return Err(format!("{}: not a file", path.display()));
        }
        (path.clone(), analyze_file(&path, lang)?)
    };
    if analysis.functions.is_empty() {
        return Err(format!(
            "{} has no functions with a supported signature ({} skipped)",
            path.display(),
            analysis.skipped.len()
        ));
    }
    let unit_name = if lang.unit_is_directory() {
        path.file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| "unit".to_string())
    } else {
        path.file_stem()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| "unit".to_string())
    };
    let module = module_name(path.strip_prefix(&repo_abs).unwrap_or(&path));
    Ok(Unit {
        unit: unit_name,
        module,
        file: source_file,
        package_root: Some(repo_abs),
        lines: analysis.lines,
        functions: analysis
            .functions
            .iter()
            .map(|found| analysis.to_function(found))
            .collect(),
        skipped: analysis.skipped.clone(),
        risks: language_risks(lang)
            .iter()
            .map(|risk| risk.to_string())
            .collect(),
        imports: analysis.imports.clone(),
    })
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

pub fn record(project: &Project, count: usize, options: &RecordOptions) -> Result<usize, String> {
    let lang = parse(&project.source_lang)
        .ok_or_else(|| format!("{} is not a plugin language", project.source_lang))?;
    if lang.whole_app_only() {
        return Err(format!(
            "{} is whole-app only: record the app (`app record -- ...`), not a unit",
            lang.label()
        ));
    }
    let repo = project
        .package_root
        .clone()
        .unwrap_or_else(|| project.dir.clone());
    let unit = inventory(&repo, &project.source_file.display().to_string(), lang)?;
    let mut calls = crate::corpora::calls(&unit.functions, count);
    if let Some(inputs) = &options.inputs {
        let text = util::read(inputs)?;
        for (number, line) in text.lines().enumerate() {
            if line.trim().is_empty() {
                continue;
            }
            let doc = Json::parse(line)
                .map_err(|error| format!("{}:{}: {error}", inputs.display(), number + 1))?;
            calls.push(Json::obj([
                ("id", Json::str(format!("input:{number}"))),
                ("fn", doc.get("fn").cloned().unwrap_or(Json::Null)),
                (
                    "args",
                    doc.get("args").cloned().unwrap_or(Json::Array(Vec::new())),
                ),
            ]));
        }
    }
    let written = match lang {
        Lang::Ruby => ruby::record(project, &unit, &calls, options)?,
        Lang::Go => go::record(project, &unit, &calls, options)?,
        Lang::Java => java::record(project, &unit, &calls, options)?,
        Lang::Ts | Lang::Js => ts::record(project, &unit, &calls, options)?,
        Lang::Php | Lang::Csharp => unreachable!(),
    };
    Ok(written)
}

/// `case id` is `<function>:<n>`; the recorded `fn` is the part before the last colon.
pub fn function_of(id: &str) -> String {
    id.rsplit_once(':')
        .map(|(name, _)| name.to_string())
        .unwrap_or_else(|| id.to_string())
}

pub fn cases_json(calls: &[Json]) -> String {
    calls
        .iter()
        .map(|call| call.to_string_compact())
        .collect::<Vec<_>>()
        .join("\n")
}

/// Time the original in its own runtime. Each plugin loads the unit once, calls
/// every function `repeat` times with its first recorded case, and returns
/// nanoseconds for the whole run.
pub fn bench_original(project: &Project, repeat: usize) -> Result<f64, String> {
    let lang = parse(&project.source_lang)
        .ok_or_else(|| format!("{} is not a plugin language", project.source_lang))?;
    match lang {
        Lang::Ruby => ruby::bench(project, repeat),
        Lang::Go => go::bench(project, repeat),
        Lang::Java => java::bench(project, repeat),
        Lang::Ts | Lang::Js => ts::bench(project, repeat, lang),
        Lang::Php | Lang::Csharp => Err(format!(
            "{} is whole-app only: bench the app, not a unit",
            lang.label()
        )),
    }
}

/// Writes `cases.jsonl` from harness output lines: `{"id", "ok"}` or
/// `{"id", "err": {"type", "msg"}}`. The input side is merged from the calls that
/// were sent, so the harness only reports what the original answered.
pub fn write_cases(project: &Project, calls: &[Json], output: &str) -> Result<usize, String> {
    let mut by_id = std::collections::BTreeMap::new();
    for call in calls {
        if let Some(id) = call.get("id").and_then(Json::as_str) {
            by_id.insert(id.to_string(), call.clone());
        }
    }
    let mut text = String::new();
    let mut count = 0usize;
    for (number, line) in output.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)
            .map_err(|error| format!("recording line {}: {error}: {line}", number + 1))?;
        let id = doc
            .get("id")
            .and_then(Json::as_str)
            .ok_or_else(|| format!("recording line {}: no id", number + 1))?;
        let call = by_id.get(id);
        let function = call
            .and_then(|call| call.get("fn"))
            .and_then(Json::as_str)
            .map(str::to_string)
            .or_else(|| {
                call.map(|_| function_of(id))
                    .filter(|name| !name.is_empty())
            })
            .unwrap_or_else(|| function_of(id));
        let args = call
            .and_then(|call| call.get("args"))
            .cloned()
            .unwrap_or(Json::Array(Vec::new()));
        let expect = if let Some(error) = doc.get("err") {
            Json::obj([("err", error.clone())])
        } else {
            Json::obj([("ok", doc.get("ok").cloned().unwrap_or(Json::Null))])
        };
        let entry = Json::obj([
            ("id", Json::str(id.to_string())),
            ("fn", Json::str(function)),
            ("args", args),
            ("expect", expect),
        ]);
        text.push_str(&entry.to_string_compact());
        text.push('\n');
        count += 1;
    }
    util::write(&project.dir.join("cases.jsonl"), &text)?;
    Ok(count)
}

/// The language-specific shadow files. Python, C and C++ keep the ctypes module
/// `codegen` writes; these call the same C ABI from the original's runtime.
pub fn shadow_files(project: &Project, ns: &str) -> Result<Vec<(String, String)>, String> {
    let Some(lang) = parse(&project.source_lang) else {
        return Ok(Vec::new());
    };
    let mut files = match lang {
        Lang::Ruby => ruby::shadow_files(project, ns),
        Lang::Go => go::shadow_files(project, ns),
        Lang::Java => java::shadow_files(project, ns),
        Lang::Ts | Lang::Js => ts::shadow_files(project, ns, lang),
        Lang::Php | Lang::Csharp => Vec::new(),
    };
    // The C ABI is the same for every runtime; every binding loads this library.
    files.push((
        "shadow/README.md".to_string(),
        shadow_readme(project, ns, lang),
    ));
    Ok(files)
}

fn shadow_readme(project: &Project, ns: &str, lang: Lang) -> String {
    format!(
        "# Shadow for `{module}` ({lang})\n\nThe original {label} runs first, the C++ port runs second, and every\ndisagreement is appended to `shadow/{ns}.shadow.jsonl`; callers only ever see the\noriginal's result. The binding talks to the port through the C ABI\n(`cppan_shadow_call` / `cppan_shadow_free`) in `build/libcppan_shadow.so`.\n\nBuild the port first: `cpp-anything build <project>`.\n\nEnvironment:\n- `CPPAN_SHADOW=off` calls only the original.\n- `CPPAN_SHADOW_LOG=...` writes the log elsewhere.\n",
        module = project.module,
        lang = lang.id(),
        label = lang.label(),
        ns = ns
    )
}

// ---------------------------------------------------------------------------
// helpers shared by the plugins
// ---------------------------------------------------------------------------

/// Calls the `record` harness that lives in the project's build directory.
pub fn run_harness(
    project: &Project,
    program: &str,
    args: &[String],
    cwd: Option<&Path>,
) -> Result<String, String> {
    let run = util::run(program, args, cwd.or(Some(&project.dir)));
    if !run.ok {
        return Err(format!(
            "the {program} recorder failed: {}\n{}",
            run.failure(),
            util::indent(&run.stderr, 2)
        ));
    }
    Ok(run.stdout)
}
