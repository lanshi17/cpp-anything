//! The library table: Python package -> C++ library, with an honest fit.
//!
//! Hand-curated on purpose: a guessed mapping is worse than an admitted unknown.
//! `fit = none` means there is no equivalent worth handing to a port, and that
//! blocks the unit instead of silently hand-rolling one.

use std::sync::LazyLock;

use crate::json::Json;

pub struct Entry {
    pub names: Vec<String>,
    pub libs: String,
    pub fit: Fit,
    pub note: String,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Fit {
    Good,
    Partial,
    None,
    /// The unit stays in the original language (UI): port what it calls instead.
    Kept,
}

impl Fit {
    pub fn label(self) -> &'static str {
        match self {
            Fit::Good => "good",
            Fit::Partial => "partial",
            Fit::None => "none",
            Fit::Kept => "kept",
        }
    }
}

const RAW: &str = include_str!("../data/libs.json");

static TABLE: LazyLock<Vec<Entry>> = LazyLock::new(|| {
    let doc = Json::parse(RAW).expect("data/libs.json is not valid JSON");
    let mut out = Vec::new();
    for item in doc
        .get("python")
        .and_then(Json::as_array)
        .unwrap_or_default()
    {
        let strings = |key: &str| -> Vec<String> {
            item.get(key)
                .and_then(Json::as_array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(|value| value.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default()
        };
        let fit = match item.get("fit").and_then(Json::as_str) {
            Some("good") => Fit::Good,
            Some("none") => Fit::None,
            Some("kept") => Fit::Kept,
            _ => Fit::Partial,
        };
        out.push(Entry {
            names: strings("names"),
            libs: item
                .get("libs")
                .and_then(Json::as_str)
                .unwrap_or("")
                .to_string(),
            fit,
            note: item
                .get("note")
                .and_then(Json::as_str)
                .unwrap_or("")
                .to_string(),
        });
    }
    out
});

pub fn find(package: &str) -> Option<&'static Entry> {
    TABLE
        .iter()
        .find(|entry| entry.names.iter().any(|name| name == package))
}

/// `@scope/name/sub` -> `@scope/name`, `pkg.module` -> `pkg`: the part a table entry
/// can name.
pub fn reduce(import: &str) -> &str {
    let import = import.trim();
    if let Some(rest) = import.strip_prefix('@') {
        let mut parts = rest.splitn(3, '/');
        let scope = parts.next().unwrap_or("");
        let name = parts.next().unwrap_or("");
        if name.is_empty() {
            return import;
        }
        return if scope.is_empty() {
            import
        } else {
            import.get(..scope.len() + 1 + name.len()).unwrap_or(import)
        };
    }
    import.split('.').next().unwrap_or(import)
}
