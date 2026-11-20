//! Java: a unit is a class file. The analyzer reads public static methods, the
//! recorder compiles a harness with javac, and the shadow binding is a JNI
//! library over the C ABI.
//!
//! The supported subset is a class (ideally public) with `public static` methods
//! whose parameters and return value cross the wire: int/long/short/byte,
//! double/float, boolean, String, List<String> and Map<String,String>. Everything
//! else (arrays, varargs, other generic types, instance methods, constructors) is
//! reported as skipped with the type that refused it.

use std::path::{Path, PathBuf};

use crate::json::Json;
use crate::project::Project;
use crate::util;

use super::{Analysis, Found, FoundParam, RecordOptions};

struct JavaParam {
    name: String,
    /// The type as written (`List<String>`, `int`): what the harness declares.
    raw: String,
    /// The wire annotation (`list[str]`, `int`): what the port receives.
    ty: String,
}

struct JavaFunction {
    name: String,
    line: u32,
    returns_raw: String,
    returns_ty: String,
    params: Vec<JavaParam>,
    throws: Vec<String>,
    doc: String,
}

struct Parsed {
    package: String,
    class: String,
    imports: Vec<String>,
    functions: Vec<JavaFunction>,
    skipped: Vec<(String, String)>,
    lines: u32,
}

