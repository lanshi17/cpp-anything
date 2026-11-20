//! C# is whole-app only: the analyzer lists public static methods so
//! `analyze --whole-app` can plan an app; `bridge`/`record` refuse per-unit ports.

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
        if trimmed.starts_with("using ") && trimmed.ends_with(';') {
            imports.push(
                trimmed
                    .trim_start_matches("using ")
                    .trim_end_matches(';')
                    .to_string(),
            );
            continue;
        }
        if trimmed.starts_with("//") || trimmed.starts_with('[') {
            continue;
        }
        if depth == 1 && (trimmed.starts_with("public static ") || trimmed.starts_with("static ")) {
            match parse_method(trimmed) {
                Ok(Some((name, params, returns))) => functions.push(Found {
                    name,
                    line: index as u32 + 1,
                    call: format!("csharp_{}", index),
                    params,
                    returns,
                    doc: String::new(),
                }),
                Ok(None) => {}
                Err((name, reason)) => skipped.push((name, reason)),
            }
        } else if depth == 1 {
            if let Some(name) = class_method(trimmed) {
                skipped.push((
                    name,
                    "an instance method: the recorder calls public static methods only".into(),
                ));
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

fn class_method(line: &str) -> Option<String> {
    if !line.contains('(') || line.contains(" class ") || line.starts_with("class ") {
        return None;
    }
    let head = line.split('(').next()?.trim();
    let name = head.split_whitespace().last()?;
    (line.trim_end().ends_with('{') || line.contains('{')).then(|| name.to_string())
}

fn parse_method(line: &str) -> Result<Option<(String, Vec<FoundParam>, String)>, (String, String)> {
    let text = line
        .trim_start_matches("public static ")
        .trim_start_matches("static ");
    let Some((head, after)) = text.split_once('(') else {
        return Ok(None);
    };
    // `int Name` or `List<string> Name`; a generic method (`Name<T>`) is skipped.
    let mut words = head.split_whitespace().collect::<Vec<_>>();
    if words.len() < 2 {
        return Ok(None);
    }
    let name = words.pop().unwrap().to_string();
    if name.contains('<') {
        return Err((name, "generic methods have no wire form".into()));
    }
    let returns = csharp_type(&words.join(" "));
    if returns == "None" {
        return Ok(None);
    }
    let (params, _tail) = split_params(after)?;
    let mut out = Vec::new();
    for param in params {
        let param = param.trim();
        if param.is_empty() {
            continue;
        }
        if param.starts_with("params ") || param.starts_with("ref ") || param.starts_with("out ") {
            return Err((name, "ref/out/params parameters have no wire form".into()));
        }
        let (default, decl) = match param.split_once('=') {
            Some((decl, value)) => (Some(value.trim().to_string()), decl.trim()),
            None => (None, param),
        };
        let mut parts = decl.split_whitespace().collect::<Vec<_>>();
        if parts.len() < 2 {
            return Err((name, "a parameter without a type".into()));
        }
        let var = parts.pop().unwrap().to_string();
        let ty = csharp_type(&parts.join(" "));
        out.push(FoundParam {
            name: var,
            ty,
            has_default: default.is_some(),
            default: None,
        });
    }
    Ok(Some((name, out, returns)))
}

fn split_params(after: &str) -> Result<(Vec<String>, &str), (String, String)> {
    let mut params = Vec::new();
    let mut depth = 0i32;
    let mut start = 0usize;
    for (index, ch) in after.char_indices() {
        match ch {
            '(' | '<' | '[' => depth += 1,
            ')' | ']' => {
                if ch == ')' && depth == 0 {
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
    Err(("method".to_string(), "an unclosed parameter list".into()))
}

fn csharp_type(text: &str) -> String {
    let text = text.trim();
    match text {
        "string" | "String" => "str".into(),
        "int" | "Int32" | "long" | "Int64" | "short" | "byte" => "int".into(),
        "double" | "Double" | "float" | "Single" => "float".into(),
        "bool" | "Boolean" => "bool".into(),
        "void" => "None".into(),
        "List<string>" | "IList<string>" | "IReadOnlyList<string>" | "string[]" => {
            "list[str]".into()
        }
        "List<int>" | "int[]" => "list[int]".into(),
        "Dictionary<string,string>" | "Dictionary<string, string>" => "dict[str]".into(),
        _ => "Any".into(),
    }
}
