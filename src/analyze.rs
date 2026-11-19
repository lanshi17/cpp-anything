//! `analyze`: what is in the repository, what porting each unit costs, and where to
//! start. `--whole-app` adds the app's shape, every dependency with its C++ library,
//! the port order and the decisions the user has to make.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::json::Json;
use crate::libs::{self, Fit};
use crate::util;

pub struct Resolved {
    pub file: PathBuf,
    pub module: String,
    pub package_root: Option<PathBuf>,
}

pub fn resolve(repo: &Path, unit: &str, module_root: Option<&Path>) -> Result<Resolved, String> {
    let repo = repo
        .canonicalize()
        .map_err(|error| format!("{}: {error}", repo.display()))?;
    let base = match module_root {
        Some(root) => {
            let root = if root.is_absolute() {
                root.to_path_buf()
            } else {
                repo.join(root)
            };
            root.canonicalize()
                .map_err(|error| format!("{}: {error}", root.display()))?
        }
        None => repo.clone(),
    };

    let direct = if Path::new(unit).is_absolute() {
        PathBuf::from(unit)
    } else {
        repo.join(unit)
    };
    let (file, module) = if direct.is_file() {
        let file = direct
            .canonicalize()
            .map_err(|error| format!("{}: {error}", direct.display()))?;
        (file.clone(), module_name(&file, &base))
    } else {
        let parts: Vec<&str> = unit.split('.').collect();
        let module_file = base.join(parts.join("/")).with_extension("py");
        let package_file = base.join(parts.join("/")).join("__init__.py");
        if module_file.is_file() {
            (module_file, unit.to_string())
        } else if package_file.is_file() {
            (package_file, unit.to_string())
        } else {
            return Err(format!(
                "cannot find unit {unit:?}: looked for {} and {}\n  \
                 pass the path to a .py file, a dotted module, or --module-root if the package \
                 root is not the repository root",
                module_file.display(),
                package_file.display()
            ));
        }
    };

    let package_root = Some(base.clone());
    Ok(Resolved {
        file,
        module,
        package_root,
    })
}

fn module_name(file: &Path, base: &Path) -> String {
    let relative = file.strip_prefix(base).unwrap_or(file);
    let mut parts: Vec<String> = relative
        .with_extension("")
        .components()
        .map(|part| part.as_os_str().to_string_lossy().into_owned())
        .collect();
    if parts.last().map(String::as_str) == Some("__init__") {
        parts.pop();
    }
    parts.join(".")
}

pub fn analyze(repo: &Path) -> Result<Json, String> {
    if crate::python::available().is_none() {
        return Err(format!(
            "{} is not on PATH: analyze reads Python with the project's own interpreter \
             (set CPPAN_PYTHON to choose one)",
            crate::python::interpreter()
        ));
    }
    crate::python::analyze(repo)
}

// ---------------------------------------------------------------------------
// the plan
// ---------------------------------------------------------------------------

struct Unit {
    module: String,
    lines: u32,
    functions: Vec<Json>,
    risks: Vec<String>,
    imports: Vec<String>,
    entry_point: bool,
    status: &'static str,
    note: String,
    readiness: i64,
    priority: i64,
    wave: usize,
}