pub fn analyze(path: &Path) -> Result<Analysis, String> {
    let parsed = parse_java(path)?;
    Ok(Analysis {
        functions: parsed
            .functions
            .iter()
            .map(|function| Found {
                name: function.name.clone(),
                line: function.line,
                call: format!("{}.{}", parsed.class, function.name),
                params: function
                    .params
                    .iter()
                    .map(|param| FoundParam {
                        name: param.name.clone(),
                        ty: param.ty.clone(),
                        has_default: false,
                        default: None,
                    })
                    .collect(),
                returns: function.returns_ty.clone(),
                doc: function.doc.clone(),
            })
            .collect(),
        skipped: parsed.skipped,
        imports: parsed.imports,
        lines: parsed.lines,
    })
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

fn parse_java(path: &Path) -> Result<Parsed, String> {
    let source = util::read(path)?;
    let lines = source.lines().count() as u32;
    let source_lines: Vec<&str> = source.lines().collect();
    let blank = blank_literals(&source);

    let package = match keyword_positions(&blank, "package")
        .first()
        .and_then(|position| statement_after(&blank, Some(position + "package".len())))
    {
        Some((text, _)) => text,
        None => String::new(),
    };
    let mut imports = Vec::new();
    if !package.is_empty() {
        imports.push(package.clone());
    }
    for position in keyword_positions(&blank, "import") {
        if let Some((text, _)) = statement_after(&blank, Some(position + "import".len())) {
            if !text.is_empty() {
                imports.push(text);
            }
        }
    }

    let classes = top_level_classes(&blank)?;
    let mut declared = classes.iter().filter(|class| class.kind == "class");
    let chosen = declared
        .clone()
        .find(|class| class.public)
        .or_else(|| declared.next())
        .ok_or_else(|| {
            format!(
                "{}: no top-level class declaration (the Java plugin ports a class with public \
                 static methods)",
                path.display()
            )
        })?;
    let body = &blank[chosen.open + 1..chosen.close];
    let base = chosen.open + 1;

    let mut functions: Vec<JavaFunction> = Vec::new();
    let mut skipped: Vec<(String, String)> = Vec::new();
    for class in &classes {
        if class.open != chosen.open {
            let reason = if class.kind == "class" {
                format!(
                    "another top-level class: only `{}` is ported (one class per unit)",
                    chosen.name
                )
            } else {
                format!(
                    "a top-level {}: only a class is ported (one class per unit)",
                    class.kind
                )
            };
            skipped.push((class.name.clone(), reason));
        }
    }
    let mut i = 0usize;
    while i < body.len() {
        i = skip_ws(body, i);
        if i >= body.len() {
            break;
        }
        let decl_start = i;
        let head = skip_annotations(body, i);
        if head >= body.len() {
            break;
        }
        // Scan the declaration head to its first `(`, `{` or `;`.
        let bytes = body.as_bytes();
        let mut j = head;
        let mut angle = 0i32;
        let mut opener: Option<usize> = None;
        let mut block: Option<usize> = None;
        while j < bytes.len() {
            match bytes[j] {
                b'<' => angle += 1,
                b'>' => angle -= 1,
                b'(' if angle <= 0 => {
                    opener = Some(j);
                    break;
                }
                b'{' | b';' if angle <= 0 => {
                    block = Some(j);
                    break;
                }
                _ => {}
            }
            j += 1;
        }
        let Some(j) = opener.or(block) else { break };
        if bytes[j] != b'(' {
            // A field, a static initializer, or a nested type: the last is a
            // supported-construct report, not a silent skip.
            let head_text = &body[head..j];
            let (_, _, _, rest) = strip_modifiers(head_text);
            for keyword in ["class", "interface", "enum", "record"] {
                if strip_word(&rest, keyword).is_none() {
                    continue;
                }
                let name_start = skip_ws(&rest, keyword.len());
                let name_end = ident_end(&rest, name_start);
                let name = if name_end > name_start {
                    rest[name_start..name_end].to_string()
                } else {
                    keyword.to_string()
                };
                skipped.push((
                    name,
                    format!("a nested {keyword}: only the outer class's methods are ported"),
                ));
                break;
            }
            if bytes[j] == b';' {
                i = j + 1;
                continue;
            }
            let mut after = matching(body, j)
                .map(|close| close + 1)
                .unwrap_or(body.len());
            let next = skip_ws(body, after);
            if next < body.len() && body.as_bytes()[next] == b';' {
                after = next + 1;
            }
            i = after.max(i + 1);
            continue;
        }
        // A parenthesized call/annotation in a field initializer is not a method.
        if body[head..j].contains('=') || body[head..j].contains('@') {
            i = skip_member(body, j);
            continue;
        }
        let mut name_start = j;
        while name_start > head && bytes[name_start - 1].is_ascii_whitespace() {
            name_start -= 1;
        }
        let name_end = name_start;
        while name_start > head && is_ident_byte(bytes[name_start - 1]) {
            name_start -= 1;
        }
        let name = body[name_start..name_end].to_string();
        let Some(close) = matching(body, j) else {
            skipped.push((name, "an unclosed parameter list".into()));
            break;
        };
        // Everything after the parameters: optional `throws`, then a body or `;`.
        let mut after = skip_ws(body, close + 1);
        let mut throws = Vec::new();
        if body[after..].starts_with("throws") {
            let start = after + "throws".len();
            let mut depth = 0i32;
            let bytes = body.as_bytes();
            let mut k = start;
            let mut end = body.len();
            while k < bytes.len() {
                match bytes[k] {
                    b'<' => depth += 1,
                    b'>' => depth -= 1,
                    b'{' | b';' if depth <= 0 => {
                        end = k;
                        break;
                    }
                    _ => {}
                }
                k += 1;
            }
            for part in split_angles(&body[start..end], ',') {
                let part = part.trim();
                if !part.is_empty() {
                    throws.push(part.to_string());
                }
            }
            after = skip_ws(body, end);
        }
        let has_body = after < body.len() && body.as_bytes()[after] == b'{';
        if has_body {
            let Some(end) = matching(body, after) else {
                skipped.push((name, "an unclosed method body".into()));
                break;
            };
            after = end + 1;
        } else if after < body.len() && body.as_bytes()[after] == b';' {
            after += 1;
        }
        i = after.max(i + 1);

        let declaration_line = 1 + blank[..base + decl_start]
            .bytes()
            .filter(|byte| *byte == b'\n')
            .count() as u32;
        let doc = doc_before(&source_lines, declaration_line as usize - 1);

        let (is_public, is_static, is_private, rest) = strip_modifiers(&body[head..name_start]);
        if rest.is_empty() {
            skipped.push((
                name,
                "a constructor: the recorder constructs no instance".into(),
            ));
            continue;
        }
        if rest.starts_with('<') {
            skipped.push((
                name,
                "a generic method: its type parameter has no wire form".into(),
            ));
            continue;
        }
        if is_private {
            skipped.push((
                name,
                "a private method: not part of the class's public surface".into(),
            ));
            continue;
        }
        if !is_static {
            skipped.push((
                name,
                "an instance method: the recorder calls public static methods only".into(),
            ));
            continue;
        }
        if !is_public {
            skipped.push((
                name,
                "not public: the recorder calls public static methods only".into(),
            ));
            continue;
        }
        if !has_body {
            skipped.push((
                name,
                "a native or abstract method: there is no body to observe".into(),
            ));
            continue;
        }
        let returns_raw = rest.trim().to_string();
        if returns_raw == "void" {
            skipped.push((
                name,
                "returns void: the wire carries values, not effects".into(),
            ));
            continue;
        }
        let Some(returns_ty) = wire_type(&returns_raw) else {
            skipped.push((
                name,
                format!("the return type `{returns_raw}` has no wire form"),
            ));
            continue;
        };
        let mut params = Vec::new();
        let param_text = &body[j + 1..close];
        let mut refused = None;
        for part in split_angles(param_text, ',') {
            if part.trim().is_empty() {
                continue;
            }
            match parse_param(part) {
                Ok((name, raw)) => match wire_type(&raw) {
                    Some(ty) => params.push(JavaParam {
                        name,
                        raw,
                        ty: ty.to_string(),
                    }),
                    None => {
                        refused = Some(format!(
                            "the parameter `{name}` has type `{raw}`, which has no wire form"
                        ));
                        break;
                    }
                },
                Err(reason) => {
                    refused = Some(reason);
                    break;
                }
            }
        }
        if let Some(reason) = refused {
            skipped.push((name, reason));
            continue;
        }
        if functions.iter().any(|function| function.name == name) {
            skipped.push((
                name,
                "an overload: the wire names functions by name, so one signature per name".into(),
            ));
            continue;
        }
        functions.push(JavaFunction {
            name,
            line: declaration_line,
            returns_raw,
            returns_ty: returns_ty.to_string(),
            params,
            throws,
            doc,
        });
    }

    if functions.is_empty() && skipped.is_empty() {
        skipped.push((
            format!("class {}", chosen.name),
            "no method declarations found in the class body".into(),
        ));
    }
    Ok(Parsed {
        package,
        class: chosen.name.clone(),
        imports,
        functions,
        skipped,
        lines,
    })
}

struct ClassDecl {
    name: String,
    kind: String,
    open: usize,
    close: usize,
    public: bool,
}

fn top_level_classes(text: &str) -> Result<Vec<ClassDecl>, String> {
    let bytes = text.as_bytes();
    let mut out = Vec::new();
    let mut i = 0usize;
    let mut depth = 0i32;
    while i < bytes.len() {
        match bytes[i] {
            b'{' => {
                depth += 1;
                i += 1;
                continue;
            }
            b'}' => {
                depth -= 1;
                i += 1;
                continue;
            }
            _ => {}
        }
        if depth == 0 && is_ident_start(bytes[i]) {
            let end = ident_end(text, i);
            let word = &text[i..end];
            if word == "class" || word == "interface" || word == "enum" || word == "record" {
                let kind = word.to_string();
                let mut name_start = skip_ws(text, end);
                if name_start < bytes.len() && bytes[name_start] == b'<' {
                    name_start = skip_angles(text, name_start)
                        .map(|close| close + 1)
                        .unwrap_or(bytes.len());
                }
                name_start = skip_ws(text, name_start);
                let name_end = ident_end(text, name_start);
                if name_end == name_start {
                    i = end;
                    continue;
                }
                let name = text[name_start..name_end].to_string();
                // The body opens after `extends`/`implements` clauses.
                let mut open = name_end;
                let mut angle = 0i32;
                while open < bytes.len() {
                    match bytes[open] {
                        b'<' => angle += 1,
                        b'>' => angle -= 1,
                        b'{' if angle <= 0 => break,
                        b';' if angle <= 0 => break,
                        _ => {}
                    }
                    open += 1;
                }
                if open >= bytes.len() || bytes[open] != b'{' {
                    i = end;
                    continue;
                }
                let Some(close) = matching(text, open) else {
                    return Err("a class body is not closed".into());
                };
                let mut start = i;
                while start > 0 && bytes[start - 1] != b';' && bytes[start - 1] != b'}' {
                    start -= 1;
                }
                let modifiers = &text[start..i];
                let public = modifiers.split_whitespace().any(|word| word == "public");
                out.push(ClassDecl {
                    name,
                    kind,
                    open,
                    close,
                    public,
                });
                i = close + 1;
                continue;
            }
            i = end;
            continue;
        }
        i += 1;
    }
    Ok(out)
}

fn parse_param(text: &str) -> Result<(String, String), String> {
    let mut text = text.trim().to_string();
    loop {
        let trimmed = text.trim_start().to_string();
        if !trimmed.starts_with('@') {
            text = trimmed;
            break;
        }
        let end = annotation_end(&trimmed);
        text = trimmed[end..].to_string();
    }
    if let Some(rest) = strip_word(&text, "final") {
        text = rest.to_string();
    }
    let text = text.trim();
    if text.contains("...") {
        return Err("a varargs parameter has no wire form".into());
    }
    let Some(split) = text.rfind(char::is_whitespace) else {
        return Err(format!("`{text}` is not a typed parameter"));
    };
    let ty = text[..split].trim().to_string();
    let name = text[split..].trim().to_string();
    if name.is_empty() || ty.is_empty() {
        return Err(format!("`{text}` is not a typed parameter"));
    }
    if name.contains('[') || ty.ends_with("[]") {
        return Err(format!("the array parameter `{name}` has no wire form"));
    }
    if !is_java_identifier(&name) {
        return Err(format!("`{name}` is not a plain parameter name"));
    }
    Ok((name, ty))
}

fn wire_type(raw: &str) -> Option<&'static str> {
    let compact: String = raw.chars().filter(|ch| !ch.is_whitespace()).collect();
    match compact.as_str() {
        "int" | "long" | "short" | "byte" => Some("int"),
        "double" | "float" => Some("float"),
        "boolean" => Some("bool"),
        "String" | "java.lang.String" => Some("str"),
        "List<String>"
        | "ArrayList<String>"
        | "LinkedList<String>"
        | "Collection<String>"
        | "java.util.List<String>"
        | "java.util.ArrayList<String>"
        | "java.util.LinkedList<String>"
        | "java.util.Collection<String>" => Some("list[str]"),
        "Map<String,String>"
        | "HashMap<String,String>"
        | "LinkedHashMap<String,String>"
        | "java.util.Map<String,String>"
        | "java.util.HashMap<String,String>"
        | "java.util.LinkedHashMap<String,String>" => Some("dict[str]"),
        _ => None,
    }
}

