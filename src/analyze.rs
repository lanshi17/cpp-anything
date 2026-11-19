//! `analyze`: what is in the repository, what it costs to port, and where to start.

use std::path::{Path, PathBuf};

use crate::json::Json;
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

    let package_root = file
        .strip_prefix(&base)
        .ok()
        .map(|_| base.clone())
        .or_else(|| Some(base.clone()));
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

pub fn render_plan(doc: &Json, repo: &Path) -> String {
    let modules = doc
        .get("modules")
        .and_then(Json::as_array)
        .unwrap_or_default();
    let mut rows: Vec<(String, u32, usize, usize, usize)> = modules
        .iter()
        .map(|module| {
            let name = module
                .get("module")
                .and_then(Json::as_str)
                .unwrap_or("?")
                .to_string();
            let lines = module.get("lines").and_then(Json::as_i64).unwrap_or(0) as u32;
            let functions = module
                .get("functions")
                .and_then(Json::as_array)
                .unwrap_or_default();
            let typed = functions
                .iter()
                .map(|function| function.get("typed").and_then(Json::as_i64).unwrap_or(0) as usize)
                .sum::<usize>();
            let params = functions
                .iter()
                .map(|function| function.get("params").and_then(Json::as_i64).unwrap_or(0) as usize)
                .sum::<usize>();
            let risks = module
                .get("risks")
                .and_then(Json::as_array)
                .map(<[Json]>::len)
                .unwrap_or(0);
            let _ = (typed, params);
            (name, lines, functions.len(), risks, params)
        })
        .collect();
    // Start with units that are big enough to matter and low on risks.
    rows.sort_by_key(|row| (row.3, std::cmp::Reverse(row.2)));

    let mut out = String::new();
    out.push_str(&format!("# Port plan for {}\n\n", repo.display()));
    out.push_str(&format!(
        "{} Python modules hold {} functions. Port one module at a time, lowest risk first; \
         `bridge`, `record`, `diff` and `gate` are per unit.\n\n",
        rows.len(),
        rows.iter().map(|row| row.2).sum::<usize>()
    ));
    out.push_str(
        "| # | module | lines | functions | risks |\n| ---: | --- | ---: | ---: | ---: |\n",
    );
    for (index, (name, lines, functions, risks, _)) in rows.iter().enumerate() {
        out.push_str(&format!(
            "| {} | `{}` | {} | {} | {} |\n",
            index + 1,
            name,
            lines,
            functions,
            risks
        ));
    }
    if let Some((name, _, functions, _, _)) = rows.first() {
        out.push_str(&format!(
            "\nStart here: `cpp-anything bridge {} {}` then `cpp-anything record <project>`.\n",
            repo.display(),
            name
        ));
        let _ = functions;
    }

    out.push_str("\n## Risks per module\n\n");
    for module in modules {
        let name = module.get("module").and_then(Json::as_str).unwrap_or("?");
        let risks = module
            .get("risks")
            .and_then(Json::as_array)
            .unwrap_or_default();
        if risks.is_empty() {
            continue;
        }
        out.push_str(&format!("### `{name}`\n\n"));
        for risk in risks {
            if let Some(text) = risk.as_str() {
                out.push_str(&format!("- {text}\n"));
            }
        }
        out.push('\n');
    }
    out.push_str("\n## What the numbers mean\n\n");
    out.push_str("- **risks** are heuristics from the analyzer: unicode offsets, int parsing, regex engines, float formatting, integer width. Each one is a place the port can differ while still compiling.\n");
    out.push_str("- **functions** counts module-level functions with a plain signature; decorated, async and `*args` functions are reported by `cpp-anything bridge` as skipped.\n");
    out.push_str("- Read the unit's risks at their file:line before writing C++ for it; the same list is copied into the generated project's README.\n");
    out
}

pub fn to_json(doc: &Json, repo: &Path) -> Json {
    Json::obj([
        ("schema", Json::str("cpp-anything/analyze@1")),
        ("root", Json::str(repo.display().to_string())),
        (
            "modules",
            doc.get("modules")
                .cloned()
                .unwrap_or(Json::Array(Vec::new())),
        ),
    ])
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