fn units(doc: &Json) -> Vec<Unit> {
    let mut out = Vec::new();
    for module in doc
        .get("modules")
        .and_then(Json::as_array)
        .unwrap_or_default()
    {
        let imports: Vec<String> = module
            .get("imports")
            .and_then(Json::as_array)
            .map(|items| {
                items
                    .iter()
                    .filter_map(|item| item.as_str().map(str::to_string))
                    .collect()
            })
            .unwrap_or_default();
        let risks: Vec<String> = module
            .get("risks")
            .and_then(Json::as_array)
            .map(|items| {
                items
                    .iter()
                    .filter_map(|item| item.as_str().map(str::to_string))
                    .collect()
            })
            .unwrap_or_default();
        let functions = module
            .get("functions")
            .and_then(Json::as_array)
            .map(<[Json]>::to_vec)
            .unwrap_or_default();
        let lines = module.get("lines").and_then(Json::as_i64).unwrap_or(0) as u32;

        let mut none_deps = 0;
        let mut unknown_deps = 0;
        let mut kept = false;
        let mut blockers = Vec::new();
        for import in &imports {
            let package = libs::reduce(import);
            match libs::find(package) {
                Some(entry) if entry.fit == Fit::None => {
                    none_deps += 1;
                    blockers.push(format!("`{package}` has no C++ equivalent in the table"));
                }
                Some(entry) if entry.libs.is_empty() && entry.fit != Fit::Partial => kept = true,
                Some(_) => {}
                None => unknown_deps += 1,
            }
        }
        if imports.iter().any(|import| {
            matches!(
                libs::reduce(import),
                "tkinter" | "PyQt5" | "PyQt6" | "PySide2" | "PySide6" | "kivy" | "pygame"
            )
        }) {
            kept = true;
        }

        let (status, mut note) = if kept {
            (
                "keep",
                "UI code stays in Python; port what it calls".to_string(),
            )
        } else if none_deps > 0 {
            ("blocked", blockers.join("; "))
        } else {
            ("ready", String::new())
        };
        if none_deps == 0 && unknown_deps > 0 {
            let suffix = format!(
                "{unknown_deps} dependency(ies) not in the library table: check them by hand"
            );
            note = if note.is_empty() {
                suffix
            } else {
                format!("{note}; {suffix}")
            };
        }

        let risk_count = risks.len() as i64;
        let readiness = (100
            - 6 * risk_count
            - (i64::from(lines).saturating_sub(600) / 10)
            - 12 * i64::from(none_deps as u32)
            - 4 * i64::from(unknown_deps as u32))
        .clamp(0, 100);
        let value = (functions.len() as i64 * 8 + i64::from(lines) / 25).min(100);
        let priority = (readiness + value) / 2;

        out.push(Unit {
            module: module
                .get("module")
                .and_then(Json::as_str)
                .unwrap_or("?")
                .to_string(),
            lines,
            functions,
            risks,
            imports,
            entry_point: module
                .get("entry_point")
                .and_then(Json::as_bool)
                .unwrap_or(false),
            status,
            note,
            readiness,
            priority,
            wave: 0,
        });
    }
    // Waves follow imports: a module that imports another local module ports after it.
    let local: Vec<(String, Vec<String>)> = out
        .iter()
        .map(|unit| {
            let package = unit
                .module
                .rsplit_once('.')
                .map(|(parent, _)| parent)
                .unwrap_or("");
            let deps = unit
                .imports
                .iter()
                .filter(|import| {
                    out.iter().any(|other| other.module == **import)
                        || (!package.is_empty()
                            && out
                                .iter()
                                .any(|other| other.module == format!("{package}.{import}")))
                })
                .cloned()
                .collect();
            (unit.module.clone(), deps)
        })
        .collect();
    let mut depths: BTreeMap<String, usize> = BTreeMap::new();
    for _ in 0..local.len() + 1 {
        for (module, deps) in &local {
            let depth = deps
                .iter()
                .filter_map(|dep| depths.get(dep).copied())
                .max()
                .map(|depth| depth + 1)
                .unwrap_or(0);
            let entry = depths.entry(module.clone()).or_insert(0);
            *entry = (*entry).max(depth);
        }
    }
    for unit in &mut out {
        unit.wave = depths.get(&unit.module).copied().unwrap_or(0);
    }
    // Within a wave: the best first port first.
    out.sort_by(|a, b| {
        (a.wave, std::cmp::Reverse(a.priority), a.module.clone()).cmp(&(
            b.wave,
            std::cmp::Reverse(b.priority),
            b.module.clone(),
        ))
    });
    out
}

fn dependency_rows(
    doc: &Json,
    units: &[Unit],
) -> Vec<(String, String, &'static str, String, Vec<String>)> {
    let mut seen: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for unit in units {
        for import in &unit.imports {
            let package = libs::reduce(import).to_string();
            if package.is_empty() || package.starts_with('.') {
                continue;
            }
            seen.entry(package).or_default().push(unit.module.clone());
        }
    }
    for declared in doc
        .get("dependencies")
        .and_then(Json::as_array)
        .unwrap_or_default()
    {
        if let Some(name) = declared.as_str() {
            seen.entry(name.to_string()).or_default();
        }
    }
    let local: Vec<String> = units.iter().map(|unit| unit.module.clone()).collect();
    let is_local = |package: &str| {
        local
            .iter()
            .any(|module| module == package || module.starts_with(&format!("{package}.")))
    };
    let mut rows = Vec::new();
    for (package, mut users) in seen {
        if is_local(&package) {
            continue;
        }
        users.sort();
        users.dedup();
        match libs::find(&package) {
            Some(entry) => rows.push((
                package,
                entry.libs.clone(),
                entry.fit.label(),
                entry.note.clone(),
                users,
            )),
            None => rows.push((package, String::new(), "unknown", String::new(), users)),
        }
    }
    rows.sort_by(|a, b| (a.2 == "unknown", &a.0).cmp(&(b.2 == "unknown", &b.0)));
    rows
}