/// Blanks comments and string/char literals with spaces, keeping newlines so
/// line numbers and byte positions stay usable for the structural scan.
fn blank_literals(source: &str) -> String {
    let chars: Vec<char> = source.chars().collect();
    let mut out = String::with_capacity(source.len());
    let mut i = 0usize;
    while i < chars.len() {
        let ch = chars[i];
        if ch == '/' && chars.get(i + 1) == Some(&'/') {
            out.push(' ');
            out.push(' ');
            i += 2;
            while i < chars.len() && chars[i] != '\n' {
                out.push(' ');
                i += 1;
            }
        } else if ch == '/' && chars.get(i + 1) == Some(&'*') {
            out.push(' ');
            out.push(' ');
            i += 2;
            while i < chars.len() {
                if chars[i] == '*' && chars.get(i + 1) == Some(&'/') {
                    out.push(' ');
                    out.push(' ');
                    i += 2;
                    break;
                }
                if chars[i] == '\n' {
                    out.push('\n');
                } else {
                    out.push(' ');
                }
                i += 1;
            }
        } else if ch == '"' {
            if chars.get(i + 1) == Some(&'"') && chars.get(i + 2) == Some(&'"') {
                out.push_str("   ");
                i += 3;
                while i < chars.len() {
                    if chars[i] == '"'
                        && chars.get(i + 1) == Some(&'"')
                        && chars.get(i + 2) == Some(&'"')
                    {
                        out.push_str("   ");
                        i += 3;
                        break;
                    }
                    if chars[i] == '\n' {
                        out.push('\n');
                    } else {
                        out.push(' ');
                    }
                    i += 1;
                }
            } else {
                out.push('"');
                i += 1;
                while i < chars.len() {
                    if chars[i] == '\\' {
                        out.push(' ');
                        out.push(' ');
                        i += 2;
                        continue;
                    }
                    if chars[i] == '"' {
                        out.push('"');
                        i += 1;
                        break;
                    }
                    if chars[i] == '\n' {
                        out.push('\n');
                    } else {
                        out.push(' ');
                    }
                    i += 1;
                }
            }
        } else if ch == '\'' {
            out.push('\'');
            i += 1;
            while i < chars.len() {
                if chars[i] == '\\' {
                    out.push(' ');
                    out.push(' ');
                    i += 2;
                    continue;
                }
                if chars[i] == '\'' {
                    out.push('\'');
                    i += 1;
                    break;
                }
                if chars[i] == '\n' {
                    out.push('\n');
                } else {
                    out.push(' ');
                }
                i += 1;
            }
        } else {
            out.push(ch);
            i += 1;
        }
    }
    out
}

fn is_ident_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'$'
}

fn is_ident_start(byte: u8) -> bool {
    byte.is_ascii_alphabetic() || byte == b'_' || byte == b'$'
}

fn is_java_identifier(text: &str) -> bool {
    let mut chars = text.chars();
    match chars.next() {
        Some(ch) if ch.is_alphabetic() || ch == '_' || ch == '$' => {}
        _ => return false,
    }
    chars.all(|ch| ch.is_alphanumeric() || ch == '_' || ch == '$')
}

fn ident_end(text: &str, start: usize) -> usize {
    let bytes = text.as_bytes();
    let mut i = start;
    while i < bytes.len() && (is_ident_byte(bytes[i]) || bytes[i] >= 0x80) {
        i += 1;
    }
    i
}

fn skip_ws(text: &str, mut i: usize) -> usize {
    let bytes = text.as_bytes();
    while i < bytes.len() && bytes[i].is_ascii_whitespace() {
        i += 1;
    }
    i
}

fn matching(text: &str, open: usize) -> Option<usize> {
    let bytes = text.as_bytes();
    let open_byte = *bytes.get(open)?;
    let close_byte = match open_byte {
        b'{' => b'}',
        b'(' => b')',
        b'[' => b']',
        _ => return None,
    };
    let mut depth = 0usize;
    let mut i = open;
    while i < bytes.len() {
        if bytes[i] == open_byte {
            depth += 1;
        } else if bytes[i] == close_byte {
            depth -= 1;
            if depth == 0 {
                return Some(i);
            }
        }
        i += 1;
    }
    None
}

fn skip_angles(text: &str, open: usize) -> Option<usize> {
    let bytes = text.as_bytes();
    if bytes.get(open) != Some(&b'<') {
        return None;
    }
    let mut depth = 0usize;
    let mut i = open;
    while i < bytes.len() {
        match bytes[i] {
            b'<' => depth += 1,
            b'>' => {
                depth -= 1;
                if depth == 0 {
                    return Some(i);
                }
            }
            _ => {}
        }
        i += 1;
    }
    None
}

fn keyword_positions(text: &str, keyword: &str) -> Vec<usize> {
    let bytes = text.as_bytes();
    let mut out = Vec::new();
    let mut i = 0usize;
    while i + keyword.len() <= bytes.len() {
        if &bytes[i..i + keyword.len()] == keyword.as_bytes()
            && (i == 0 || !is_ident_byte(bytes[i - 1]))
            && (i + keyword.len() == bytes.len() || !is_ident_byte(bytes[i + keyword.len()]))
        {
            out.push(i);
            i += keyword.len();
        } else {
            i += 1;
        }
    }
    out
}

fn statement_after(text: &str, position: Option<usize>) -> Option<(String, usize)> {
    let position = position?;
    let rest = skip_ws(text, position);
    let end = text[rest..].find(';')? + rest;
    Some((text[rest..end].trim().to_string(), end + 1))
}

fn skip_annotations(text: &str, mut i: usize) -> usize {
    loop {
        i = skip_ws(text, i);
        if i >= text.len() || text.as_bytes()[i] != b'@' {
            return i;
        }
        i = annotation_end(&text[i..]) + i;
    }
}

