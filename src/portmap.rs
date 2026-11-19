//! `map`: the port map — what is ported, what still is not, which recordings went
//! stale, and where the front door still falls back.

use std::path::{Path, PathBuf};

use crate::json::Json;
use crate::project::Project;
use crate::util;

pub const MAP_FILE: &str = "cpp/PORT_MAP.md";

pub struct Port {
    pub unit: String,
    pub dir: String,
    pub module: String,
    pub language: String,
    pub functions: Vec<String>,
    pub kept: Vec<String>,
    pub cases: usize,
    pub last_diff: Option<(i64, i64)>,
    pub changed_upstream: Option<String>,
}

pub struct Glue {
    pub file: String,
    pub falls_back: bool,
}

pub struct Map {
    pub root: PathBuf,
    pub ports: Vec<Port>,
    pub not_ported: Vec<Json>,
    pub glue: Vec<Glue>,
    pub analyze_error: Option<String>,
}

pub fn build(repo: &Path, extra: &[PathBuf]) -> Result<Map, String> {
    let root = repo
        .canonicalize()
        .map_err(|error| format!("{}: {error}", repo.display()))?;
    let mut ports = Vec::new();
    let mut candidates = walk(&root, 0);
    for directory in extra {
        candidates.extend(walk(directory, 0));
    }
    candidates.sort();
    candidates.dedup();
    for entry in candidates {
        let project_file = entry.join(crate::project::META_DIR).join("project.json");
        if !project_file.is_file() {
            continue;
        }
        let Ok(project) = Project::load(&entry) else {
            continue;
        };
        let cases = util::read(&project.dir.join("cases.jsonl"))
            .map(|text| text.lines().filter(|line| !line.trim().is_empty()).count())
            .unwrap_or(0);
        let history = project
            .dir
            .join(crate::project::META_DIR)
            .join("diff-history.jsonl");
        let last_diff = util::read(&history).ok().and_then(|text| {
            text.lines()
                .filter(|line| !line.trim().is_empty())
                .filter_map(|line| Json::parse(line).ok())
                .next_back()
                .and_then(|doc| {
                    Some((
                        doc.get("passed").and_then(Json::as_i64)?,
                        doc.get("total").and_then(Json::as_i64)?,
                    ))
                })
        });
        ports.push(Port {
            unit: project.unit.clone(),
            dir: entry
                .strip_prefix(&root)
                .unwrap_or(&entry)
                .display()
                .to_string(),
            module: project.module.clone(),
            language: project.source_lang.clone(),
            functions: project
                .functions
                .iter()
                .map(|function| function.name.clone())
                .collect(),
            kept: Vec::new(),
            cases,
            last_diff,
            changed_upstream: crate::recording::stale(&project),
        });
    }
    ports.sort_by(|a, b| (&a.unit, &a.dir).cmp(&(&b.unit, &b.dir)));

    let (not_ported, analyze_error) = match crate::python::available() {
        Some(_) => match crate::python::analyze(&root) {
            Ok(doc) => {
                let mut units = Vec::new();
                for module in doc
                    .get("modules")
                    .and_then(Json::as_array)
                    .unwrap_or_default()
                {
                    let name = module
                        .get("module")
                        .and_then(Json::as_str)
                        .unwrap_or("?")
                        .to_string();
                    if ports.iter().any(|port| port.module == name) {
                        continue;
                    }
                    units.push(Json::obj([
                        ("unit", Json::str(name)),
                        (
                            "lines",
                            Json::Int(module.get("lines").and_then(Json::as_i64).unwrap_or(0)),
                        ),
                        (
                            "exports",
                            Json::Int(
                                module
                                    .get("functions")
                                    .and_then(Json::as_array)
                                    .map(<[Json]>::len)
                                    .unwrap_or(0) as i64,
                            ),
                        ),
                        (
                            "risks",
                            Json::Int(
                                module
                                    .get("risks")
                                    .and_then(Json::as_array)
                                    .map(<[Json]>::len)
                                    .unwrap_or(0) as i64,
                            ),
                        ),
                    ]));
                }
                (units, None)
            }
            Err(error) => (Vec::new(), Some(error)),
        },
        None => (
            Vec::new(),
            Some("python3 is not available: cannot list the units still in Python".into()),
        ),
    };

    let mut glue = Vec::new();
    let glue_dir = root.join("cpp").join("app").join("src");
    if let Ok(entries) = std::fs::read_dir(&glue_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !name.starts_with("glue_") || !name.ends_with(".cpp") {
                continue;
            }
            let text = util::read(&entry.path()).unwrap_or_default();
            glue.push(Glue {
                file: entry
                    .path()
                    .strip_prefix(&root)
                    .unwrap_or(&entry.path())
                    .display()
                    .to_string(),
                falls_back: text.contains("return false"),
            });
        }
    }
    glue.sort_by(|a, b| a.file.cmp(&b.file));

    Ok(Map {
        root,
        ports,
        not_ported,
        glue,
        analyze_error,
    })
}

fn walk(directory: &Path, depth: usize) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if depth > 4 {
        return out;
    }
    let Ok(entries) = std::fs::read_dir(directory) else {
        return out;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        if matches!(
            name.as_str(),
            "build"
                | "node_modules"
                | "target"
                | "vendor"
                | ".git"
                | "__pycache__"
                | ".venv"
                | "venv"
        ) || name.starts_with('.')
        {
            continue;
        }
        out.push(path.clone());
        out.extend(walk(&path, depth + 1));
    }
    out
}

