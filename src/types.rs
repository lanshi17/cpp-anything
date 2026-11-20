//! The type model: Python annotations -> C++20/23 signatures.
//!
//! One dialect, `Ty`, is the contract between the analyzer, the code generator
//! and the differ. It maps onto C++ the way a person would write it now:
//! `string_view` in, `std::string` out, `std::span` for read-only sequences,
//! `std::expected<T, PortError>` for anything that can fail.

use crate::json::Json;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Ty {
    Str,
    Int,
    Float,
    Bool,
    None,
    List(Box<Ty>),
    Dict(Box<Ty>),
    Tuple(Vec<Ty>),
    Optional(Box<Ty>),
    /// No annotation, or one the model does not cover: crosses as JSON.
    Any,
}

impl Ty {
    /// Parses the string form the analyzer emits: `str`, `list[int]`,
    /// `dict[str, float]`, `tuple[int, str]`, `str | None`, `Optional[str]`.
    pub fn parse(text: &str) -> Ty {
        let text = text.trim();
        if text.is_empty() || text == "Any" || text == "object" || text == "..." {
            return Ty::Any;
        }
        if let Some(inner) = strip_generic(text, "Optional") {
            return Ty::Optional(Box::new(Ty::parse(inner)));
        }
        if let Some((left, right)) = split_union(text) {
            if right == "None" || left == "None" {
                let inner = if right == "None" { left } else { right };
                return Ty::Optional(Box::new(Ty::parse(inner)));
            }
            return Ty::Any;
        }
        if let Some(inner) = strip_generic(text, "list") {
            return Ty::List(Box::new(Ty::parse(inner)));
        }
        if let Some(inner) = strip_generic(text, "Sequence") {
            return Ty::List(Box::new(Ty::parse(inner)));
        }
        if let Some(inner) = strip_generic(text, "Iterable") {
            return Ty::List(Box::new(Ty::parse(inner)));
        }
        if let Some(inner) = strip_generic(text, "set") {
            return Ty::List(Box::new(Ty::parse(inner)));
        }
        if let Some(inner) = strip_generic(text, "dict") {
            let value = split_top_level(inner, ',')
                .into_iter()
                .nth(1)
                .unwrap_or(inner);
            return Ty::Dict(Box::new(Ty::parse(value)));
        }
        if let Some(inner) = strip_generic(text, "tuple") {
            let items = split_top_level(inner, ',')
                .into_iter()
                .filter(|item| !item.trim().is_empty() && item.trim() != "...")
                .map(Ty::parse)
                .collect::<Vec<_>>();
            return Ty::Tuple(items);
        }
        match text {
            "str" => Ty::Str,
            "int" => Ty::Int,
            "float" => Ty::Float,
            "bool" => Ty::Bool,
            "None" | "NoneType" => Ty::None,
            "bytes" | "bytearray" => Ty::Any,
            _ => Ty::Any,
        }
    }