/// `text` starts with `@`; returns the offset just past the annotation.
fn annotation_end(text: &str) -> usize {
    let bytes = text.as_bytes();
    let mut i = 1usize;
    while i < bytes.len() && (is_ident_byte(bytes[i]) || bytes[i] == b'.' || bytes[i] >= 0x80) {
        i += 1;
    }
    i = skip_ws(text, i);
    if i < bytes.len() && bytes[i] == b'(' {
        matching(text, i)
            .map(|close| close + 1)
            .unwrap_or(bytes.len())
    } else {
        i
    }
}

/// Body text from a field start to the end of its statement or initializer block.
fn skip_member(text: &str, mut i: usize) -> usize {
    let bytes = text.as_bytes();
    let mut paren = 0i32;
    let mut brace = 0i32;
    let mut bracket = 0i32;
    while i < bytes.len() {
        match bytes[i] {
            b'(' => paren += 1,
            b')' => paren -= 1,
            b'{' => brace += 1,
            b'}' => {
                brace -= 1;
                if brace < 0 {
                    return i + 1;
                }
            }
            b'[' => bracket += 1,
            b']' => bracket -= 1,
            b';' if paren == 0 && brace == 0 && bracket == 0 => return i + 1,
            _ => {}
        }
        i += 1;
    }
    i
}

fn split_angles(text: &str, separator: char) -> Vec<&str> {
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut start = 0usize;
    for (index, ch) in text.char_indices() {
        match ch {
            '(' | '[' | '{' | '<' => depth += 1,
            ')' | ']' | '}' | '>' => depth -= 1,
            ch if ch == separator && depth <= 0 => {
                out.push(&text[start..index]);
                start = index + ch.len_utf8();
            }
            _ => {}
        }
    }
    out.push(&text[start..]);
    out
}

fn strip_word<'a>(text: &'a str, word: &str) -> Option<&'a str> {
    let rest = text.strip_prefix(word)?;
    if rest.is_empty() || rest.starts_with(|ch: char| ch.is_whitespace()) {
        Some(rest.trim_start())
    } else {
        None
    }
}

fn strip_modifiers(text: &str) -> (bool, bool, bool, String) {
    let mut public = false;
    let mut is_static = false;
    let mut private = false;
    let mut rest = text.trim().to_string();
    loop {
        let mut matched = false;
        for word in [
            "public",
            "private",
            "protected",
            "static",
            "final",
            "abstract",
            "synchronized",
            "native",
            "strictfp",
            "default",
            "transient",
            "volatile",
            "sealed",
            "non-sealed",
        ] {
            if let Some(after) = strip_word(&rest, word) {
                match word {
                    "public" => public = true,
                    "private" => private = true,
                    "static" => is_static = true,
                    _ => {}
                }
                rest = after.to_string();
                matched = true;
                break;
            }
        }
        if !matched {
            break;
        }
    }
    (public, is_static, private, rest.trim().to_string())
}

fn doc_before(lines: &[&str], line: usize) -> String {
    let mut index = line;
    while index > 0 {
        let text = lines[index - 1].trim();
        if text.is_empty() {
            index -= 1;
            continue;
        }
        if !text.ends_with("*/") {
            return String::new();
        }
        let mut collected = vec![text.to_string()];
        let mut start = index - 1;
        while !lines[start].contains("/**") {
            if start == 0 {
                return String::new();
            }
            start -= 1;
            collected.push(lines[start].trim().to_string());
        }
        collected.reverse();
        let joined = collected.join("\n");
        let Some(open) = joined.find("/**") else {
            return String::new();
        };
        let Some(close) = joined.rfind("*/") else {
            return String::new();
        };
        let inner = &joined[open + 3..close];
        let mut parts = Vec::new();
        for part in inner.lines() {
            let part = part.trim();
            let part = part.strip_prefix('*').unwrap_or(part).trim();
            if !part.is_empty() {
                parts.push(part.to_string());
            }
        }
        return parts.join(" ");
    }
    String::new()
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

pub fn record(
    project: &Project,
    _unit: &crate::inventory::Unit,
    calls: &[Json],
    _options: &RecordOptions,
) -> Result<usize, String> {
    let parsed = parse_java(&project.source_file)?;
    let harness = render_record(&parsed, calls)?;
    let harness_path = project.dir.join("build").join("Record.java");
    util::write(&harness_path, &harness)?;
    let classes = compile(project, &parsed, &[harness_path])?;
    let output = run_java(project, &classes, &["Record".to_string()])?;
    super::write_cases(project, calls, &output)
}

fn compile(project: &Project, parsed: &Parsed, sources: &[PathBuf]) -> Result<PathBuf, String> {
    let classes = project.dir.join("build").join("classes");
    let _ = std::fs::remove_dir_all(&classes);
    std::fs::create_dir_all(&classes).map_err(|error| format!("{}: {error}", classes.display()))?;
    let root = source_root(&project.source_file, &parsed.package);
    let mut args = vec![
        "-encoding".to_string(),
        "UTF-8".to_string(),
        "-proc:none".to_string(),
        "-d".to_string(),
        classes.display().to_string(),
        "-sourcepath".to_string(),
        root.display().to_string(),
    ];
    for source in sources {
        args.push(source.display().to_string());
    }
    args.push(project.source_file.display().to_string());
    super::run_harness(project, "javac", &args, None)?;
    Ok(classes)
}

fn run_java(project: &Project, classes: &Path, extra: &[String]) -> Result<String, String> {
    let mut args = vec![
        "-Dstdout.encoding=UTF-8".to_string(),
        "-cp".to_string(),
        classes.display().to_string(),
    ];
    args.extend(extra.iter().cloned());
    super::run_harness(project, "java", &args, None)
}

fn source_root(file: &Path, package: &str) -> PathBuf {
    let mut root = file.parent().map(Path::to_path_buf).unwrap_or_default();
    for _ in package.split('.').filter(|part| !part.is_empty()) {
        if let Some(parent) = root.parent() {
            root = parent.to_path_buf();
        }
    }
    root
}

fn render_record(parsed: &Parsed, calls: &[Json]) -> Result<String, String> {
    let mut out = String::new();
    out.push_str(
        "// Generated by cpp-anything record: one JSON line per call, no JSON parser needed.\n",
    );
    out.push_str("public final class Record {\n");
    out.push_str("  interface Case { void run() throws Throwable; }\n\n");
    out.push_str(
        r#"  static void run(String id, Case body) {
    try {
      body.run();
    } catch (Throwable error) {
      System.out.println("{\"id\":" + quote(id) + ",\"err\":{\"type\":"
          + quote(error.getClass().getSimpleName()) + ",\"msg\":" + quote(message(error)) + "}}");
    }
  }

  static String message(Throwable error) {
    String text = error.getMessage();
    if (text == null) return "";
    return text.length() > 200 ? text.substring(0, 200) : text;
  }

  static String quote(String text) {
    StringBuilder out = new StringBuilder("\"");
    for (int i = 0; i < text.length(); i++) {
      char c = text.charAt(i);
      switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
          if (c < 0x20) out.append(String.format("\\u%04x", (int) c));
          else out.append(c);
      }
    }
    out.append('"');
    return out.toString();
  }

  static String numJson(double value) {
    if (!Double.isFinite(value)) return "{\"$opaque\":\"Double\"}";
    return Double.toString(value);
  }

  static String strJson(String value) {
    return value == null ? "null" : quote(value);
  }

  static String listJson(java.util.Collection<String> values) {
    if (values == null) return "null";
    StringBuilder out = new StringBuilder("[");
    boolean first = true;
    for (String item : values) {
      if (!first) out.append(',');
      first = false;
      out.append(item == null ? "null" : quote(item));
    }
    return out.append(']').toString();
  }

  static String mapJson(java.util.Map<String, String> values) {
    if (values == null) return "null";
    java.util.TreeMap<String, String> sorted = new java.util.TreeMap<>(values);
    StringBuilder out = new StringBuilder("{");
    boolean first = true;
    for (java.util.Map.Entry<String, String> entry : sorted.entrySet()) {
      if (!first) out.append(',');
      first = false;
      out.append(quote(entry.getKey())).append(':');
      out.append(entry.getValue() == null ? "null" : quote(entry.getValue()));
    }
    return out.append('}').toString();
  }

  static String opaque(String type) {
    return "{\"$opaque\":" + quote(type) + "}";
  }

  public static void main(String[] args) {
"#,
    );
    for call in calls {
        let function_name = call
            .get("fn")
            .and_then(Json::as_str)
            .ok_or("a recorded call without fn")?;
        let id = call
            .get("id")
            .and_then(Json::as_str)
            .unwrap_or(function_name);
        let args = call
            .get("args")
            .and_then(Json::as_array)
            .unwrap_or(&[])
            .to_vec();
        let function = parsed
            .functions
            .iter()
            .find(|function| function.name == function_name)
            .ok_or_else(|| {
                format!("the recorder has no supported Java signature for `{function_name}`")
            })?;
        if args.len() != function.params.len() {
            return Err(format!(
                "`{function_name}` takes {} arguments, the call has {}",
                function.params.len(),
                args.len()
            ));
        }
        let mut literals = Vec::new();
        for (param, arg) in function.params.iter().zip(&args) {
            let literal = java_literal(arg, &param.raw, &param.ty).ok_or_else(|| {
                format!(
                    "`{}` cannot be written as a `{}` literal in the Java harness: {arg}",
                    function_name, param.raw
                )
            })?;
            literals.push(literal);
        }
        let target = format!(
            "{}.{}.{}({})",
            parsed.package,
            parsed.class,
            function.name,
            literals.join(", ")
        );
        let target = if parsed.package.is_empty() {
            target.trim_start_matches('.').to_string()
        } else {
            target
        };
        let id_literal = java_quote(id);
        out.push_str("    run(");
        out.push_str(&id_literal);
        out.push_str(", () -> {\n      ");
        out.push_str(&format!(
            "{} cppanResult = {};\n",
            function.returns_raw, target
        ));
        out.push_str("      System.out.println(\"{\\\"id\\\":\" + quote(");
        out.push_str(&id_literal);
        out.push_str(") + \",\\\"ok\\\":\" + ");
        out.push_str(&value_expression(
            &function.returns_ty,
            "cppanResult",
            &function.returns_raw,
        ));
        out.push_str(" + \"}\");\n    });\n");
    }
    out.push_str("  }\n}\n");
    Ok(out)
}

