//! Project metadata: `.cppan/project.json` is the record of what was bridged.

use std::path::{Path, PathBuf};

use crate::inventory::{Function, Unit};
use crate::json::Json;
use crate::util;

pub const SCHEMA: &str = "cpp-anything/project@1";
pub const META_DIR: &str = ".cppan";
pub const META_FILE: &str = "project.json";

#[derive(Clone, Debug)]
pub struct Target {
    pub toolchain: String,
    pub version: String,
    pub std: String,
}

impl Default for Target {
    fn default() -> Self {
        // gcc 13 is the oldest toolchain with complete C++20 library support and
        // `-std=c++23`; it is the safest default for a C++20/23 port.
        Target {
            toolchain: "gcc".into(),
            version: "13".into(),
            std: "c++23".into(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct Recorded {
    pub cases: usize,
    pub at: String,
}

#[derive(Clone, Debug)]
pub struct Project {
    pub unit: String,
    pub module: String,
    pub source_file: PathBuf,
    pub package_root: Option<PathBuf>,
    pub source_lang: String,
    pub target: Target,
    pub functions: Vec<Function>,
    pub skipped: Vec<(String, String)>,
    pub risks: Vec<String>,
    pub imports: Vec<String>,
    pub lines: u32,
    pub created_at: String,
    pub recorded: Option<Recorded>,
    /// Saved by `record --float-tolerance`: what `diff` and `gate` compare with.
    pub tolerance: f64,
    /// `--batch function:param`: functions answered over many items in one call.
    pub batches: Vec<Batch>,
    /// Generate a C++20 module (`export module <ns>;`) instead of a header.
    pub modules: bool,
    /// TS/JS recording: extra `node` arguments, where npm packages resolve from,
    /// and the built module to import instead of the source file.
    pub node_args: Vec<String>,
    pub resolve_from: Option<String>,
    pub source_module: Option<String>,
    pub dir: PathBuf,
}

#[derive(Clone, Debug, Default)]
pub struct Batch {
    pub function: String,
    pub param: String,
}

impl Project {
    pub fn from_unit(unit: &Unit, target: Target, dir: PathBuf) -> Project {
        Project {
            unit: unit.unit.clone(),
            module: unit.module.clone(),
            source_file: unit.file.clone(),
            package_root: unit.package_root.clone(),
            source_lang: "python".into(),
            target,
            functions: unit.functions.clone(),
            skipped: unit.skipped.clone(),
            risks: unit.risks.clone(),
            imports: unit.imports.clone(),
            lines: unit.lines,
            created_at: util::now_iso8601(),
            recorded: None,
            tolerance: 0.0,
            batches: Vec::new(),
            modules: false,
            node_args: Vec::new(),
            resolve_from: None,
            source_module: None,
            dir,
        }
    }

    pub fn load(dir: &Path) -> Result<Project, String> {
        let path = dir.join(META_DIR).join(META_FILE);
        let text = util::read(&path).map_err(|error| {
            format!(
                "{}: {error} (is this a cpp-anything project?)",
                path.display()
            )
        })?;
        let doc = Json::parse(&text)?;
        let dir_absolute = dir.canonicalize().unwrap_or_else(|_| dir.to_path_buf());
        let unit_doc = doc.get("unit").cloned().unwrap_or(Json::Null);
        let mut functions = Vec::new();
        for item in doc
            .get("functions")
            .and_then(Json::as_array)
            .unwrap_or_default()
        {
            functions.push(crate::inventory::Function::parse(item)?);
        }
        let target = doc.get("target").cloned().unwrap_or(Json::Null);
        let text_of = |value: &Json, key: &str| {
            value
                .get(key)
                .and_then(Json::as_str)
                .unwrap_or_default()
                .to_string()
        };
        Ok(Project {
            unit: text_of(&unit_doc, "name"),
            module: text_of(&unit_doc, "module"),
            source_file: {
                // Relative paths are stored relative to the project directory, so a
                // fixture or a moved checkout still describes its source.
                let file = PathBuf::from(text_of(&unit_doc, "file"));
                let file = if file.is_absolute() {
                    file
                } else {
                    dir_absolute.join(file)
                };
                file.canonicalize().unwrap_or(file)
            },
            package_root: {
                let root = text_of(&unit_doc, "package_root");
                (!root.is_empty()).then(|| {
                    let path = PathBuf::from(root);
                    let path = if path.is_absolute() {
                        path
                    } else {
                        dir_absolute.join(path)
                    };
                    path.canonicalize().unwrap_or(path)
                })
            },
            source_lang: text_of(&unit_doc, "lang"),
            target: Target {
                toolchain: text_of(&target, "toolchain"),
                version: text_of(&target, "version"),
                std: text_of(&target, "std"),
            },
            functions,
            skipped: Vec::new(),
            risks: Vec::new(),
            imports: Vec::new(),
            lines: unit_doc.get("lines").and_then(Json::as_i64).unwrap_or(0) as u32,
            created_at: text_of(&doc, "created_at"),
            recorded: doc.get("recorded").map(|recorded| Recorded {
                cases: recorded.get("cases").and_then(Json::as_i64).unwrap_or(0) as usize,
                at: text_of(recorded, "at"),
            }),
            tolerance: doc
                .get("float_tolerance")
                .and_then(Json::as_f64)
                .unwrap_or(0.0),
            modules: doc.get("modules").and_then(Json::as_bool).unwrap_or(false),
            node_args: doc
                .get("node_args")
                .and_then(Json::as_array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(|item| item.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default(),
            resolve_from: doc
                .get("resolve_from")
                .and_then(Json::as_str)
                .map(str::to_string),
            source_module: doc
                .get("source_module")
                .and_then(Json::as_str)
                .map(str::to_string),
            batches: doc
                .get("batches")
                .and_then(Json::as_array)
                .map(|items| {
                    items
                        .iter()
                        .map(|item| Batch {
                            function: item
                                .get("function")
                                .and_then(Json::as_str)
                                .unwrap_or("")
                                .to_string(),
                            param: item
                                .get("param")
                                .and_then(Json::as_str)
                                .unwrap_or("")
                                .to_string(),
                        })
                        .collect()
                })
                .unwrap_or_default(),
            dir: dir.canonicalize().unwrap_or_else(|_| dir.to_path_buf()),
        })
    }

    pub fn save(&self) -> Result<(), String> {
        let path = self.dir.join(META_DIR).join(META_FILE);
        util::write(&path, &self.to_json().to_string_pretty())
            .map_err(|error| format!("{}: {error}", path.display()))
    }

    pub fn to_json(&self) -> Json {
        // Paths are stored relative to the project directory: a fixture or a moved
        // checkout still describes its source, and `load` resolves them back.
        let stored = |path: &Path| -> String {
            util::relative_path(&self.dir, path)
                .unwrap_or_else(|| path.to_path_buf())
                .display()
                .to_string()
        };
        let skipped = self
            .skipped
            .iter()
            .map(|(name, reason)| {
                Json::obj([
                    ("name", Json::str(name.clone())),
                    ("reason", Json::str(reason.clone())),
                ])
            })
            .collect::<Vec<_>>();
        Json::obj([
            ("schema", Json::str(SCHEMA)),
            ("created_at", Json::str(self.created_at.clone())),
            (
                "unit",
                Json::obj([
                    ("name", Json::str(self.unit.clone())),
                    ("module", Json::str(self.module.clone())),
                    ("file", Json::str(stored(&self.source_file))),
                    (
                        "package_root",
                        self.package_root
                            .as_ref()
                            .map(|path| Json::str(stored(path)))
                            .unwrap_or(Json::Null),
                    ),
                    ("lang", Json::str(self.source_lang.clone())),
                    ("lines", Json::Int(i64::from(self.lines))),
                ]),
            ),
            (
                "target",
                Json::obj([
                    ("toolchain", Json::str(self.target.toolchain.clone())),
                    ("version", Json::str(self.target.version.clone())),
                    ("std", Json::str(self.target.std.clone())),
                ]),
            ),
            ("float_tolerance", Json::Real(self.tolerance)),
            ("modules", Json::Bool(self.modules)),
            (
                "node_args",
                Json::Array(
                    self.node_args
                        .iter()
                        .map(|arg| Json::str(arg.clone()))
                        .collect(),
                ),
            ),
            (
                "resolve_from",
                self.resolve_from
                    .as_ref()
                    .map_or(Json::Null, |path| Json::str(path.clone())),
            ),
            (
                "source_module",
                self.source_module
                    .as_ref()
                    .map_or(Json::Null, |path| Json::str(path.clone())),
            ),
            (
                "batches",
                Json::Array(
                    self.batches
                        .iter()
                        .map(|batch| {
                            Json::obj([
                                ("function", Json::str(batch.function.clone())),
                                ("param", Json::str(batch.param.clone())),
                            ])
                        })
                        .collect(),
                ),
            ),
            (
                "functions",
                Json::Array(self.functions.iter().map(Function::to_json).collect()),
            ),
            ("skipped", Json::Array(skipped)),
            (
                "imports",
                Json::Array(
                    self.imports
                        .iter()
                        .map(|name| Json::str(name.clone()))
                        .collect(),
                ),
            ),
            (
                "risks",
                Json::Array(
                    self.risks
                        .iter()
                        .map(|risk| Json::str(risk.clone()))
                        .collect(),
                ),
            ),
        ])
    }
}
