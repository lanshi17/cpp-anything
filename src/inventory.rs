//! What the probe found in a unit: functions, types, defaults, risks.

use std::path::PathBuf;

use crate::json::Json;
use crate::types::Ty;

#[derive(Clone, Debug)]
pub struct Param {
    pub name: String,
    pub annotation: String,
    pub ty: Ty,
    pub default: Option<Json>,
    pub has_default: bool,
    pub keyword_only: bool,
}

#[derive(Clone, Debug)]
pub struct Function {
    pub name: String,
    pub line: u32,
    pub params: Vec<Param>,
    pub returns: Ty,
    pub returns_annotation: String,
    pub doc: String,
    pub risks: Vec<String>,
    /// The original raises somewhere in this function: a port must too, and a
    /// recorded failing case must exist.
    pub raises: bool,
}

#[derive(Clone, Debug)]
pub struct Unit {
    pub unit: String,
    pub module: String,
    pub file: PathBuf,
    pub package_root: Option<PathBuf>,
    pub lines: u32,
    pub functions: Vec<Function>,
    pub skipped: Vec<(String, String)>,
    pub risks: Vec<String>,
    pub imports: Vec<String>,
}

impl Function {
    pub fn parse(item: &Json) -> Result<Function, String> {
        let mut params = Vec::new();
        for param in item
            .get("params")
            .and_then(Json::as_array)
            .unwrap_or_default()
        {
            let annotation = param
                .get("annotation")
                .and_then(Json::as_str)
                .unwrap_or("")
                .to_string();
            params.push(Param {
                name: param
                    .get("name")
                    .and_then(Json::as_str)
                    .unwrap_or("arg")
                    .to_string(),
                ty: Ty::parse(&annotation),
                annotation,
                default: param
                    .get("default")
                    .cloned()
                    .filter(|value| !value.is_null()),
                has_default: param
                    .get("has_default")
                    .and_then(Json::as_bool)
                    .unwrap_or(false),
                keyword_only: param
                    .get("keyword_only")
                    .and_then(Json::as_bool)
                    .unwrap_or(false),
            });
        }
        let returns_annotation = item
            .get("returns")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        Ok(Function {
            name: item
                .get("name")
                .and_then(Json::as_str)
                .ok_or("function without a name")?
                .to_string(),
            line: item.get("line").and_then(Json::as_i64).unwrap_or(0) as u32,
            params,
            returns: Ty::parse(&returns_annotation),
            returns_annotation,
            doc: first_line(item.get("doc")),
            risks: strings(item.get("risks")),
            raises: item.get("raises").and_then(Json::as_bool).unwrap_or(false),
        })
    }

    pub fn to_json(&self) -> Json {
        let params = self
            .params
            .iter()
            .map(|param| {
                Json::obj([
                    ("name", Json::str(param.name.clone())),
                    ("annotation", Json::str(param.annotation.clone())),
                    ("default", param.default.clone().unwrap_or(Json::Null)),
                    ("has_default", Json::Bool(param.has_default)),
                    ("keyword_only", Json::Bool(param.keyword_only)),
                ])
            })
            .collect::<Vec<_>>();
        Json::obj([
            ("name", Json::str(self.name.clone())),
            ("line", Json::Int(i64::from(self.line))),
            ("params", Json::Array(params)),
            ("returns", Json::str(self.returns_annotation.clone())),
            ("raises", Json::Bool(self.raises)),
            (
                "doc",
                Json::Array(
                    self.doc
                        .lines()
                        .map(|line| Json::str(line.to_string()))
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

    /// `[[nodiscard]] std::expected<std::string, PortError> slug(std::string_view text);`
    pub fn cpp_declaration(&self) -> String {
        format!("{};", self.cpp_signature())
    }

    /// The definition's opening line: same types, no default arguments.
    pub fn cpp_definition(&self) -> String {
        self.cpp_signature_with(false)
    }

    /// The declaration, without the semicolon.
    pub fn cpp_signature(&self) -> String {
        self.cpp_signature_with(true)
    }

    fn cpp_signature_with(&self, with_defaults: bool) -> String {
        let mut params = Vec::new();
        for param in &self.params {
            let ty = param.ty.cpp_param(true);
            let mut default = String::new();
            if param.has_default && with_defaults {
                if let Some(value) = param
                    .default
                    .as_ref()
                    .and_then(|value| param.ty.cpp_default(value))
                {
                    default = format!(" = {value}");
                }
            }
            params.push(format!("{ty} {}{default}", param.name));
        }
        format!(
            "[[nodiscard]] cppan::Result<{}> {}({})",
            self.returns.cpp_return(),
            self.name,
            params.join(", ")
        )
    }

    /// `std::string slug(std::string_view text)`: the reader-facing signature,
    /// without the `cppan::Result` wrapper and without defaults.
    pub fn plain_signature(&self) -> String {
        let params = self
            .params
            .iter()
            .map(|param| format!("{} {}", param.ty.cpp_param(true), param.name))
            .collect::<Vec<_>>()
            .join(", ");
        format!("{} {}({})", self.returns.cpp_return(), self.name, params)
    }

    /// `slug(text: str) -> str`, the Python side of the same function.
    pub fn signature_line(&self) -> String {
        let params = self
            .params
            .iter()
            .map(|param| {
                let default = if param.has_default { " = …" } else { "" };
                format!("{}: {}{}", param.name, param.ty.label(), default)
            })
            .collect::<Vec<_>>()
            .join(", ");
        format!("{}({}) -> {}", self.name, params, self.returns.label())
    }
}

impl Unit {
    pub fn parse(doc: &Json) -> Result<Unit, String> {
        let text = |key: &str| -> Result<String, String> {
            Ok(doc
                .get(key)
                .and_then(Json::as_str)
                .ok_or_else(|| format!("inventory is missing {key}"))?
                .to_string())
        };
        let file = text("file")?;
        let mut functions = Vec::new();
        for item in doc
            .get("functions")
            .and_then(Json::as_array)
            .ok_or("inventory is missing functions")?
        {
            functions.push(Function::parse(item)?);
        }
        let mut skipped = Vec::new();
        for item in doc
            .get("skipped")
            .and_then(Json::as_array)
            .unwrap_or_default()
        {
            skipped.push((
                item.get("name")
                    .and_then(Json::as_str)
                    .unwrap_or("?")
                    .to_string(),
                item.get("reason")
                    .and_then(Json::as_str)
                    .unwrap_or("?")
                    .to_string(),
            ));
        }
        Ok(Unit {
            unit: text("unit")?,
            module: text("module")?,
            file: PathBuf::from(file),
            package_root: doc
                .get("package_root")
                .and_then(Json::as_str)
                .filter(|text| !text.is_empty())
                .map(PathBuf::from),
            lines: doc.get("lines").and_then(Json::as_i64).unwrap_or(0) as u32,
            functions,
            skipped,
            risks: strings(doc.get("risks")),
            imports: strings(doc.get("imports")),
        })
    }
}

fn first_line(value: Option<&Json>) -> String {
    value
        .and_then(Json::as_array)
        .and_then(|lines| lines.first())
        .and_then(Json::as_str)
        .unwrap_or("")
        .to_string()
}

fn strings(value: Option<&Json>) -> Vec<String> {
    value
        .and_then(Json::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}