fn render_bench(parsed: &Parsed, chosen: &Json) -> Result<String, String> {
    let mut calls = Vec::new();
    for function in &parsed.functions {
        let Some(doc) = chosen.get(&function.name).and_then(Json::as_array) else {
            continue;
        };
        let mut literals = Vec::new();
        for (param, arg) in function.params.iter().zip(doc.iter()) {
            let literal = java_literal(arg, &param.raw, &param.ty).ok_or_else(|| {
                format!(
                    "`{}` cannot be written as a `{}` literal in the Java harness",
                    function.name, param.raw
                )
            })?;
            literals.push(literal);
        }
        let target = if parsed.package.is_empty() {
            format!(
                "{}.{}({})",
                parsed.class,
                function.name,
                literals.join(", ")
            )
        } else {
            format!(
                "{}.{}.{}({})",
                parsed.package,
                parsed.class,
                function.name,
                literals.join(", ")
            )
        };
        calls.push(target);
    }
    let mut out = String::new();
    out.push_str("// Generated by cpp-anything bench: warm up once, then time the whole loop.\n");
    out.push_str("public final class Bench {\n");
    out.push_str("  public static void main(String[] args) {\n");
    out.push_str("    int repeat = Integer.parseInt(args[0]);\n");
    out.push_str("    for (int cppanWarm = 0; cppanWarm < 1; cppanWarm++) {\n");
    for target in &calls {
        out.push_str(&format!(
            "      try {{ {target}; }} catch (Throwable cppanIgnored) {{}}\n"
        ));
    }
    out.push_str("    }\n");
    out.push_str("    long started = System.nanoTime();\n");
    out.push_str("    int calls = 0;\n");
    out.push_str("    for (int cppanI = 0; cppanI < repeat; cppanI++) {\n");
    for target in &calls {
        out.push_str(&format!(
            "      try {{ {target}; }} catch (Throwable cppanIgnored) {{}}\n"
        ));
    }
    out.push_str(&format!("      calls += {};\n", calls.len()));
    out.push_str("    }\n");
    out.push_str("    long elapsed = System.nanoTime() - started;\n");
    out.push_str("    System.out.println(\"{\\\"ns\\\":\" + elapsed + \",\\\"calls\\\":\" + calls + \"}\");\n");
    out.push_str("  }\n}\n");
    Ok(out)
}

fn value_expression(ty: &str, value: &str, _raw: &str) -> String {
    match ty {
        "int" => format!("Long.toString((long) {value})"),
        "float" => format!("numJson({value})"),
        "bool" => format!("Boolean.toString({value})"),
        "str" => format!("strJson({value})"),
        "list[str]" => format!("listJson({value})"),
        "dict[str]" => format!("mapJson({value})"),
        _ => format!("opaque(\"{}\")", ty.replace('"', "'")),
    }
}

fn java_literal(value: &Json, raw: &str, ty: &str) -> Option<String> {
    if value.is_null() {
        return Some("null".to_string());
    }
    match ty {
        "int" => {
            let number = value.as_i64()?;
            if raw == "long" {
                Some(format!("{number}L"))
            } else {
                Some(number.to_string())
            }
        }
        "float" => {
            let number = value.as_f64()?;
            Some(float_literal(number, raw))
        }
        "bool" => Some(if value.as_bool()? { "true" } else { "false" }.to_string()),
        "str" => Some(java_quote(value.as_str()?)),
        "list[str]" => {
            let items = value.as_array()?;
            let mut parts = Vec::new();
            for item in items {
                if item.is_null() {
                    parts.push("null".to_string());
                } else {
                    parts.push(java_quote(item.as_str()?));
                }
            }
            let base = format!("java.util.List.of({})", parts.join(", "));
            Some(collection_literal(raw, base))
        }
        "dict[str]" => {
            let pairs = value.as_object()?;
            let mut parts = Vec::new();
            for (key, item) in pairs {
                parts.push(java_quote(key));
                if item.is_null() {
                    parts.push("null".to_string());
                } else {
                    parts.push(java_quote(item.as_str()?));
                }
            }
            let base = format!("java.util.Map.of({})", parts.join(", "));
            Some(map_literal(raw, base))
        }
        _ => None,
    }
}