    /// Parameter type in the port's header. `top_level` lets the outer list be a
    /// `std::span` while nested ones stay owning vectors.
    pub fn cpp_param(&self, top_level: bool) -> String {
        match self {
            Ty::Str => "std::string_view".to_string(),
            Ty::Int => "std::int64_t".to_string(),
            Ty::Float => "double".to_string(),
            Ty::Bool => "bool".to_string(),
            Ty::None => "const cppan::Json&".to_string(),
            Ty::List(inner) if top_level => {
                format!("std::span<const {}>", inner.cpp_value())
            }
            Ty::List(inner) => format!("const std::vector<{}>&", inner.cpp_param(false)),
            Ty::Dict(value) => {
                format!("const std::map<std::string, {}>&", value.cpp_param(false))
            }
            Ty::Tuple(items) => format!(
                "const std::tuple<{}>&",
                items
                    .iter()
                    .map(|item| item.cpp_param(false))
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
            Ty::Optional(inner) => format!("std::optional<{}>", inner.cpp_param(false)),
            Ty::Any => "const cppan::Json&".to_string(),
        }
    }

    /// The owning type for a value of this type: what a `std::span` element or a
    /// decoded argument is stored in.
    pub fn cpp_value(&self) -> String {
        match self {
            Ty::List(inner) => format!("std::vector<{}>", inner.cpp_value()),
            Ty::Dict(value) => format!("std::map<std::string, {}>", value.cpp_value()),
            Ty::Tuple(items) => format!(
                "std::tuple<{}>",
                items
                    .iter()
                    .map(Ty::cpp_value)
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
            Ty::Optional(inner) => format!("std::optional<{}>", inner.cpp_value()),
            other => other.cpp_param(false),
        }
    }

    /// Type the dispatch decodes into. Identical to the parameter type except for
    /// sequences, which need storage the port's span can point at.
    pub fn cpp_storage(&self) -> String {
        match self {
            Ty::List(inner) => format!("std::vector<{}>", inner.cpp_value()),
            Ty::Any => "cppan::Json".to_string(),
            other => other.cpp_param(false),
        }
    }

    /// Return type inside `std::expected<_, PortError>`.
    pub fn cpp_return(&self) -> String {
        match self {
            Ty::Str => "std::string".to_string(),
            Ty::Int => "std::int64_t".to_string(),
            Ty::Float => "double".to_string(),
            Ty::Bool => "bool".to_string(),
            Ty::None => "cppan::Json".to_string(),
            Ty::List(inner) => format!("std::vector<{}>", inner.cpp_return()),
            Ty::Dict(value) => format!("std::map<std::string, {}>", value.cpp_return()),
            Ty::Tuple(items) => format!(
                "std::tuple<{}>",
                items
                    .iter()
                    .map(Ty::cpp_return)
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
            Ty::Optional(inner) => format!("std::optional<{}>", inner.cpp_return()),
            Ty::Any => "cppan::Json".to_string(),
        }
    }

    /// How the analyzer labels the type in a report.
    pub fn label(&self) -> String {
        match self {
            Ty::Str => "str".into(),
            Ty::Int => "int".into(),
            Ty::Float => "float".into(),
            Ty::Bool => "bool".into(),
            Ty::None => "None".into(),
            Ty::List(inner) => format!("list[{}]", inner.label()),
            Ty::Dict(value) => format!("dict[str, {}]", value.label()),
            Ty::Tuple(items) => format!(
                "tuple[{}]",
                items.iter().map(Ty::label).collect::<Vec<_>>().join(", ")
            ),
            Ty::Optional(inner) => format!("Optional[{}]", inner.label()),
            Ty::Any => "Any".into(),
        }
    }

    /// Headers a generated file needs for this type.
    pub fn headers(&self, out: &mut Vec<String>) {
        let mut need = |header: &str| {
            if !out.iter().any(|existing| existing == header) {
                out.push(header.to_string());
            }
        };
        match self {
            Ty::Str => need("<string_view>"),
            Ty::Int => need("<cstdint>"),
            Ty::Float => need("<cmath>"),
            Ty::Bool => {}
            Ty::None => {}
            Ty::List(inner) => {
                need("<span>");
                need("<vector>");
                inner.headers(out);
            }
            Ty::Dict(value) => {
                need("<map>");
                need("<string>");
                value.headers(out);
            }
            Ty::Tuple(items) => {
                need("<tuple>");
                for item in items {
                    item.headers(out);
                }
            }
            Ty::Optional(inner) => {
                need("<optional>");
                inner.headers(out);
            }
            Ty::Any => {}
        }
    }

    /// A C++ expression for a Python default value.
    pub fn cpp_default(&self, value: &Json) -> Option<String> {
        match (self, value) {
            (_, Json::Null) => Some("std::nullopt".to_string()),
            (Ty::Int, Json::Int(number)) => Some(number.to_string()),
            (Ty::Float, Json::Real(number)) => Some(crate::json::format_real(*number)),
            (Ty::Float, Json::Int(number)) => Some(format!("{number}.0")),
            (Ty::Bool, Json::Bool(flag)) => Some(flag.to_string()),
            (Ty::Str, Json::Str(text)) => Some(cpp_string_literal(text)),
            (Ty::Optional(inner), other) => inner.cpp_default(other),
            (Ty::List(_), Json::Array(items)) if items.is_empty() => Some("{}".to_string()),
            (Ty::Dict(_), Json::Object(pairs)) if pairs.is_empty() => Some("{}".to_string()),
            _ => None,
        }
    }
}

pub fn cpp_string_literal(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            ch if (ch as u32) < 0x20 => out.push_str(&format!("\\x{:02x}", ch as u32)),
            // UTF-8 stays as UTF-8: the compiler is told the source is UTF-8.
            ch => out.push(ch),
        }
    }
    out.push('"');
    out
}

fn strip_generic<'a>(text: &'a str, name: &str) -> Option<&'a str> {
    let prefix = format!("{name}[");
    if text.starts_with(&prefix) && text.ends_with(']') {
        Some(&text[prefix.len()..text.len() - 1])
    } else {
        None
    }
}

fn split_union(text: &str) -> Option<(&str, &str)> {
    let parts = split_top_level(text, '|');
    if parts.len() == 2 {
        Some((parts[0].trim(), parts[1].trim()))
    } else {
        None
    }
}

pub fn split_top_level(text: &str, separator: char) -> Vec<&str> {
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut start = 0;
    for (index, ch) in text.char_indices() {
        match ch {
            '[' | '(' | '{' => depth += 1,
            ']' | ')' | '}' => depth -= 1,
            ch if ch == separator && depth == 0 => {
                out.push(&text[start..index]);
                start = index + ch.len_utf8();
            }
            _ => {}
        }
    }
    out.push(&text[start..]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_annotations_to_cpp() {
        assert_eq!(Ty::parse("str").cpp_param(true), "std::string_view");
        assert_eq!(
            Ty::parse("list[str]").cpp_param(true),
            "std::span<const std::string_view>"
        );
        assert_eq!(
            Ty::parse("list[str]").cpp_return(),
            "std::vector<std::string>"
        );
        assert_eq!(
            Ty::parse("dict[str, int]").cpp_param(true),
            "const std::map<std::string, std::int64_t>&"
        );
        assert_eq!(
            Ty::parse("int | None").cpp_param(true),
            "std::optional<std::int64_t>"
        );
        assert_eq!(Ty::parse("Optional[int]").label(), "Optional[int]");
        assert_eq!(
            Ty::parse("list[list[int]]").cpp_param(true),
            "std::span<const std::vector<std::int64_t>>"
        );
        assert_eq!(
            Ty::parse("Sequence[str]").cpp_return(),
            "std::vector<std::string>"
        );
        assert_eq!(Ty::parse("mystery").label(), "Any");
    }

    #[test]
    fn renders_string_literals() {
        assert_eq!(cpp_string_literal("a\"b\\c\n"), "\"a\\\"b\\\\c\\n\"");
        assert_eq!(cpp_string_literal("café"), "\"café\"");
    }
}
