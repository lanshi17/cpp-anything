//! PHP is whole-app only: the analyzer lists top-level functions so `analyze
//! --whole-app` can plan an app, and `bridge`/`record` refuse per-unit ports.

use std::path::Path;

use crate::util;

use super::{Analysis, Found, FoundParam};

pub fn analyze(path: &Path) -> Result<Analysis, String> {
    let source = util::read(path)?;
    let lines = source.lines().count() as u32;
    let mut functions = Vec::new();
    let mut skipped = Vec::new();
    let mut imports = Vec::new();
    let mut depth = 0i32;
    for (index, line) in source.lines().enumerate() {
        let trimmed = line.trim();
        if trimmed.starts_with("require") || trimmed.starts_with("include") {
            imports.push(trimmed.to_string());
        }
        if trimmed.starts_with("//") || trimmed.starts_with('#') {
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix("function ") {
            if depth != 0 {
                skipped.push((
                    rest.split('(').next().unwrap_or("function").to_string(),
                    "a nested or method function: PHP classes are whole-app only".into(),
                ));
                continue;
            }
            match parse_function(rest) {
                Ok((name, params, returns)) => functions.push(Found {
                    name,
                    line: index as u32 + 1,
                    call: format!("php_{}", index),
                    params,
                    returns,
                    doc: String::new(),
                }),
                Err((name, reason)) => skipped.push((name, reason)),
            }
        }
        depth += trimmed.matches('{').count() as i32 - trimmed.matches('}').count() as i32;
        if depth < 0 {
            depth = 0;
        }
    }
    Ok(Analysis {
        functions,
        skipped,
        imports,
        lines,
    })
}

fn parse_function(rest: &str) -> Result<(String, Vec<FoundParam>, String), (String, String)> {
    let (name, after) = rest.split_once('(').ok_or_else(|| {
        (
            "function".to_string(),
            "a function without a parameter list".to_string(),
        )
    })?;
    let name = name.trim().to_string();
    let (params, tail) = split_params(after)?;
    let mut returns = "Any".to_string();
    if let Some(annotation) = tail.trim().strip_prefix(':') {
        let annotation = annotation
            .split(['{', ';'])
            .next()
            .unwrap_or("")
            .trim()
            .trim_start_matches('?');
        returns = php_type(annotation);
    }
    let mut out = Vec::new();
    for param in params {
        let mut text = param.trim();
        if text.starts_with("...") || text.starts_with('&') {
            return Err((
                name,
                "by-reference and variadic parameters have no wire form".into(),
            ));
        }
        text = text.trim_start_matches('&');
        let (ty, rest) = match text.split_once(char::is_whitespace) {
            Some((head, rest))
                if !head.contains('$') && !head.contains('=') && !head.contains('$') =>
            {
                (php_type(head.trim_start_matches('?')), rest.trim())
            }
            _ => ("Any".to_string(), text),
        };
        let (var, default) = match rest.split_once('=') {
            Some((var, value)) => (var.trim(), Some(value.trim().to_string())),
            None => (rest, None),
        };
        let name = var.trim_start_matches('$').trim();
        if name.is_empty()
            || !name
                .chars()
                .next()
                .is_some_and(|c| c.is_ascii_alphabetic() || c == '_')
        {
            return Err((name.to_string(), "a parameter without a plain name".into()));
        }
        out.push(FoundParam {
            name: name.to_string(),
            ty,
            has_default: default.is_some(),
            default: None,
        });
    }
    Ok((name, out, returns))
}

fn split_params<'a>(after: &'a str) -> Result<(Vec<String>, &'a str), (String, String)> {
    let mut params = Vec::new();
    let mut depth = 0i32;
    let mut start = 0usize;
    let bytes = after.as_bytes();
    for (index, ch) in after.char_indices() {
        match ch {
            '(' | '[' => depth += 1,
            ')' | ']' => {
                if depth == 0 && ch == ')' {
                    params.push(after[start..index].to_string());
                    return Ok((params, &after[index + 1..]));
                }
                depth -= 1;
            }
            ',' if depth == 0 => {
                params.push(after[start..index].to_string());
                start = index + 1;
            }
            _ => {}
        }
    }
    let _ = bytes;
    Err(("function".to_string(), "an unclosed parameter list".into()))
}

fn php_type(text: &str) -> String {
    match text.to_ascii_lowercase().as_str() {
        "string" => "str".into(),
        "int" | "integer" => "int".into(),
        "float" | "double" => "float".into(),
        "bool" | "boolean" => "bool".into(),
        "void" => "None".into(),
        other if other.starts_with("array") => "list[str]".into(),
        _ => "Any".into(),
    }
}