fn collection_literal(raw: &str, base: String) -> String {
    let compact: String = raw.chars().filter(|ch| !ch.is_whitespace()).collect();
    if compact.contains("ArrayList") || compact.contains("LinkedList") {
        format!("new java.util.ArrayList<>({base})")
    } else {
        base
    }
}

fn map_literal(raw: &str, base: String) -> String {
    let compact: String = raw.chars().filter(|ch| !ch.is_whitespace()).collect();
    if compact.contains("HashMap") || compact.contains("LinkedHashMap") {
        format!("new java.util.HashMap<>({base})")
    } else {
        base
    }
}

fn float_literal(value: f64, raw: &str) -> String {
    if value.is_nan() {
        return if raw == "float" {
            "Float.NaN".to_string()
        } else {
            "Double.NaN".to_string()
        };
    }
    if value.is_infinite() {
        let name = if value > 0.0 {
            "POSITIVE_INFINITY"
        } else {
            "NEGATIVE_INFINITY"
        };
        return if raw == "float" {
            format!("Float.{name}")
        } else {
            format!("Double.{name}")
        };
    }
    let mut text = format!("{value:?}");
    if !text.contains('.') && !text.contains('e') && !text.contains('E') {
        text.push_str(".0");
    }
    if raw == "float" {
        text.push('f');
    }
    text
}

fn java_quote(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            ch if (ch as u32) < 0x20 || (ch as u32) > 0x7e => {
                let mut buffer = [0u16; 2];
                for unit in ch.encode_utf16(&mut buffer).iter() {
                    out.push_str(&format!("\\u{unit:04x}"));
                }
            }
            ch => out.push(ch),
        }
    }
    out.push('"');
    out
}

pub fn bench(project: &Project, repeat: usize) -> Result<f64, String> {
    let parsed = parse_java(&project.source_file)?;
    let text = util::read(&project.dir.join("cases.jsonl"))?;
    let mut first: std::collections::BTreeMap<String, (bool, Json)> =
        std::collections::BTreeMap::new();
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)?;
        let Some(name) = doc.get("fn").and_then(Json::as_str) else {
            continue;
        };
        let ok = doc
            .get("expect")
            .and_then(|expect| expect.get("ok"))
            .is_some();
        let entry = first.entry(name.to_string());
        match entry {
            std::collections::btree_map::Entry::Vacant(slot) => {
                slot.insert((ok, doc));
            }
            std::collections::btree_map::Entry::Occupied(mut slot) => {
                if ok && !slot.get().0 {
                    slot.insert((ok, doc));
                }
            }
        }
    }
    let mut chosen: std::collections::BTreeMap<String, Vec<Json>> =
        std::collections::BTreeMap::new();
    for (name, (_, doc)) in first {
        chosen.insert(
            name,
            doc.get("args")
                .and_then(Json::as_array)
                .map(|items| items.to_vec())
                .unwrap_or_default(),
        );
    }
    let chosen = Json::Object(
        chosen
            .into_iter()
            .map(|(name, args)| (name, Json::Array(args)))
            .collect(),
    );
    let harness = render_bench(&parsed, &chosen)?;
    let harness_path = project.dir.join("build").join("Bench.java");
    util::write(&harness_path, &harness)?;
    let classes = compile(project, &parsed, &[harness_path])?;
    let output = run_java(
        project,
        &classes,
        &["Bench".to_string(), repeat.to_string()],
    )?;
    let doc = Json::parse(output.trim())?;
    let ns = doc.get("ns").and_then(Json::as_f64).unwrap_or(0.0);
    let calls = doc.get("calls").and_then(Json::as_f64).unwrap_or(1.0);
    if calls <= 0.0 {
        return Err("the Java benchmark ran no calls".into());
    }
    Ok(ns / calls)
}

// ---------------------------------------------------------------------------
// shadow
// ---------------------------------------------------------------------------