impl Map {
    pub fn changed(&self) -> usize {
        self.ports
            .iter()
            .filter(|port| port.changed_upstream.is_some())
            .count()
    }

    pub fn markdown(&self) -> String {
        let mut out = String::new();
        out.push_str(&format!(
            "# Port map: {}\n\n",
            self.root
                .file_name()
                .map(|name| name.to_string_lossy().into_owned())
                .unwrap_or_default()
        ));
        let sent = if self.changed() > 0 {
            format!(
                "; **{} changed upstream** since they were recorded: re-record them and run `diff`.",
                self.changed()
            )
        } else {
            "; no original changed since it was recorded.".to_string()
        };
        out.push_str(&format!(
            "{} unit(s) have a C++ port{}{}.\n\n",
            self.ports.len(),
            sent,
            if self.not_ported.is_empty() {
                String::new()
            } else {
                format!(", {} still only in Python", self.not_ported.len())
            }
        ));
        out.push_str("## Ports\n\n| Unit | Project | In C++ | Cases | Last diff | Original |\n| --- | --- | --- | ---: | --- | --- |\n");
        for port in &self.ports {
            let functions = if port.functions.is_empty() {
                "-".to_string()
            } else {
                port.functions
                    .iter()
                    .map(|name| format!("`{name}`"))
                    .collect::<Vec<_>>()
                    .join(", ")
            };
            let last_diff = match port.last_diff {
                Some((passed, total)) => format!("{passed}/{total}"),
                None => "not run".to_string(),
            };
            let original = match &port.changed_upstream {
                Some(message) => format!("**changed**: {}", util::truncate_text(message, 80)),
                None => "unchanged".to_string(),
            };
            out.push_str(&format!(
                "| `{}` | `{}` | {} | {} | {} | {} |\n",
                port.unit, port.dir, functions, port.cases, last_diff, original
            ));
        }
        if !self.glue.is_empty() {
            out.push_str("\n## Whole app\n\n| Glue | Runs |\n| --- | --- |\n");
            for glue in &self.glue {
                out.push_str(&format!(
                    "| `{}` | {} |\n",
                    glue.file,
                    if glue.falls_back {
                        "C++, handing some cases to the original"
                    } else {
                        "C++ only"
                    }
                ));
            }
        }
        if !self.not_ported.is_empty() {
            out.push_str("\n## Not ported yet\n\n| Unit | Lines | Exports | Risks |\n| --- | ---: | ---: | ---: |\n");
            for unit in &self.not_ported {
                out.push_str(&format!(
                    "| `{}` | {} | {} | {} |\n",
                    unit.get("unit").and_then(Json::as_str).unwrap_or("?"),
                    unit.get("lines").and_then(Json::as_i64).unwrap_or(0),
                    unit.get("exports").and_then(Json::as_i64).unwrap_or(0),
                    unit.get("risks").and_then(Json::as_i64).unwrap_or(0),
                ));
            }
        }
        if let Some(error) = &self.analyze_error {
            out.push_str(&format!(
                "\nThe rest of the repository could not be analyzed: {error}\n"
            ));
        }
        out.push_str("\nRegenerate with `cpp-anything map --write`. Commit this file: when the original changes, the next `map` says so.\n");
        out
    }

    pub fn to_json(&self) -> Json {
        Json::obj([
            ("schema", Json::str("cpp-anything/port-map@1")),
            ("root", Json::str(self.root.display().to_string())),
            (
                "ports",
                Json::Array(
                    self.ports
                        .iter()
                        .map(|port| {
                            let mut fields = vec![
                                ("unit", Json::str(port.unit.clone())),
                                ("project", Json::str(port.dir.clone())),
                                ("module", Json::str(port.module.clone())),
                                ("language", Json::str(port.language.clone())),
                                (
                                    "in_cpp",
                                    Json::Array(
                                        port.functions
                                            .iter()
                                            .map(|name| Json::str(name.clone()))
                                            .collect(),
                                    ),
                                ),
                                (
                                    "kept_in_original",
                                    Json::Array(
                                        port.kept
                                            .iter()
                                            .map(|name| Json::str(name.clone()))
                                            .collect(),
                                    ),
                                ),
                                ("cases", Json::Int(port.cases as i64)),
                            ];
                            if let Some((passed, total)) = port.last_diff {
                                fields.push((
                                    "last_diff",
                                    Json::Array(vec![Json::Int(passed), Json::Int(total)]),
                                ));
                            }
                            if let Some(message) = &port.changed_upstream {
                                fields.push(("changed_upstream", Json::str(message.clone())));
                            }
                            Json::obj(fields)
                        })
                        .collect(),
                ),
            ),
            ("not_ported", Json::Array(self.not_ported.clone())),
            (
                "glue",
                Json::Array(
                    self.glue
                        .iter()
                        .map(|glue| {
                            Json::obj([
                                ("file", Json::str(glue.file.clone())),
                                ("falls_back", Json::Bool(glue.falls_back)),
                            ])
                        })
                        .collect(),
                ),
            ),
            (
                "analyze_error",
                self.analyze_error
                    .as_ref()
                    .map(|error| Json::str(error.clone()))
                    .unwrap_or(Json::Null),
            ),
        ])
    }
}