pub fn render_plan(doc: &Json, repo: &Path, whole_app: bool) -> String {
    let units = units(doc);
    let dependencies = dependency_rows(doc, &units);
    let mut out = String::new();
    out.push_str(&format!("# Port plan for {}\n\n", repo.display()));
    let total_functions: usize = units.iter().map(|unit| unit.functions.len()).sum();
    let language = doc
        .get("language")
        .and_then(Json::as_str)
        .unwrap_or("source");
    out.push_str(&format!(
        "{} {language} unit(s), {} function(s), {} dependency(ies). Port one unit at a time; \
         `bridge`, `record`, `diff` and `gate` are per unit.\n\n",
        units.len(),
        total_functions,
        dependencies.len()
    ));

    if whole_app {
        out.push_str("## What the app is\n\n");
        let shape = app_shape(&units);
        out.push_str(&format!("- shape: **{shape}**\n"));
        let entry: Vec<&Unit> = units.iter().filter(|unit| unit.entry_point).collect();
        if !entry.is_empty() {
            out.push_str(&format!(
                "- entry points: {}\n",
                entry
                    .iter()
                    .map(|unit| format!("`{}`", unit.module))
                    .collect::<Vec<_>>()
                    .join(", ")
            ));
        }
        let servers: Vec<&Unit> = units
            .iter()
            .filter(|unit| {
                unit.imports.iter().any(|import| {
                    matches!(
                        libs::reduce(import),
                        "flask" | "fastapi" | "django" | "starlette" | "uvicorn" | "aiohttp"
                    )
                })
            })
            .collect();
        if !servers.is_empty() {
            out.push_str(&format!(
                "- servers: {}\n",
                servers
                    .iter()
                    .map(|unit| format!("`{}`", unit.module))
                    .collect::<Vec<_>>()
                    .join(", ")
            ));
        }
        let kept: Vec<&Unit> = units.iter().filter(|unit| unit.status == "keep").collect();
        let blocked: Vec<&Unit> = units
            .iter()
            .filter(|unit| unit.status == "blocked")
            .collect();
        out.push_str(&format!(
            "- {} ready, {} kept in Python, {} blocked\n",
            units.iter().filter(|unit| unit.status == "ready").count(),
            kept.len(),
            blocked.len()
        ));
        out.push_str(
            "\nFor a whole app, `app record`, `app replay` and `app front-door` come first: the \
             front door ships with every command answered by the original until a unit is wired.\n",
        );
    }

    out.push_str("\n## Units in port order\n\n");
    out.push_str("| # | wave | unit | lines | functions | readiness | priority | status |\n| ---: | ---: | --- | ---: | ---: | ---: | ---: | --- |\n");
    for (index, unit) in units.iter().enumerate() {
        out.push_str(&format!(
            "| {} | {} | `{}` | {} | {} | {} | {} | {} |\n",
            index + 1,
            unit.wave,
            unit.module,
            unit.lines,
            unit.functions.len(),
            unit.readiness,
            unit.priority,
            unit.status
        ));
    }
    if let Some(first) = units.iter().find(|unit| unit.status == "ready") {
        let lang_flag = match doc.get("language").and_then(Json::as_str) {
            Some("c") => " --lang c",
            Some("cpp") => " --lang cpp",
            _ => "",
        };
        out.push_str(&format!(
            "\nStart here: `cpp-anything bridge {} {}{lang_flag}` then `cpp-anything record <project>`.\n",
            repo.display(),
            first.module
        ));
        if first.entry_point {
            out.push_str(
                "\nThis unit is an entry point: port it together with what it calls, and wire the \
                 command into `cpp/app` when it passes gate.\n",
            );
        }
    } else if units.iter().all(|unit| unit.status == "keep") {
        out.push_str("\nEvery unit is UI code that stays in Python: nothing to port here.\n");
    } else {
        out.push_str("\nNothing is ready: every unit is blocked. See the dependencies table.\n");
    }

    if whole_app {
        out.push_str("\n## Dependencies\n\n| package | C++ library | fit | used by |\n| --- | --- | --- | --- |\n");
        for (package, libs, fit, _note, users) in &dependencies {
            out.push_str(&format!(
                "| `{}` | {} | {} | {} |\n",
                package,
                if libs.is_empty() {
                    "—".to_string()
                } else {
                    format!("`{libs}`")
                },
                fit,
                if users.is_empty() {
                    "declared only".to_string()
                } else {
                    users.iter().take(3).cloned().collect::<Vec<_>>().join(", ")
                }
            ));
        }
        let notes: Vec<&(String, String, &'static str, String, Vec<String>)> = dependencies
            .iter()
            .filter(|row| !row.3.is_empty())
            .collect();
        if !notes.is_empty() {
            out.push_str("\nNotes on the mappings that are not drop-in:\n\n");
            for (package, _, fit, note, _) in notes {
                out.push_str(&format!("- `{package}` ({fit}): {note}\n"));
            }
        }
    }

    out.push_str("\n## Risks per module\n\n");
    for unit in &units {
        if unit.risks.is_empty() && unit.note.is_empty() {
            continue;
        }
        out.push_str(&format!("### `{}`\n\n", unit.module));
        if !unit.note.is_empty() {
            out.push_str(&format!("- status: {}\n", unit.note));
        }
        for risk in &unit.risks {
            out.push_str(&format!("- {risk}\n"));
        }
        out.push('\n');
    }

    out.push_str("\n## Decisions to confirm before porting\n\n");
    out.push_str("1. **Target toolchain and standard**: `cpp-anything matrix features --toolchain gcc --version 13 --std c++23` says what it can express; the default is GCC 13 with C++23.\n");
    out.push_str("2. **Which unit first**: the order above is by dependency wave, then readiness, then value. Porting a lower-priority unit first is fine if the user says so.\n");
    if whole_app {
        out.push_str("3. **The app's shape**: a CLI or server keeps its entry point in `cpp/app`; a library becomes the front door's public API. Confirm which one this is.\n");
        out.push_str("4. **Dependencies marked `none`**: their units are blocked. Decide per package: keep that unit in Python, hand-roll the missing piece, or change the app.\n");
        out.push_str("5. **Dependencies marked `unknown`**: not in `data/libs.json`, so nobody has judged the fit. Check each before porting a unit that uses it.\n");
    } else {
        out.push_str("3. **Badged units**: blocked and keep units are decisions for the user, not the agent.\n");
    }

    out.push_str("\n## What the numbers mean\n\n");
    out.push_str("- **readiness** = 100 - 6 x risks - size over 600 lines - 12 x each `none` dependency - 4 x each unknown dependency, clamped to 0..100.\n");
    out.push_str("- **priority** = the mean of readiness and value, where value = 8 x functions + lines/25 (capped at 100).\n");
    out.push_str("- **wave** = 1 + the deepest wave among the local modules this one imports: port dependencies first.\n");
    out.push_str("- **risks** are heuristics from the analyzer: unicode offsets, int parsing, regex engines, float formatting, integer width, dynamic dispatch. Each one is a place the port can differ while still compiling.\n");
    out.push_str("- **status**: `ready` ports now; `keep` is UI code that stays in Python; `blocked` has a dependency with no C++ equivalent.\n");
    out
}

fn app_shape(units: &[Unit]) -> &'static str {
    let imports_server = units.iter().any(|unit| {
        unit.imports.iter().any(|import| {
            matches!(
                libs::reduce(import),
                "flask" | "fastapi" | "django" | "starlette" | "uvicorn" | "aiohttp" | "bottle"
            )
        })
    });
    if imports_server {
        return "server";
    }
    if units.iter().any(|unit| unit.entry_point) {
        "CLI"
    } else {
        "library"
    }
}

pub fn to_json(doc: &Json, repo: &Path, whole_app: bool) -> Json {
    let mut fields = vec![
        ("schema", Json::str("cpp-anything/analyze@1")),
        ("root", Json::str(repo.display().to_string())),
        (
            "modules",
            doc.get("modules")
                .cloned()
                .unwrap_or(Json::Array(Vec::new())),
        ),
        (
            "dependencies",
            doc.get("dependencies")
                .cloned()
                .unwrap_or(Json::Array(Vec::new())),
        ),
    ];
    if whole_app {
        fields.push(("whole_app", Json::Bool(true)));
    }
    Json::obj(fields)
}

pub fn summarize(doc: &Json) -> String {
    let modules = doc
        .get("modules")
        .and_then(Json::as_array)
        .unwrap_or_default();
    let functions: usize = modules
        .iter()
        .map(|module| {
            module
                .get("functions")
                .and_then(Json::as_array)
                .map(<[Json]>::len)
                .unwrap_or(0)
        })
        .sum();
    format!("{} modules, {} functions", modules.len(), functions)
}

pub fn write_plan(path: &Path, text: &str) -> Result<(), String> {
    util::write(path, text).map_err(|error| format!("{}: {error}", path.display()))
}