pub fn shadow_files(project: &Project, _ns: &str) -> Vec<(String, String)> {
    let Ok(parsed) = parse_java(&project.source_file) else {
        return Vec::new();
    };
    let log = crate::shadow::log_path(project);
    let class = format!(
        "{}{}",
        if parsed.package.is_empty() {
            String::new()
        } else {
            format!("{}.", parsed.package)
        },
        parsed.class
    );
    let mut out = String::new();
    out.push_str(
        "// Generated by cpp-anything shadow: the original runs first, the port second,\n",
    );
    out.push_str("// and callers always see the original's result. Disagreements go to the log.\n");
    out.push_str("import java.math.BigDecimal;\n");
    out.push_str("import java.time.Instant;\n");
    out.push_str("import java.util.ArrayList;\n");
    out.push_str("import java.util.LinkedHashMap;\n");
    out.push_str("import java.util.List;\n");
    out.push_str("import java.util.Map;\n\n");
    out.push_str("public final class CppanShadow {\n");
    out.push_str("  static { System.loadLibrary(\"cppan_shadow_jni\"); }\n\n");
    out.push_str("  public static native String call(String fn, String argsJson);\n\n");
    out.push_str(
        "  private static final String LOG = System.getenv(\"CPPAN_SHADOW_LOG\") != null\n",
    );
    out.push_str("      ? System.getenv(\"CPPAN_SHADOW_LOG\")\n");
    out.push_str(&format!(
        "      : {};\n",
        java_quote(&log.display().to_string())
    ));
    out.push_str(
        "  private static final boolean OFF = \"off\".equals(System.getenv(\"CPPAN_SHADOW\"));\n\n",
    );
    out.push_str("  private CppanShadow() {}\n\n");
    out.push_str(
        r#"  static <T extends Throwable> T sneak(Throwable error) throws T {
    throw (T) error;
  }

  static String quote(String text) {
    StringBuilder out = new StringBuilder("\"");
    for (int i = 0; i < text.length(); i++) {
      char c = text.charAt(i);
      switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
          if (c < 0x20) out.append(String.format("\\u%04x", (int) c));
          else out.append(c);
      }
    }
    out.append('"');
    return out.toString();
  }

  static String numJson(double value) {
    if (!Double.isFinite(value)) return "{\"$opaque\":\"Double\"}";
    return Double.toString(value);
  }

  static String strJson(String value) {
    return value == null ? "null" : quote(value);
  }

  static String listJson(java.util.Collection<String> values) {
    if (values == null) return "null";
    StringBuilder out = new StringBuilder("[");
    boolean first = true;
    for (String item : values) {
      if (!first) out.append(',');
      first = false;
      out.append(item == null ? "null" : quote(item));
    }
    return out.append(']').toString();
  }

  static String mapJson(java.util.Map<String, String> values) {
    if (values == null) return "null";
    java.util.TreeMap<String, String> sorted = new java.util.TreeMap<>(values);
    StringBuilder out = new StringBuilder("{");
    boolean first = true;
    for (java.util.Map.Entry<String, String> entry : sorted.entrySet()) {
      if (!first) out.append(',');
      first = false;
      out.append(quote(entry.getKey())).append(':');
      out.append(entry.getValue() == null ? "null" : quote(entry.getValue()));
    }
    return out.append('}').toString();
  }

  static String nativeResult(String fn, String args) {
    try {
      return call(fn, args);
    } catch (Throwable error) {
      return "{\"err\":{\"type\":" + quote(error.getClass().getSimpleName())
          + ",\"msg\":" + quote(String.valueOf(error.getMessage())) + "}}";
    }
  }

  static void check(String fn, String args, String expected) {
    if (OFF) return;
    String got = nativeResult(fn, args);
    String reason = compare(expected, got);
    if (reason != null) log(fn, args, expected, got, reason);
  }

  static void checkError(String fn, String args, Throwable error) {
    if (OFF) return;
    String got = nativeResult(fn, args);
    String expected = "{\"err\":{\"type\":" + quote(error.getClass().getSimpleName())
        + ",\"msg\":" + quote(String.valueOf(error.getMessage())) + "}}";
    String reason = compare(expected, got);
    if (reason != null) log(fn, args, expected, got, reason);
  }

  static void log(String fn, String args, String expected, String got, String reason) {
    try (java.io.FileWriter file = new java.io.FileWriter(LOG, true)) {
      file.write("{\"at\":" + quote(Instant.now().toString())
          + ",\"fn\":" + quote(fn)
          + ",\"args\":" + args
          + ",\"expected\":" + expected
          + ",\"got\":" + got
          + ",\"reason\":" + quote(reason) + "}\n");
    } catch (java.io.IOException ignored) {
    }
  }

  static String compare(String expectedJson, String gotJson) {
    Object expected;
    Object got;
    try {
      expected = parseJson(expectedJson);
      got = parseJson(gotJson);
    } catch (RuntimeException error) {
      return expectedJson.equals(gotJson) ? null : "values differ";
    }
    if (!(expected instanceof Map) || !(got instanceof Map)) {
      return sameValue(expected, got) ? null : "values differ";
    }
    Map<?, ?> left = (Map<?, ?>) expected;
    Map<?, ?> right = (Map<?, ?>) got;
    Object leftError = left.get("err");
    Object rightError = right.get("err");
    if (leftError != null || rightError != null) {
      if (leftError == null) return "the original returns a value, the port raises";
      if (rightError == null) return "the original raises, the port returns a value";
      String expectedType = errorType(leftError);
      String gotType = errorType(rightError);
      if (expectedType.equals(gotType)) return null;
      return "error type: expected " + expectedType + ", got " + gotType;
    }
    Object leftOk = left.containsKey("ok") ? left.get("ok") : null;
    Object rightOk = right.containsKey("ok") ? right.get("ok") : null;
    return sameValue(leftOk, rightOk) ? null : "values differ";
  }

  static String errorType(Object error) {
    if (error instanceof Map && ((Map<?, ?>) error).get("type") instanceof String) {
      return (String) ((Map<?, ?>) error).get("type");
    }
    return "?";
  }

  static boolean sameValue(Object left, Object right) {
    if (left == null || right == null) return left == null && right == null;
    if (left instanceof BigDecimal && right instanceof BigDecimal) {
      return ((BigDecimal) left).compareTo((BigDecimal) right) == 0;
    }
    if (left instanceof Double && right instanceof Double) {
      double a = (Double) left;
      double b = (Double) right;
      return (Double.isNaN(a) && Double.isNaN(b)) || Double.compare(a, b) == 0;
    }
    if (left instanceof Map && right instanceof Map) {
      Map<?, ?> a = (Map<?, ?>) left;
      Map<?, ?> b = (Map<?, ?>) right;
      if (!a.keySet().equals(b.keySet())) return false;
      for (Object key : a.keySet()) {
        if (!sameValue(a.get(key), b.get(key))) return false;
      }
      return true;
    }
    if (left instanceof List && right instanceof List) {
      List<?> a = (List<?>) left;
      List<?> b = (List<?>) right;
      if (a.size() != b.size()) return false;
      for (int i = 0; i < a.size(); i++) {
        if (!sameValue(a.get(i), b.get(i))) return false;
      }
      return true;
    }
    return left.equals(right);
  }

  static Object parseJson(String text) {
    Parser parser = new Parser(text);
    Object value = parser.value();
    parser.space();
    if (parser.pos != text.length()) throw new IllegalArgumentException("trailing input");
    return value;
  }

  static final class Parser {
    final String text;
    int pos;

    Parser(String text) {
      this.text = text;
    }

    void space() {
      while (pos < text.length() && Character.isWhitespace(text.charAt(pos))) pos++;
    }

    Object value() {
      space();
      if (pos >= text.length()) throw new IllegalArgumentException("empty");
      char c = text.charAt(pos);
      switch (c) {
        case '{': return object();
        case '[': return array();
        case '"': return string();
        case 't': literal("true"); return Boolean.TRUE;
        case 'f': literal("false"); return Boolean.FALSE;
        case 'n': literal("null"); return null;
        default: return number();
      }
    }

    void literal(String expected) {
      if (!text.startsWith(expected, pos)) throw new IllegalArgumentException("literal");
      pos += expected.length();
    }

    Object number() {
      if (text.startsWith("NaN", pos)) { pos += 3; return Double.NaN; }
      if (text.startsWith("Infinity", pos)) { pos += 8; return Double.POSITIVE_INFINITY; }
      if (text.startsWith("-Infinity", pos)) { pos += 9; return Double.NEGATIVE_INFINITY; }
      int start = pos;
      while (pos < text.length() && "+-0123456789.eE".indexOf(text.charAt(pos)) >= 0) pos++;
      if (start == pos) throw new IllegalArgumentException("number");
      return new BigDecimal(text.substring(start, pos));
    }

    Object string() {
      StringBuilder out = new StringBuilder();
      pos++;
      while (pos < text.length()) {
        char c = text.charAt(pos++);
        if (c == '"') return out.toString();
        if (c != '\\') { out.append(c); continue; }
        char escape = text.charAt(pos++);
        switch (escape) {
          case '"': out.append('"'); break;
          case '\\': out.append('\\'); break;
          case '/': out.append('/'); break;
          case 'b': out.append('\b'); break;
          case 'f': out.append('\f'); break;
          case 'n': out.append('\n'); break;
          case 'r': out.append('\r'); break;
          case 't': out.append('\t'); break;
          case 'u':
            out.append((char) Integer.parseInt(text.substring(pos, pos + 4), 16));
            pos += 4;
            break;
          default: throw new IllegalArgumentException("escape");
        }
      }
      throw new IllegalArgumentException("unterminated string");
    }

    Object array() {
      List<Object> out = new ArrayList<>();
      pos++;
      space();
      if (pos < text.length() && text.charAt(pos) == ']') { pos++; return out; }
      while (true) {
        out.add(value());
        space();
        char c = text.charAt(pos++);
        if (c == ']') return out;
        if (c != ',') throw new IllegalArgumentException("array");
      }
    }

    Object object() {
      Map<String, Object> out = new LinkedHashMap<>();
      pos++;
      space();
      if (pos < text.length() && text.charAt(pos) == '}') { pos++; return out; }
      while (true) {
        space();
        String key = (String) string();
        space();
        if (text.charAt(pos++) != ':') throw new IllegalArgumentException("object");
        out.put(key, value());
        space();
        char c = text.charAt(pos++);
        if (c == '}') return out;
        if (c != ',') throw new IllegalArgumentException("object");
      }
    }
  }
"#,
    );
    for function in &parsed.functions {
        let params = function
            .params
            .iter()
            .map(|param| format!("{} {}", param.raw, param.name))
            .collect::<Vec<_>>()
            .join(", ");
        let names = function
            .params
            .iter()
            .map(|param| param.name.clone())
            .collect::<Vec<_>>()
            .join(", ");
        let throws = if function.throws.is_empty() {
            String::new()
        } else {
            format!(" throws {}", function.throws.join(", "))
        };
        let args = args_expression(&function.params);
        out.push_str(&format!(
            "\n  public static {} {}({}){} {{\n",
            function.returns_raw, function.name, params, throws
        ));
        out.push_str("\n    String cppanArgs = ");
        out.push_str(&args);
        out.push_str(";\n");
        out.push_str(&format!("    {} cppanValue;\n", function.returns_raw));
        out.push_str("    try {\n      cppanValue = ");
        out.push_str(&class);
        out.push('.');
        out.push_str(&function.name);
        out.push('(');
        out.push_str(&names);
        out.push_str(");\n    } catch (Throwable cppanError) {\n      checkError(\"");
        out.push_str(&function.name);
        out.push_str("\", cppanArgs, cppanError);\n      throw CppanShadow.<RuntimeException>sneak(cppanError);\n    }\n");
        out.push_str("    check(\"");
        out.push_str(&function.name);
        out.push_str("\", cppanArgs, \"{\\\"ok\\\":\" + ");
        out.push_str(&value_expression(
            &function.returns_ty,
            "cppanValue",
            &function.returns_raw,
        ));
        out.push_str(" + \"}\");\n    return cppanValue;\n  }\n");
    }
    out.push_str("}\n");

    let jni = r#"// Generated by cpp-anything shadow: the JNI entry point over the C ABI.
//
// Strings cross through JNI's modified UTF-8 (GetStringUTFChars/NewStringUTF): for
// ordinary text that is standard UTF-8, but U+0000 and supplementary characters
// (surrogate pairs) use a different encoding, so a JSON argument containing those
// must not be assumed byte-identical to the wire's UTF-8.
#include <jni.h>

extern "C" {
const char* cppan_shadow_call(const char* fn, const char* args_json);
void cppan_shadow_free(const char* ptr);
}

extern "C" JNIEXPORT jstring JNICALL Java_CppanShadow_call(JNIEnv* env, jclass, jstring fn,
                                                          jstring args) {
  if (fn == nullptr || args == nullptr) {
    return env->NewStringUTF(
        "{\"err\":{\"type\":\"TypeError\",\"msg\":\"missing function or arguments\"}}");
  }
  const char* fn_chars = env->GetStringUTFChars(fn, nullptr);
  const char* args_chars = env->GetStringUTFChars(args, nullptr);
  if (fn_chars == nullptr || args_chars == nullptr) {
    if (fn_chars != nullptr) env->ReleaseStringUTFChars(fn, fn_chars);
    if (args_chars != nullptr) env->ReleaseStringUTFChars(args, args_chars);
    return env->NewStringUTF(
        "{\"err\":{\"type\":\"OutOfMemoryError\",\"msg\":\"GetStringUTFChars\"}}");
  }
  const char* result = cppan_shadow_call(fn_chars, args_chars);
  env->ReleaseStringUTFChars(fn, fn_chars);
  env->ReleaseStringUTFChars(args, args_chars);
  if (result == nullptr) {
    return env->NewStringUTF("{\"err\":{\"type\":\"ShadowError\",\"msg\":\"no result\"}}");
  }
  jstring out = env->NewStringUTF(result);
  cppan_shadow_free(result);
  return out;
}
"#;

    let build = r#"#!/bin/sh
# Generated by cpp-anything shadow. Builds the JNI bridge against the port's
# libcppan_shadow.so (built by `cpp-anything build <project>`).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -n "${JAVA_HOME:-}" ] && [ -f "$JAVA_HOME/include/jni.h" ]; then
  java_home=$JAVA_HOME
else
  javac_path=$(command -v javac || true)
  if [ -z "$javac_path" ]; then
    echo "build.sh: javac is not on PATH and JAVA_HOME is unset" >&2
    exit 1
  fi
  resolved=$(readlink -f "$javac_path" 2>/dev/null || printf '%s' "$javac_path")
  java_home=$(dirname "$(dirname "$resolved")")
fi
platform=linux
if [ "$(uname -s)" = "Darwin" ]; then
  platform=darwin
fi
suffix=so
if [ "$(uname -s)" = "Darwin" ]; then
  suffix=dylib
fi
cxx=${CXX:-c++}
"$cxx" -std=c++17 -O2 -fPIC -shared \
  -I"$java_home/include" -I"$java_home/include/$platform" \
  "$here/cppan_shadow_jni.cpp" \
  -o "$here/libcppan_shadow_jni.$suffix" \
  -L"$here/../build" -lcppan_shadow -Wl,-rpath,"$here/../build"
printf 'built %s\n' "$here/libcppan_shadow_jni.$suffix"
"#;

    vec![
        ("shadow/CppanShadow.java".to_string(), out),
        ("shadow/cppan_shadow_jni.cpp".to_string(), jni.to_string()),
        ("shadow/build.sh".to_string(), build.to_string()),
    ]
}

fn args_expression(params: &[JavaParam]) -> String {
    if params.is_empty() {
        return "\"[]\"".to_string();
    }
    let mut out = String::from("\"[\"");
    for (index, param) in params.iter().enumerate() {
        if index == 0 {
            out.push_str(" + ");
        } else {
            out.push_str(" + \",\" + ");
        }
        out.push_str(&value_expression(&param.ty, &param.name, &param.raw));
    }
    out.push_str(" + \"]\"");
    out
}
