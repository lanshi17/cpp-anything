//! TypeScript/JavaScript: functions exported from a module. The analyzer reads
//! TypeScript annotations; the recorder runs node; the shadow binding is a
//! Node-API addon over the C ABI.
//!
//! The analyzer is a small, honest parser of the module surface, not a
//! TypeScript compiler: it tokenizes, skips comments and strings, and reads the
//! top-level `export function`, `export const ... = (...) =>`, `export const
//! ... = function` and `module.exports.name = ...` forms. Everything else is
//! reported in `skipped` with a reason, never guessed.

use std::path::{Path, PathBuf};

use crate::json::Json;
use crate::project::Project;
use crate::util;

use super::{Analysis, Found, FoundParam, Lang, RecordOptions};

// ---------------------------------------------------------------------------
// lexical layer
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind {
    Ident,
    Number,
    Str,
    Template,
    Regex,
    Punct,
}

#[derive(Clone, Debug)]
struct Tok {
    kind: Kind,
    text: String,
    line: u32,
    start: usize,
}

#[derive(Clone, Debug)]
struct Doc {
    end: usize,
    text: String,
}

fn is_ident_start(byte: u8) -> bool {
    byte.is_ascii_alphabetic() || byte == b'_' || byte == b'$' || byte >= 0x80
}

fn is_ident_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'$' || byte >= 0x80
}

/// Tokenizes the file. Strings, templates, comments and regular expressions are
/// single tokens or absent, so brace matching for bodies is not confused by the
/// text inside them.
fn tokenize(source: &str) -> (Vec<Tok>, Vec<Doc>) {
    let bytes = source.as_bytes();
    let mut tokens = Vec::new();
    let mut docs = Vec::new();
    let mut i = 0usize;
    let mut line = 1u32;
    while i < bytes.len() {
        let byte = bytes[i];
        if byte == b'\n' {
            line += 1;
            i += 1;
            continue;
        }
        if byte.is_ascii_whitespace() {
            i += 1;
            continue;
        }
        // comments
        if byte == b'/' && i + 1 < bytes.len() && bytes[i + 1] == b'/' {
            while i < bytes.len() && bytes[i] != b'\n' {
                i += 1;
            }
            continue;
        }
        if byte == b'/' && i + 1 < bytes.len() && bytes[i + 1] == b'*' {
            let is_doc = i + 2 < bytes.len() && bytes[i + 2] == b'*';
            i += 2;
            let body_start = i;
            while i + 1 < bytes.len() && !(bytes[i] == b'*' && bytes[i + 1] == b'/') {
                if bytes[i] == b'\n' {
                    line += 1;
                }
                i += 1;
            }
            let body_end = i.min(bytes.len());
            i = (i + 2).min(bytes.len());
            if is_doc {
                docs.push(Doc {
                    end: i,
                    text: clean_doc(&source[body_start..body_end]),
                });
            }
            continue;
        }
        let token_line = line;
        let start = i;
        // strings and templates
        if byte == b'\'' || byte == b'"' || byte == b'`' {
            let quote = byte;
            i += 1;
            while i < bytes.len() {
                if bytes[i] == b'\\' {
                    i = (i + 2).min(bytes.len());
                    continue;
                }
                if bytes[i] == quote {
                    i += 1;
                    break;
                }
                if bytes[i] == b'\n' {
                    line += 1;
                }
                i += 1;
            }
            tokens.push(Tok {
                kind: if quote == b'`' {
                    Kind::Template
                } else {
                    Kind::Str
                },
                text: source[start..i].to_string(),
                line: token_line,
                start,
            });
            continue;
        }
        // regular expressions: only where an expression may start
        if byte == b'/' && regex_can_start(tokens.last()) {
            i += 1;
            let mut in_class = false;
            while i < bytes.len() {
                match bytes[i] {
                    b'\\' => {
                        i = (i + 2).min(bytes.len());
                        continue;
                    }
                    b'[' => in_class = true,
                    b']' => in_class = false,
                    b'/' if !in_class => {
                        i += 1;
                        break;
                    }
                    b'\n' => break,
                    _ => {}
                }
                i += 1;
            }
            while i < bytes.len() && bytes[i].is_ascii_alphabetic() {
                i += 1;
            }
            tokens.push(Tok {
                kind: Kind::Regex,
                text: source[start..i].to_string(),
                line: token_line,
                start,
            });
            continue;
        }
        // numbers
        if byte.is_ascii_digit()
            || (byte == b'.' && i + 1 < bytes.len() && bytes[i + 1].is_ascii_digit())
        {
            i += 1;
            while i < bytes.len() {
                let b = bytes[i];
                if b.is_ascii_alphanumeric() || b == b'.' || b == b'_' {
                    i += 1;
                } else if (b == b'+' || b == b'-')
                    && i > start
                    && (bytes[i - 1] == b'e' || bytes[i - 1] == b'E')
                {
                    i += 1;
                } else {
                    break;
                }
            }
            tokens.push(Tok {
                kind: Kind::Number,
                text: source[start..i].to_string(),
                line: token_line,
                start,
            });
            continue;
        }
        // identifiers
        if is_ident_start(byte) {
            i += 1;
            while i < bytes.len() && is_ident_byte(bytes[i]) {
                i += 1;
            }
            tokens.push(Tok {
                kind: Kind::Ident,
                text: source[start..i].to_string(),
                line: token_line,
                start,
            });
            continue;
        }
        // punctuation, longest first
        let mut width = 1usize;
        for candidate in [
            "...", "=>", "?.", "??", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=",
            "**",
        ] {
            if source[start..].starts_with(candidate) {
                width = candidate.len();
                break;
            }
        }
        i += width;
        tokens.push(Tok {
            kind: Kind::Punct,
            text: source[start..i].to_string(),
            line: token_line,
            start,
        });
    }
    (tokens, docs)
}

fn regex_can_start(previous: Option<&Tok>) -> bool {
    let Some(previous) = previous else {
        return true;
    };
    match previous.kind {
        Kind::Ident => matches!(
            previous.text.as_str(),
            "return"
                | "typeof"
                | "instanceof"
                | "in"
                | "of"
                | "new"
                | "delete"
                | "void"
                | "case"
                | "yield"
                | "await"
        ),
        Kind::Number | Kind::Str | Kind::Template | Kind::Regex => false,
        Kind::Punct => !matches!(previous.text.as_str(), ")" | "]" | "}"),
    }
}

fn clean_doc(body: &str) -> String {
    let mut lines = Vec::new();
    for line in body.lines() {
        let trimmed = line.trim_start();
        let text = trimmed.strip_prefix('*').unwrap_or(trimmed);
        lines.push(text.trim_end().to_string());
    }
    while lines.first().is_some_and(|line| line.is_empty()) {
        lines.remove(0);
    }
    while lines.last().is_some_and(|line| line.is_empty()) {
        lines.pop();
    }
    lines
        .iter()
        .map(|line| line.trim_start().to_string())
        .collect::<Vec<_>>()
        .join("\n")
}

fn text_at(tokens: &[Tok], index: usize) -> &str {
    tokens.get(index).map_or("", |token| token.text.as_str())
}

/// Index of the token closing the bracket opened at `open` (`(`, `[`, `{` or `<`).
fn matching(tokens: &[Tok], open: usize) -> Option<usize> {
    let opener = text_at(tokens, open);
    let closer = match opener {
        "(" => ")",
        "[" => "]",
        "{" => "}",
        "<" => ">",
        _ => return None,
    };
    let mut depth = 0i32;
    for (index, token) in tokens.iter().enumerate().skip(open) {
        if token.text == opener {
            depth += 1;
        } else if token.text == closer {
            depth -= 1;
            if depth == 0 {
                return Some(index);
            }
        }
    }
    None
}

fn join_tokens(tokens: &[Tok], start: usize, end: usize) -> String {
    tokens
        .iter()
        .take(end)
        .skip(start)
        .map(|token| token.text.as_str())
        .collect::<Vec<_>>()
        .join("")
}

/// Splits `[start, end)` at top-level `separator` tokens, respecting all brackets
/// (including `<...>` so `Record<string, number>` stays one parameter).
fn split_ranges(tokens: &[Tok], start: usize, end: usize, separator: &str) -> Vec<(usize, usize)> {
    let mut ranges = Vec::new();
    let mut depth = 0i32;
    let mut piece = start;
    for index in start..end {
        match text_at(tokens, index) {
            "(" | "[" | "{" | "<" => depth += 1,
            ")" | "]" | "}" | ">" => depth -= 1,
            text if text == separator && depth == 0 => {
                ranges.push((piece, index));
                piece = index + 1;
            }
            _ => {}
        }
    }
    ranges.push((piece, end));
    ranges
}

fn skip_block(tokens: &[Tok], open: usize) -> usize {
    matching(tokens, open).map_or(tokens.len(), |close| close + 1)
}

fn is_declaration_start(text: &str) -> bool {
    matches!(
        text,
        "export"
            | "import"
            | "const"
            | "let"
            | "var"
            | "function"
            | "class"
            | "async"
            | "module"
            | "exports"
            | "type"
            | "interface"
            | "declare"
            | "@"
    )
}

/// Consumes a statement, including its braces, stopping at the top-level `;` or
/// at the first declaration-looking token on a later line (asi).
fn skip_statement(tokens: &[Tok], start: usize) -> usize {
    let start_line = tokens.get(start).map_or(0, |token| token.line);
    let mut depth = 0i32;
    let mut index = start;
    while index < tokens.len() {
        let text = text_at(tokens, index);
        match text {
            "(" | "[" | "{" => depth += 1,
            ")" | "]" | "}" => depth -= 1,
            ";" if depth == 0 => return index + 1,
            _ => {}
        }
        index += 1;
        if depth == 0
            && index < tokens.len()
            && tokens[index].line > start_line
            && is_declaration_start(text_at(tokens, index))
        {
            return index;
        }
    }
    index
}

fn skip_expression(tokens: &[Tok], start: usize) -> usize {
    skip_statement(tokens, start)
}

/// After a function's `)`: skips the return annotation and the body.
fn after_signature(tokens: &[Tok], close: usize) -> usize {
    let mut index = close + 1;
    let mut depth = 0i32;
    while index < tokens.len() {
        match text_at(tokens, index) {
            "(" | "[" | "<" => depth += 1,
            ")" | "]" | ">" => depth -= 1,
            "{" if depth == 0 => return skip_block(tokens, index),
            ";" if depth == 0 => return index + 1,
            "=>" if depth == 0 => return skip_expression(tokens, index + 1),
            _ => {}
        }
        index += 1;
    }
    index
}

fn skip_class(tokens: &[Tok], start: usize) -> usize {
    let mut index = start;
    while index < tokens.len() {
        match text_at(tokens, index) {
            "{" => return skip_block(tokens, index),
            ";" => return index + 1,
            _ => index += 1,
        }
    }
    index
}

// ---------------------------------------------------------------------------
// types
// ---------------------------------------------------------------------------

fn strip_outer_parens(text: &str) -> &str {
    let text = text.trim();
    if text.starts_with('(') && text.ends_with(')') {
        let inner = &text[1..text.len() - 1];
        let mut depth = 0i32;
        for ch in inner.chars() {
            match ch {
                '(' | '[' | '{' | '<' => depth += 1,
                ')' | ']' | '}' | '>' => depth -= 1,
                _ => {}
            }
            if depth < 0 {
                return text;
            }
        }
        if depth == 0 {
            return strip_outer_parens(inner);
        }
    }
    text
}

fn generic_parts(text: &str) -> Option<(&str, &str)> {
    let open = text.find('<')?;
    if !text.ends_with('>') {
        return None;
    }
    let name = text[..open].trim();
    let inner = &text[open + 1..text.len() - 1];
    if name
        .chars()
        .next()
        .is_some_and(|ch| ch.is_ascii_alphabetic() || ch == '_' || ch == '$')
    {
        Some((name, inner))
    } else {
        None
    }
}

/// Maps a TypeScript type onto the dialect `Ty::parse` understands. Unknown
/// types (interfaces, classes, generics, object literals) become `Any`: they
/// cross the wire as JSON.
fn ts_type(raw: &str) -> String {
    let text = raw.trim();
    if text.is_empty() {
        return "Any".into();
    }
    let union = crate::types::split_top_level(text, '|');
    if union.len() > 1 {
        let mut nullable = false;
        let mut members: Vec<&str> = Vec::new();
        for member in union {
            let member = member.trim();
            match member {
                "null" | "undefined" => nullable = true,
                "" => {}
                other => members.push(other),
            }
        }
        if members.len() == 1 {
            let inner = ts_type(members[0]);
            if nullable {
                return format!("Optional[{inner}]");
            }
            return inner;
        }
        return "Any".into();
    }
    let text = strip_outer_parens(text);
    if let Some(inner) = text.strip_suffix("[]") {
        return format!("list[{}]", ts_type(inner));
    }
    if let Some((name, inner)) = generic_parts(text) {
        return match name {
            "Array" | "ReadonlyArray" => format!("list[{}]", ts_type(inner)),
            "Record" => {
                let args = crate::types::split_top_level(inner, ',');
                if args.len() == 2 && args[0].trim() == "string" {
                    format!("dict[{}]", ts_type(args[1]))
                } else {
                    "Any".into()
                }
            }
            _ => "Any".into(),
        };
    }
    match text {
        "string" => "str",
        "number" => "float",
        "boolean" => "bool",
        "null" => "None",
        _ => "Any",
    }
    .into()
}

fn optional(annotation: String) -> String {
    if annotation.starts_with("Optional[") || annotation == "Any" {
        annotation
    } else {
        format!("Optional[{annotation}]")
    }
}

/// Names a generated `export function` can declare: identifiers minus the
/// reserved words. Property names read from CommonJS can be neither.
fn is_binding_name(name: &str) -> bool {
    let mut chars = name.chars();
    let Some(first) = chars.next() else {
        return false;
    };
    if !(first.is_ascii_alphabetic() || first == '_' || first == '$') {
        return false;
    }
    if !chars.all(|ch| ch.is_ascii_alphanumeric() || ch == '_' || ch == '$') {
        return false;
    }
    !matches!(
        name,
        "await"
            | "break"
            | "case"
            | "catch"
            | "class"
            | "const"
            | "continue"
            | "debugger"
            | "default"
            | "delete"
            | "do"
            | "else"
            | "enum"
            | "export"
            | "extends"
            | "false"
            | "finally"
            | "for"
            | "function"
            | "if"
            | "import"
            | "in"
            | "instanceof"
            | "let"
            | "new"
            | "null"
            | "return"
            | "static"
            | "super"
            | "switch"
            | "this"
            | "throw"
            | "true"
            | "try"
            | "typeof"
            | "var"
            | "void"
            | "while"
            | "with"
            | "yield"
    )
}

/// A JavaScript literal used as a default value, in the JSON the wire carries.
fn js_literal(text: &str) -> Option<Json> {
    let text = text.trim();
    if text.is_empty() {
        return None;
    }
    if text == "null" {
        return Some(Json::Null);
    }
    if text == "true" {
        return Some(Json::Bool(true));
    }
    if text == "false" {
        return Some(Json::Bool(false));
    }
    if text == "undefined" || text == "NaN" || text == "Infinity" {
        return None;
    }
    if (text.starts_with('"') && text.ends_with('"'))
        || (text.starts_with('\'') && text.ends_with('\''))
    {
        return unquote(text).map(Json::Str);
    }
    if text.starts_with('[') && text.ends_with(']') {
        let inner = &text[1..text.len() - 1];
        let mut items = Vec::new();
        for part in crate::types::split_top_level(inner, ',') {
            if part.trim().is_empty() {
                continue;
            }
            items.push(js_literal(part)?);
        }
        return Some(Json::Array(items));
    }
    if text.starts_with('{') && text.ends_with('}') {
        let inner = &text[1..text.len() - 1];
        let mut pairs = Vec::new();
        for part in crate::types::split_top_level(inner, ',') {
            let part = part.trim();
            if part.is_empty() {
                continue;
            }
            let (key, value) = part.split_once(':')?;
            let key = key.trim();
            let key = unquote(key).unwrap_or_else(|| key.to_string());
            pairs.push((key, js_literal(value)?));
        }
        return Some(Json::obj(pairs));
    }
    let cleaned = text.replace('_', "");
    if !cleaned.contains('.') && !cleaned.contains('e') && !cleaned.contains('E') {
        if let Ok(value) = cleaned.parse::<i64>() {
            return Some(Json::Int(value));
        }
    }
    cleaned
        .parse::<f64>()
        .ok()
        .filter(|value| value.is_finite())
        .map(Json::Real)
}

fn unquote(text: &str) -> Option<String> {
    let quote = text.chars().next()?;
    if !matches!(quote, '"' | '\'') || !text.ends_with(quote) || text.len() < 2 {
        return None;
    }
    let mut out = String::new();
    let mut chars = text[1..text.len() - 1].chars();
    while let Some(ch) = chars.next() {
        if ch != '\\' {
            out.push(ch);
            continue;
        }
        match chars.next()? {
            'n' => out.push('\n'),
            'r' => out.push('\r'),
            't' => out.push('\t'),
            'b' => out.push('\u{08}'),
            'f' => out.push('\u{0c}'),
            'v' => out.push('\u{0b}'),
            '0' => out.push('\0'),
            '\\' => out.push('\\'),
            '\'' => out.push('\''),
            '"' => out.push('"'),
            '`' => out.push('`'),
            'u' => {
                let hex: String = chars.by_ref().take(4).collect();
                let value = u32::from_str_radix(&hex, 16).ok()?;
                out.push(char::from_u32(value)?);
            }
            other => out.push(other),
        }
    }
    Some(out)
}

// ---------------------------------------------------------------------------
// declarations
// ---------------------------------------------------------------------------

enum Outcome {
    Found(Found, usize),
    Skipped(String, String, usize),
    None(usize),
}

fn clone_found(found: &Found) -> Found {
    Found {
        name: found.name.clone(),
        line: found.line,
        call: found.call.clone(),
        params: found
            .params
            .iter()
            .map(|param| FoundParam {
                name: param.name.clone(),
                ty: param.ty.clone(),
                has_default: param.has_default,
                default: param.default.clone(),
            })
            .collect(),
        returns: found.returns.clone(),
        doc: found.doc.clone(),
    }
}

struct Parser<'a> {
    source: &'a str,
    tokens: Vec<Tok>,
    docs: Vec<Doc>,
    functions: Vec<Found>,
    /// Function-valued declarations that were not exported where they were
    /// defined; `module.exports.name = local` can promote one later.
    locals: Vec<(String, Found)>,
    /// `(exported name, local name)` from CommonJS assignments, resolved after
    /// the whole file has been read.
    promotions: Vec<(String, String)>,
    skipped: Vec<(String, String)>,
    imports: Vec<String>,
    lines: u32,
}

impl<'a> Parser<'a> {
    fn doc(&self, byte: usize) -> String {
        let Some(doc) = self.docs.iter().rev().find(|doc| doc.end <= byte) else {
            return String::new();
        };
        if !self.source[doc.end..byte].trim().is_empty() {
            return String::new();
        }
        doc.text.clone()
    }

    fn parse_params(&self, open: usize, close: usize) -> Result<Vec<FoundParam>, String> {
        let mut params = Vec::new();
        for (start, end) in split_ranges(&self.tokens, open + 1, close, ",") {
            if start >= end {
                continue;
            }
            let name_token = &self.tokens[start];
            if name_token.text == "..." {
                return Err("a rest parameter (`...args`) has no fixed signature".into());
            }
            if name_token.text == "{" || name_token.text == "[" {
                return Err("a destructured parameter has no name the harness can pass".into());
            }
            if name_token.kind != Kind::Ident {
                return Err(format!(
                    "`{}` is not a plain parameter name",
                    name_token.text
                ));
            }
            let name = name_token.text.clone();
            if name == "this" {
                return Err("a `this` parameter exists only at compile time".into());
            }
            let mut index = start + 1;
            let mut is_optional = false;
            let mut annotation = "Any".to_string();
            if text_at(&self.tokens, index) == "?" {
                is_optional = true;
                index += 1;
            }
            if text_at(&self.tokens, index) == ":" {
                index += 1;
                let type_start = index;
                let mut depth = 0i32;
                while index < end {
                    match text_at(&self.tokens, index) {
                        "(" | "[" | "{" | "<" => depth += 1,
                        ")" | "]" | "}" | ">" => depth -= 1,
                        "=" if depth == 0 => break,
                        _ => {}
                    }
                    index += 1;
                }
                annotation = ts_type(&join_tokens(&self.tokens, type_start, index));
            }
            let mut default = None;
            let mut has_default = false;
            if text_at(&self.tokens, index) == "=" {
                has_default = true;
                let value_text = join_tokens(&self.tokens, index + 1, end);
                default = js_literal(&value_text);
                index = end;
            }
            if index < end {
                return Err(format!(
                    "the parameter `{name}` has trailing syntax the analyzer does not read"
                ));
            }
            if is_optional {
                annotation = optional(annotation);
            }
            params.push(FoundParam {
                name,
                ty: annotation,
                has_default,
                default,
            });
        }
        Ok(params)
    }

    fn parse_function(
        &self,
        start: usize,
        forced_name: Option<&str>,
    ) -> Result<(Found, usize), (String, String, usize)> {
        let mut index = start;
        let mut is_async = false;
        if text_at(&self.tokens, index) == "async" {
            is_async = true;
            index += 1;
        }
        if text_at(&self.tokens, index) != "function" {
            let next = skip_statement(&self.tokens, start);
            return Err((
                "anonymous".into(),
                "not a function declaration".into(),
                next,
            ));
        }
        index += 1;
        let mut is_generator = false;
        if text_at(&self.tokens, index) == "*" {
            is_generator = true;
            index += 1;
        }
        let forced = forced_name.map(str::to_string);
        let mut name = forced.clone();
        if name.is_none() {
            if self
                .tokens
                .get(index)
                .is_some_and(|token| token.kind == Kind::Ident)
            {
                name = Some(self.tokens[index].text.clone());
                index += 1;
            }
        }
        let Some(name) = name else {
            let next = after_signature(&self.tokens, index);
            return Err((
                "anonymous".into(),
                "an anonymous function has no export name".into(),
                next,
            ));
        };
        if text_at(&self.tokens, index) == "<" {
            index = matching(&self.tokens, index).map_or(index, |close| close + 1);
        }
        let body_next = if text_at(&self.tokens, index) == "(" {
            matching(&self.tokens, index)
                .map_or(index, |close| after_signature(&self.tokens, close))
        } else {
            skip_statement(&self.tokens, start)
        };
        if is_async {
            return Err((
                name,
                "an async function returns a Promise, which the wire does not carry".into(),
                body_next,
            ));
        }
        if is_generator {
            return Err((
                name,
                "a generator function returns an iterator, which the wire does not carry".into(),
                body_next,
            ));
        }
        if text_at(&self.tokens, index) != "(" {
            return Err((
                name,
                "a function without a parameter list".into(),
                body_next,
            ));
        }
        let close = match matching(&self.tokens, index) {
            Some(close) => close,
            None => return Err((name, "an unclosed parameter list".into(), body_next)),
        };
        let params = match self.parse_params(index, close) {
            Ok(params) => params,
            Err(reason) => return Err((name, reason, after_signature(&self.tokens, close))),
        };
        let mut cursor = close + 1;
        let mut returns = "Any".to_string();
        if text_at(&self.tokens, cursor) == ":" {
            cursor += 1;
            if text_at(&self.tokens, cursor) == "{" {
                // An object/interface return type: no wire form, and its braces
                // are not the body.
                cursor = matching(&self.tokens, cursor).map_or(cursor, |end| end + 1);
            } else {
                let type_start = cursor;
                let mut depth = 0i32;
                while cursor < self.tokens.len() {
                    let text = text_at(&self.tokens, cursor);
                    match text {
                        "{" | ";" | "=>" if depth == 0 => break,
                        "(" | "[" | "{" | "<" => depth += 1,
                        ")" | "]" | "}" | ">" => depth -= 1,
                        _ => {}
                    }
                    cursor += 1;
                }
                returns = ts_type(&join_tokens(&self.tokens, type_start, cursor));
            }
        }
        if text_at(&self.tokens, cursor) == ";" {
            return Err((
                name,
                "an overload without a body: the implementation is the same name later".into(),
                cursor + 1,
            ));
        }
        if text_at(&self.tokens, cursor) != "{" {
            return Err((
                name,
                "a function declaration without a body".into(),
                after_signature(&self.tokens, cursor),
            ));
        }
        let next = skip_block(&self.tokens, cursor);
        let line = self.tokens[start].line;
        let doc = self.doc(self.tokens[start].start);
        Ok((
            Found {
                name: name.clone(),
                line,
                call: name,
                params,
                returns,
                doc,
            },
            next,
        ))
    }

    fn parse_arrow(
        &self,
        start: usize,
        name: &str,
    ) -> Result<(Found, usize), (String, String, usize)> {
        let mut index = start;
        let mut is_async = false;
        if text_at(&self.tokens, index) == "async" {
            is_async = true;
            index += 1;
        }
        if text_at(&self.tokens, index) == "<" {
            index = matching(&self.tokens, index).map_or(index, |close| close + 1);
        }
        let single_ident = self.tokens.get(index).is_some_and(|token| {
            token.kind == Kind::Ident && text_at(&self.tokens, index + 1) == "=>"
        });
        let (open, close) = if text_at(&self.tokens, index) == "(" {
            let close = matching(&self.tokens, index).map_or(index, |close| close);
            (Some(index), Some(close))
        } else if single_ident {
            (None, None)
        } else {
            let next = skip_statement(&self.tokens, start);
            return Err((
                name.to_string(),
                "an arrow whose parameters are not a parenthesized list".into(),
                next,
            ));
        };
        let params = match (open, close) {
            (Some(open), Some(close)) => match self.parse_params(open, close) {
                Ok(params) => params,
                Err(reason) => {
                    return Err((
                        name.to_string(),
                        reason,
                        after_signature(&self.tokens, close),
                    ))
                }
            },
            _ => vec![FoundParam {
                name: self.tokens[index].text.clone(),
                ty: "Any".into(),
                has_default: false,
                default: None,
            }],
        };
        let mut cursor = close.map_or(index + 1, |close| close + 1);
        let mut returns = "Any".to_string();
        if text_at(&self.tokens, cursor) == ":" {
            cursor += 1;
            if text_at(&self.tokens, cursor) == "{" {
                cursor = matching(&self.tokens, cursor).map_or(cursor, |end| end + 1);
            } else {
                let type_start = cursor;
                let mut depth = 0i32;
                while cursor < self.tokens.len() {
                    let text = text_at(&self.tokens, cursor);
                    match text {
                        "(" | "[" | "{" | "<" => depth += 1,
                        ")" | "]" | "}" | ">" => depth -= 1,
                        "=>" if depth == 0 => break,
                        _ => {}
                    }
                    cursor += 1;
                }
                returns = ts_type(&join_tokens(&self.tokens, type_start, cursor));
            }
        }
        if is_async {
            return Err((
                name.to_string(),
                "an async function returns a Promise, which the wire does not carry".into(),
                skip_statement(&self.tokens, cursor),
            ));
        }
        if text_at(&self.tokens, cursor) != "=>" {
            let next = skip_statement(&self.tokens, cursor);
            return Err((
                name.to_string(),
                "a value that is not an arrow function".into(),
                next,
            ));
        }
        cursor += 1;
        let next = if text_at(&self.tokens, cursor) == "{" {
            skip_block(&self.tokens, cursor)
        } else {
            skip_expression(&self.tokens, cursor)
        };
        let line = self.tokens[start].line;
        let doc = self.doc(self.tokens[start].start);
        Ok((
            Found {
                name: name.to_string(),
                line,
                call: name.to_string(),
                params,
                returns,
                doc,
            },
            next,
        ))
    }

    /// A function-valued initializer: `function ...`, `async ...`, `(...) =>`.
    fn parse_value(&self, start: usize, name: &str) -> Outcome {
        match text_at(&self.tokens, start) {
            "function" | "async" => match self.parse_function(start, Some(name)) {
                Ok((found, next)) => Outcome::Found(found, next),
                Err((_name, reason, next)) => Outcome::Skipped(name.to_string(), reason, next),
            },
            "(" | "<" => match self.parse_arrow(start, name) {
                Ok((found, next)) => Outcome::Found(found, next),
                Err((_name, reason, next)) => Outcome::Skipped(name.to_string(), reason, next),
            },
            text if text == name => {
                // single-parameter arrow: `name => ...`
                if text_at(&self.tokens, start + 1) == "=>" {
                    match self.parse_arrow(start, name) {
                        Ok((found, next)) => Outcome::Found(found, next),
                        Err((_name, reason, next)) => {
                            Outcome::Skipped(name.to_string(), reason, next)
                        }
                    }
                } else {
                    Outcome::None(skip_statement(&self.tokens, start))
                }
            }
            _ => Outcome::None(skip_statement(&self.tokens, start)),
        }
    }

    fn parse_var(&self, start: usize) -> Outcome {
        let mut index = start + 1;
        if self.tokens.get(index).map(|token| token.kind) != Some(Kind::Ident) {
            return Outcome::None(skip_statement(&self.tokens, start));
        }
        let name = self.tokens[index].text.clone();
        index += 1;
        if text_at(&self.tokens, index) == ":" {
            // explicit variable type; the initializer still decides the shape
            let mut depth = 0i32;
            while index < self.tokens.len() {
                match text_at(&self.tokens, index) {
                    "(" | "[" | "{" | "<" => depth += 1,
                    ")" | "]" | "}" | ">" => depth -= 1,
                    "=" if depth == 0 => break,
                    ";" if depth == 0 => return Outcome::None(index + 1),
                    _ => {}
                }
                index += 1;
            }
        }
        if text_at(&self.tokens, index) != "=" {
            return Outcome::None(skip_statement(&self.tokens, start));
        }
        self.parse_value(index + 1, &name)
    }

    fn parse_module_exports(&mut self, start: usize) -> Outcome {
        // `module . exports . name = value`
        let mut index = start;
        if text_at(&self.tokens, index) == "module" {
            if text_at(&self.tokens, index + 1) != "."
                || text_at(&self.tokens, index + 2) != "exports"
            {
                return Outcome::None(skip_statement(&self.tokens, start));
            }
            index += 3;
            if text_at(&self.tokens, index) != "." {
                return Outcome::Skipped(
                    "module.exports".into(),
                    "an object-literal export: only `module.exports.name = ...` is read".into(),
                    skip_statement(&self.tokens, start),
                );
            }
            index += 1;
        } else {
            // `exports . name = value`
            if text_at(&self.tokens, index + 1) != "." {
                return Outcome::None(skip_statement(&self.tokens, start));
            }
            index += 2;
        }
        if self.tokens.get(index).map(|token| token.kind) != Some(Kind::Ident) {
            return Outcome::Skipped(
                "module.exports".into(),
                "a computed export name".into(),
                skip_statement(&self.tokens, start),
            );
        }
        let name = self.tokens[index].text.clone();
        index += 1;
        if !is_binding_name(&name) {
            return Outcome::Skipped(
                name,
                "an exported name that is not a JavaScript binding identifier".into(),
                skip_statement(&self.tokens, start),
            );
        }
        if text_at(&self.tokens, index) != "=" {
            return Outcome::Skipped(
                name,
                "an export assignment the analyzer does not read".into(),
                skip_statement(&self.tokens, start),
            );
        }
        // `module.exports.name = local`: the signature is the local declaration,
        // which the scan has already read; promote it after the whole file.
        let value = index + 1;
        if self.tokens.get(value).map(|token| token.kind) == Some(Kind::Ident) {
            let after = value + 1;
            let ends = self
                .tokens
                .get(after)
                .is_none_or(|token| token.text == ";" || token.line > self.tokens[value].line);
            if ends {
                self.promotions
                    .push((name, self.tokens[value].text.clone()));
                return Outcome::None(if text_at(&self.tokens, after) == ";" {
                    after + 1
                } else {
                    after
                });
            }
        }
        self.parse_value(index + 1, &name)
    }

    fn parse_import(&mut self, start: usize) -> usize {
        let end = skip_statement(&self.tokens, start);
        let mut specifier = None;
        let mut index = start + 1;
        while index < end {
            if self.tokens[index].kind == Kind::Str {
                specifier = unquote(&self.tokens[index].text);
            }
            index += 1;
        }
        if let Some(specifier) = specifier {
            self.imports.push(specifier);
        }
        end
    }
}

impl<'a> Parser<'a> {
    fn new(source: &'a str) -> Parser<'a> {
        let (tokens, docs) = tokenize(source);
        let lines = source.lines().count() as u32;
        Parser {
            source,
            tokens,
            docs,
            functions: Vec::new(),
            locals: Vec::new(),
            promotions: Vec::new(),
            skipped: Vec::new(),
            imports: Vec::new(),
            lines,
        }
    }
}
impl<'a> Parser<'a> {
    fn record(&mut self, outcome: Outcome, exported: bool, declaration: usize) -> usize {
        match outcome {
            Outcome::Found(mut found, next) => {
                if found.doc.is_empty() {
                    if let Some(token) = self.tokens.get(declaration) {
                        found.doc = self.doc(token.start);
                    }
                }
                if exported {
                    self.functions.push(found);
                } else {
                    self.skipped
                        .push((found.name.clone(), "not exported from the module".into()));
                    self.locals.push((found.name.clone(), found));
                }
                next
            }
            Outcome::Skipped(name, reason, next) => {
                self.skipped.push((name, reason));
                next
            }
            Outcome::None(next) => next,
        }
    }

    fn resolve_promotions(&mut self) {
        for (exported, local) in std::mem::take(&mut self.promotions) {
            let promoted = self
                .locals
                .iter()
                .find(|(name, _)| *name == local)
                .map(|(_, found)| clone_found(found));
            match promoted {
                Some(mut found) => {
                    found.name = exported.clone();
                    found.call = exported.clone();
                    self.skipped.retain(|(name, reason)| {
                        !(*name == local && reason == "not exported from the module")
                    });
                    if !self.functions.iter().any(|other| other.name == exported) {
                        self.functions.push(found);
                    }
                }
                None => {
                    if !self.functions.iter().any(|other| other.name == exported) {
                        self.skipped.push((
                            exported,
                            "an exported binding whose signature is imported or defined elsewhere"
                                .into(),
                        ));
                    }
                }
            }
        }
    }

    fn scan(&mut self) {
        let mut index = 0usize;
        while index < self.tokens.len() {
            let text = self.tokens[index].text.clone();
            match text.as_str() {
                "import" => {
                    index = self.parse_import(index);
                }
                "require" => {
                    if text_at(&self.tokens, index + 1) == "("
                        && self.tokens.get(index + 2).map(|token| token.kind) == Some(Kind::Str)
                        && text_at(&self.tokens, index + 3) == ")"
                    {
                        if let Some(specifier) = unquote(&self.tokens[index + 2].text) {
                            self.imports.push(specifier);
                        }
                        index += 4;
                    } else {
                        index += 1;
                    }
                }
                "export" => {
                    index = self.scan_export(index);
                }
                "module"
                    if text_at(&self.tokens, index + 1) == "."
                        && text_at(&self.tokens, index + 2) == "exports" =>
                {
                    let outcome = self.parse_module_exports(index);
                    index = self.record(outcome, true, index);
                }
                "exports" if text_at(&self.tokens, index + 1) == "." => {
                    let outcome = self.parse_module_exports(index);
                    index = self.record(outcome, true, index);
                }
                "function" | "async" => {
                    let outcome = match self.parse_function(index, None) {
                        Ok((found, next)) => Outcome::Found(found, next),
                        Err((name, reason, next)) => Outcome::Skipped(name, reason, next),
                    };
                    index = self.record(outcome, false, index);
                }
                "class" => {
                    let name = self
                        .tokens
                        .get(index + 1)
                        .filter(|token| token.kind == Kind::Ident)
                        .map_or("class".to_string(), |token| token.text.clone());
                    self.skipped
                        .push((name, "a class: its methods are not module exports".into()));
                    index = skip_class(&self.tokens, index + 1);
                }
                "abstract" if text_at(&self.tokens, index + 1) == "class" => {
                    let name = self
                        .tokens
                        .get(index + 2)
                        .filter(|token| token.kind == Kind::Ident)
                        .map_or("class".to_string(), |token| token.text.clone());
                    self.skipped
                        .push((name, "a class: its methods are not module exports".into()));
                    index = skip_class(&self.tokens, index + 2);
                }
                "declare" => {
                    let mut name_token = index + 1;
                    if matches!(
                        text_at(&self.tokens, name_token),
                        "function" | "const" | "let" | "var" | "class"
                    ) {
                        name_token += 1;
                    }
                    let name = self
                        .tokens
                        .get(name_token)
                        .filter(|token| token.kind == Kind::Ident)
                        .map_or("declare".to_string(), |token| token.text.clone());
                    self.skipped
                        .push((name, "an ambient declaration has no runtime body".into()));
                    index = skip_statement(&self.tokens, index);
                }
                "const" | "let" | "var" => {
                    let outcome = self.parse_var(index);
                    index = self.record(outcome, false, index);
                }
                "@" => {
                    let mut cursor = index;
                    while text_at(&self.tokens, cursor) == "@" {
                        cursor = skip_decorator(&self.tokens, cursor);
                    }
                    let name = self
                        .tokens
                        .get(cursor + 1)
                        .filter(|token| token.kind == Kind::Ident)
                        .map_or("decorated".to_string(), |token| token.text.clone());
                    self.skipped.push((
                        name,
                        "a decorated declaration: decorators are outside the supported subset"
                            .into(),
                    ));
                    index = skip_class(&self.tokens, cursor);
                }
                "(" | "[" | "{" => {
                    index = matching(&self.tokens, index).map_or(index + 1, |close| close + 1);
                }
                _ => index += 1,
            }
        }
    }

    fn scan_export(&mut self, index: usize) -> usize {
        let after = index + 1;
        match text_at(&self.tokens, after) {
            "function" | "async" => {
                let outcome = match self.parse_function(after, None) {
                    Ok((found, next)) => Outcome::Found(found, next),
                    Err((name, reason, next)) => Outcome::Skipped(name, reason, next),
                };
                self.record(outcome, true, index)
            }
            "const" | "let" | "var" => {
                let outcome = self.parse_var(after);
                self.record(outcome, true, index)
            }
            "class" => {
                let name = self
                    .tokens
                    .get(after + 1)
                    .filter(|token| token.kind == Kind::Ident)
                    .map_or("class".to_string(), |token| token.text.clone());
                self.skipped
                    .push((name, "a class: its methods are not module exports".into()));
                skip_class(&self.tokens, after + 1)
            }
            "abstract" if text_at(&self.tokens, after + 1) == "class" => {
                let name = self
                    .tokens
                    .get(after + 2)
                    .filter(|token| token.kind == Kind::Ident)
                    .map_or("class".to_string(), |token| token.text.clone());
                self.skipped
                    .push((name, "a class: its methods are not module exports".into()));
                skip_class(&self.tokens, after + 2)
            }
            "declare" => {
                let mut name_token = after + 1;
                if matches!(
                    text_at(&self.tokens, name_token),
                    "function" | "const" | "let" | "var" | "class"
                ) {
                    name_token += 1;
                }
                let name = self
                    .tokens
                    .get(name_token)
                    .filter(|token| token.kind == Kind::Ident)
                    .map_or("declare".to_string(), |token| token.text.clone());
                self.skipped
                    .push((name, "an ambient declaration has no runtime body".into()));
                skip_statement(&self.tokens, after)
            }
            "default" => {
                let mut name_token = after + 1;
                if matches!(text_at(&self.tokens, name_token), "function" | "class") {
                    name_token += 1;
                }
                let name = self
                    .tokens
                    .get(name_token)
                    .filter(|token| token.kind == Kind::Ident)
                    .map_or("default".to_string(), |token| token.text.clone());
                self.skipped.push((
                    name,
                    "a default export has no stable name the harness can call".into(),
                ));
                if text_at(&self.tokens, after + 1) == "function" {
                    skip_block_or_statement(&self.tokens, after + 2)
                } else if text_at(&self.tokens, after + 1) == "class" {
                    skip_class(&self.tokens, after + 2)
                } else {
                    skip_statement(&self.tokens, after + 1)
                }
            }
            "{" => {
                self.skipped.push((
                    "export { ... }".into(),
                    "an export list: the signature is not restated here".into(),
                ));
                skip_block(&self.tokens, after)
            }
            "*" => {
                self.skipped.push((
                    "export *".into(),
                    "a re-export: the functions live in another module".into(),
                ));
                skip_statement(&self.tokens, after)
            }
            "type" | "interface" | "enum" | "namespace" => skip_statement(&self.tokens, after),
            _ => skip_statement(&self.tokens, after),
        }
    }
}

fn skip_decorator(tokens: &[Tok], start: usize) -> usize {
    // `@name`, `@name(...)`, `@ns.name`
    let mut index = start + 1;
    while index < tokens.len() && (tokens[index].kind == Kind::Ident || tokens[index].text == ".") {
        index += 1;
    }
    if text_at(tokens, index) == "(" {
        return skip_block(tokens, index);
    }
    index
}

fn skip_block_or_statement(tokens: &[Tok], start: usize) -> usize {
    let mut index = start;
    while index < tokens.len() {
        match text_at(tokens, index) {
            "{" => return skip_block(tokens, index),
            ";" => return index + 1,
            _ => index += 1,
        }
    }
    index
}

pub fn analyze(path: &Path, _lang: Lang) -> Result<Analysis, String> {
    let source = util::read(path)?;
    let mut parser = Parser::new(&source);
    parser.scan();
    parser.resolve_promotions();
    Ok(Analysis {
        functions: parser.functions,
        skipped: parser.skipped,
        imports: parser.imports,
        lines: parser.lines,
    })
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

/// Where the recorder (and the shadow module) imports the original from.
enum Specifier {
    File(PathBuf),
    Bare(String),
}

fn specifier(project: &Project, module: Option<&Path>, resolve_from: Option<&Path>) -> Specifier {
    let Some(module) = module else {
        return Specifier::File(project.source_file.clone());
    };
    if module.is_absolute() {
        return Specifier::File(module.to_path_buf());
    }
    let bases: Vec<PathBuf> = [
        resolve_from.map(Path::to_path_buf),
        project.package_root.clone(),
        Some(project.dir.clone()),
    ]
    .into_iter()
    .flatten()
    .collect();
    for base in &bases {
        let candidate = base.join(module);
        if candidate.exists() {
            return Specifier::File(candidate);
        }
    }
    if module.components().count() > 1 || module.display().to_string().starts_with('.') {
        return Specifier::File(project.dir.join(module));
    }
    Specifier::Bare(module.display().to_string())
}

fn specifier_js(specifier: &Specifier) -> String {
    let path = match specifier {
        Specifier::File(path) => path.display().to_string(),
        Specifier::Bare(name) => return name.clone(),
    };
    // The harness turns an absolute path into a file URL; a bare specifier is
    // resolved by node from the cwd/NODE_PATH the recorder passes.
    path
}

fn target_is_typescript(specifier: &Specifier) -> bool {
    matches!(
        specifier,
        Specifier::File(path)
            if path
                .extension()
                .and_then(|ext| ext.to_str())
                .is_some_and(|ext| matches!(ext, "ts" | "mts" | "cts"))
    )
}

fn node_environment(resolve_from: Option<&Path>) -> Vec<(String, String)> {
    let mut envs = Vec::new();
    if let Some(path) = resolve_from {
        // `NODE_PATH` entries are searched as module roots, not as parents that
        // contain `node_modules`: include both so `--resolve-from <project>` works
        // whether the caller means the directory itself or its package directory.
        let mut paths = vec![path.to_path_buf()];
        let modules = path.join("node_modules");
        if modules.is_dir() {
            paths.push(modules);
        }
        if let Ok(joined) = std::env::join_paths(paths) {
            envs.push((
                "NODE_PATH".to_string(),
                joined.to_string_lossy().into_owned(),
            ));
        }
    }
    envs
}

fn run_node(
    args: &[String],
    cwd: Option<&Path>,
    envs: &[(String, String)],
) -> Result<String, String> {
    let run = util::run_with_env("node", args, cwd, envs);
    if !run.ok {
        return Err(format!(
            "the node recorder failed: {}\n{}",
            run.failure(),
            util::indent(&run.stderr, 2)
        ));
    }
    Ok(run.stdout)
}

fn run_node_with_fallback(
    project: &Project,
    options: &RecordOptions,
    harness: &Path,
    specifier: &Specifier,
) -> Result<String, String> {
    let cwd = options
        .resolve_from
        .clone()
        .or_else(|| project.package_root.clone())
        .or_else(|| Some(project.dir.clone()));
    let envs = node_environment(options.resolve_from.as_deref());
    let mut args = options.node_args.clone();
    args.push(harness.display().to_string());
    let first = match run_node(&args, cwd.as_deref(), &envs) {
        Ok(output) => return Ok(output),
        Err(error) => error,
    };
    if !target_is_typescript(specifier)
        || options
            .node_args
            .iter()
            .any(|arg| arg.contains("strip-types") || arg.contains("experimental-transform-types"))
    {
        return Err(format!(
            "{first}\n  the original is TypeScript: pass `--node-arg=--experimental-strip-types` \
             or `--module=<the built JavaScript>`"
        ));
    }
    // Node older than 22.6 (and some builds) needs the flag; retry once, visibly.
    let mut retry = vec!["--experimental-strip-types".to_string()];
    retry.extend(options.node_args.clone());
    retry.push(harness.display().to_string());
    run_node(&retry, cwd.as_deref(), &envs).map_err(|second| {
        format!(
            "{first}\n  retried with `--experimental-strip-types`:\n{second}\n  \
             this node cannot run TypeScript; pass `--module=<the built JavaScript>`"
        )
    })
}

fn js_string(text: &str) -> String {
    crate::json::escape_json(text)
}

fn emit_serde() -> &'static str {
    r#"
function $cppan_emit(value) {
  if (value === null) return "null";
  const type = typeof value;
  if (type === "boolean") return value ? "true" : "false";
  if (type === "number") {
    if (!Number.isFinite(value)) return '"' + String(value) + '"';
    if (Object.is(value, -0)) return "-0.0";
    const text = String(value);
    return /[.eE]/.test(text) ? text : text + ".0";
  }
  if (type === "string") return JSON.stringify(value);
  if (Array.isArray(value)) return "[" + value.map($cppan_emit).join(",") + "]";
  if (type === "object") {
    return "{" + Object.keys(value).map((key) => JSON.stringify(key) + ":" + $cppan_emit(value[key])).join(",") + "}";
  }
  return "null";
}

function $cppan_encode(value, seen) {
  if (value === null) return null;
  const type = typeof value;
  if (type === "string" || type === "boolean") return value;
  if (type === "number") return Number.isFinite(value) ? value : { "$opaque": String(value) };
  if (type === "undefined") return { "$opaque": "undefined" };
  if (type === "bigint") return { "$opaque": "bigint" };
  if (type === "symbol") return { "$opaque": "symbol" };
  if (type === "function") return { "$opaque": "function" };
  seen = seen || new Set();
  if (seen.has(value)) return { "$opaque": "circular" };
  seen.add(value);
  try {
    if (Array.isArray(value)) return value.map((item) => $cppan_encode(item, seen));
    if (value instanceof Promise) return { "$opaque": "Promise" };
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) {
      return { "$opaque": (value.constructor && value.constructor.name) || "Object" };
    }
    const out = {};
    for (const key of Object.keys(value)) out[key] = $cppan_encode(value[key], seen);
    return out;
  } finally {
    seen.delete(value);
  }
}

function $cppan_error_name(error) {
  if (error !== null && typeof error === "object") {
    const name = error.constructor && error.constructor.name;
    if (name) return name;
  }
  return typeof error;
}

function $cppan_error_message(error) {
  const text = error !== null && typeof error === "object" && error.message !== undefined
    ? String(error.message)
    : String(error);
  return text.slice(0, 200);
}
"#
}

pub fn record(
    project: &Project,
    _unit: &crate::inventory::Unit,
    calls: &[Json],
    options: &RecordOptions,
) -> Result<usize, String> {
    let spec = specifier(
        project,
        options.module.as_deref(),
        options.resolve_from.as_deref(),
    );
    let calls_js = calls
        .iter()
        .map(Json::to_string_compact)
        .collect::<Vec<_>>()
        .join(",\n  ");
    let harness = format!(
        r#"// Generated by cpp-anything record; runs the original and prints one JSON line per case.
import {{ pathToFileURL }} from "node:url";
{encode}
const CALLS = [
  {calls}
];

const TARGET = {target};
const original = await import(
  TARGET.startsWith("/") || /^[A-Za-z]:[\\/]/.test(TARGET) ? pathToFileURL(TARGET).href : TARGET
);
const module_exports = original.default && typeof original.default === "object"
  ? {{ ...original.default, ...original }}
  : original;

for (const call of CALLS) {{
  try {{
    const fn = module_exports[call.fn];
    if (typeof fn !== "function") throw new TypeError(`no exported function ${{call.fn}}`);
    const value = fn.apply(module_exports, call.args);
    process.stdout.write($cppan_emit({{ id: call.id, ok: $cppan_encode(value) }}) + "\n");
  }} catch (error) {{
    process.stdout.write(
      $cppan_emit({{ id: call.id, err: {{ type: $cppan_error_name(error), msg: $cppan_error_message(error) }} }}) + "\n"
    );
  }}
}}
"#,
        encode = emit_serde(),
        calls = calls_js,
        target = js_string(&specifier_js(&spec)),
    );
    let harness_path = project.dir.join("build").join("record.mjs");
    util::write(&harness_path, &harness)?;
    let output = run_node_with_fallback(project, options, &harness_path, &spec)?;
    super::write_cases(project, calls, &output)
}

// ---------------------------------------------------------------------------
// benchmark
// ---------------------------------------------------------------------------

pub fn bench(project: &Project, repeat: usize, _lang: Lang) -> Result<f64, String> {
    let text = util::read(&project.dir.join("cases.jsonl"))?;
    // One call per function: the first case whose expectation is a value. A
    // function whose only recorded cases raise is measured on the first case
    // anyway, with the exception swallowed inside the timing loop.
    let mut chosen: Vec<(String, Vec<Json>)> = Vec::new();
    let mut fallback: Vec<(String, Vec<Json>)> = Vec::new();
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)?;
        let Some(function) = doc.get("fn").and_then(Json::as_str).map(str::to_string) else {
            continue;
        };
        if chosen.iter().any(|(name, _)| *name == function) {
            continue;
        }
        let args = doc
            .get("args")
            .and_then(Json::as_array)
            .map(<[Json]>::to_vec)
            .unwrap_or_default();
        let ok = doc
            .get("expect")
            .and_then(|expect| expect.get("ok"))
            .is_some();
        if ok {
            fallback.retain(|(name, _)| *name != function);
            chosen.push((function, args));
        } else if !fallback.iter().any(|(name, _)| *name == function) {
            fallback.push((function, args));
        }
    }
    for (name, args) in fallback {
        if !chosen.iter().any(|(chosen, _)| *chosen == name) {
            chosen.push((name, args));
        }
    }
    let calls = chosen
        .iter()
        .map(|(name, args)| {
            Json::obj([
                ("id", Json::str(name.clone())),
                ("fn", Json::str(name.clone())),
                ("args", Json::Array(args.clone())),
            ])
        })
        .collect::<Vec<_>>();
    let calls_js = calls
        .iter()
        .map(Json::to_string_compact)
        .collect::<Vec<_>>()
        .join(",\n  ");
    let spec = specifier(
        project,
        project.source_module.as_deref().map(Path::new),
        project.resolve_from.as_deref().map(Path::new),
    );
    let harness = format!(
        r#"// Generated by cpp-anything bench; loads the original once and times it.
import {{ pathToFileURL }} from "node:url";
const CALLS = [
  {calls}
];
const REPEAT = {repeat};
const TARGET = {target};
const original = await import(
  TARGET.startsWith("/") || /^[A-Za-z]:[\\/]/.test(TARGET) ? pathToFileURL(TARGET).href : TARGET
);
const module_exports = original.default && typeof original.default === "object"
  ? {{ ...original.default, ...original }}
  : original;

for (const call of CALLS) {{
  try {{ module_exports[call.fn].apply(module_exports, call.args); }} catch {{}}
}}
const started = process.hrtime.bigint();
for (let round = 0; round < REPEAT; round++) {{
  for (const call of CALLS) {{
    try {{ module_exports[call.fn].apply(module_exports, call.args); }} catch {{}}
  }}
}}
const finished = process.hrtime.bigint();
process.stdout.write(`{{"ns":${{finished - started}},"calls":${{CALLS.length * REPEAT}}}}\n`);
"#,
        calls = calls_js,
        repeat = repeat.max(1),
        target = js_string(&specifier_js(&spec)),
    );
    let harness_path = project.dir.join("build").join("bench.mjs");
    util::write(&harness_path, &harness)?;
    let options = RecordOptions {
        inputs: None,
        module: project.source_module.as_deref().map(PathBuf::from),
        resolve_from: project.resolve_from.as_deref().map(PathBuf::from),
        node_args: project.node_args.clone(),
    };
    let output = run_node_with_fallback(project, &options, &harness_path, &spec)?;
    let doc = Json::parse(output.trim())?;
    let ns = doc.get("ns").and_then(Json::as_f64).unwrap_or(0.0);
    let calls = doc.get("calls").and_then(Json::as_f64).unwrap_or(0.0);
    if calls <= 0.0 {
        return Err("the node benchmark ran no calls".into());
    }
    Ok(ns / calls)
}

// ---------------------------------------------------------------------------
// shadow binding
// ---------------------------------------------------------------------------

fn addon_name(ns: &str) -> String {
    format!("{ns}_shadow")
}

pub fn shadow_files(project: &Project, ns: &str, _lang: Lang) -> Vec<(String, String)> {
    let library = crate::codegen::shadow_library(project);
    let log = crate::shadow::log_path(project);
    let addon = project
        .dir
        .join("shadow")
        .join(format!("{}.node", addon_name(ns)));
    let spec = specifier(
        project,
        project.source_module.as_deref().map(Path::new),
        project.resolve_from.as_deref().map(Path::new),
    );
    let names = project
        .functions
        .iter()
        .map(|function| js_string(&function.name))
        .collect::<Vec<_>>()
        .join(", ");
    let wrappers = project
        .functions
        .iter()
        .map(|function| {
            let name = &function.name;
            format!(
                r#"export function {name}(...args) {{
  let value;
  try {{
    value = module_exports[{name_js}].apply(module_exports, args);
  }} catch (error) {{
    if (process.env.CPPAN_SHADOW !== "off") $cppan_observe({name_js}, args, {{ err: $cppan_error_shape(error) }});
    throw error;
  }}
  if (process.env.CPPAN_SHADOW !== "off") $cppan_observe({name_js}, args, {{ ok: $cppan_encode(value) }});
  return value;
}}
"#,
                name = name,
                name_js = js_string(name)
            )
        })
        .collect::<Vec<_>>()
        .join("\n");
    let module = format!(
        r#"// Generated by cpp-anything: the original runs first, the C++ port runs second,
// and callers only ever see the original's result. Disagreements go to the log.
//
// Build the port and the addon first:
//   cpp-anything build {project_dir}
//   bash shadow/build.sh
import {{ appendFileSync }} from "node:fs";
import {{ createRequire }} from "node:module";
import {{ pathToFileURL }} from "node:url";
{encode}
const PROJECT = {project};
const LIBRARY = {library};
const ADDON = {addon};
const LOG = process.env.CPPAN_SHADOW_LOG || {log};
const TARGET = {target};
const FUNCTIONS = [{names}];

const require = createRequire(import.meta.url);
const addon = require(ADDON);
if (!addon || typeof addon.call !== "function") {{
  throw new Error(`cpp-anything shadow: ${{ADDON}} does not export call() (build it with bash shadow/build.sh)`);
}}

let original;
try {{
  original = await import(
    TARGET.startsWith("/") || /^[A-Za-z]:[\\/]/.test(TARGET) ? pathToFileURL(TARGET).href : TARGET
  );
}} catch (error) {{
  throw new Error(
    `cpp-anything shadow: cannot load ${{TARGET}}: ${{error && error.message ? error.message : error}}. ` +
    "If it is TypeScript, run node with --experimental-strip-types or point the project at the built JavaScript (--module).",
  );
}}
const module_exports = original.default && typeof original.default === "object"
  ? {{ ...original.default, ...original }}
  : original;
for (const name of FUNCTIONS) {{
  if (typeof module_exports[name] !== "function") {{
    throw new Error(`cpp-anything shadow: ${{TARGET}} does not export ${{name}}`);
  }}
}}

function $cppan_error_shape(error) {{
  if (error !== null && typeof error === "object") {{
    return {{
      type: (error.constructor && error.constructor.name) || "Error",
      msg: String(error.message !== undefined ? error.message : error).slice(0, 200),
    }};
  }}
  return {{ type: typeof error, msg: String(error).slice(0, 200) }};
}}

function $cppan_same(left, right) {{
  if (left === right) return true;
  if (typeof left !== typeof right) return false;
  if (left && right && typeof left === "object") {{
    if (Array.isArray(left) !== Array.isArray(right)) return false;
    if (Array.isArray(left)) {{
      return left.length === right.length && left.every((item, index) => $cppan_same(item, right[index]));
    }}
    const left_keys = Object.keys(left);
    const right_keys = Object.keys(right);
    return left_keys.length === right_keys.length
      && left_keys.every((key) => Object.prototype.hasOwnProperty.call(right, key) && $cppan_same(left[key], right[key]));
  }}
  return false;
}}

function $cppan_compare(expected, got) {{
  const expected_error = expected && expected.err;
  const got_error = got && got.err;
  if (expected_error || got_error) {{
    if (expected_error && got_error) {{
      return expected_error.type === got_error.type
        ? null
        : `error type: expected ${{expected_error.type}}, got ${{got_error.type}}`;
    }}
    if (!expected_error) return `the original returns a value, the port raises ${{got_error.type}}`;
    return `the original raises ${{expected_error.type}}, the port returns a value`;
  }}
  if (!got || !Object.prototype.hasOwnProperty.call(got, "ok")) return "the port returned no value";
  return $cppan_same(expected.ok, got.ok)
    ? null
    : `values differ: expected ${{$cppan_emit(expected.ok)}}, got ${{$cppan_emit(got.ok)}}`;
}}

function $cppan_native_call(fn, args) {{
  const encoded = JSON.stringify(args.map((arg) => $cppan_encode(arg)));
  return JSON.parse(addon.call(fn, encoded));
}}

function $cppan_observe(fn, args, expected) {{
  let got;
  try {{
    got = $cppan_native_call(fn, args);
  }} catch (error) {{
    got = {{ err: {{ type: "ShadowError", msg: String(error && error.message ? error.message : error).slice(0, 200) }} }};
  }}
  const reason = $cppan_compare(expected, got);
  if (reason === null) return;
  try {{
    appendFileSync(
      LOG,
      $cppan_emit({{
        at: new Date().toISOString().replace(/\.\d{{3}}Z$/, "Z"),
        fn,
        args: args.map((arg) => $cppan_encode(arg)),
        expected,
        got,
        reason,
      }}) + "\n",
    );
  }} catch {{
    // The log is best effort: never change what the caller sees.
  }}
}}

{wrappers}
"#,
        project_dir = project.dir.display(),
        encode = emit_serde(),
        project = js_string(&project.dir.display().to_string()),
        library = js_string(&library.display().to_string()),
        addon = js_string(&addon.display().to_string()),
        log = js_string(&log.display().to_string()),
        target = js_string(&specifier_js(&spec)),
        names = names,
        wrappers = wrappers,
    );

    let addon_cc = format!(
        r#"// Node-API addon for the {ns} shadow: one string in, one JSON string out.
// Generated by cpp-anything; the C ABI is cppan_shadow_call/cppan_shadow_free.
#include <node_api.h>

#include <cstdlib>
#include <string>

extern "C" {{
const char* cppan_shadow_call(const char* fn, const char* args_json);
void cppan_shadow_free(const char* pointer);
}}

namespace {{

bool get_string(napi_env env, napi_value value, std::string& out) {{
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return false;
  out.resize(length + 1);
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) {{
    out.clear();
    return false;
  }}
  out.resize(copied);
  return true;
}}

napi_value call(napi_env env, napi_callback_info info) {{
  size_t count = 2;
  napi_value argv[2];
  if (napi_get_cb_info(env, info, &count, argv, nullptr, nullptr) != napi_ok || count < 2) {{
    napi_throw_type_error(env, nullptr, "call(fn, argsJson) needs two strings");
    return nullptr;
  }}
  std::string fn;
  std::string args;
  if (!get_string(env, argv[0], fn) || !get_string(env, argv[1], args)) {{
    napi_throw_type_error(env, nullptr, "call(fn, argsJson) needs two strings");
    return nullptr;
  }}
  const char* result = cppan_shadow_call(fn.c_str(), args.c_str());
  if (result == nullptr) {{
    napi_throw_error(env, nullptr, "cppan_shadow_call returned no result");
    return nullptr;
  }}
  napi_value out;
  const napi_status status = napi_create_string_utf8(env, result, NAPI_AUTO_LENGTH, &out);
  cppan_shadow_free(result);
  if (status != napi_ok) {{
    napi_throw_error(env, nullptr, "cannot allocate the result string");
    return nullptr;
  }}
  return out;
}}

napi_value init(napi_env env, napi_value exports) {{
  napi_value function = nullptr;
  if (napi_create_function(env, "call", NAPI_AUTO_LENGTH, call, nullptr, &function) != napi_ok) {{
    return nullptr;
  }}
  napi_set_named_property(env, exports, "call", function);
  return exports;
}}

}}  // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
"#,
        ns = ns
    );

    let build_sh = format!(
        r#"#!/bin/sh
# Builds the Node-API addon that loads libcppan_shadow.so. Run after `cpp-anything build`.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NODE="${{NODE:-node}}"
CXX="${{CXX:-g++}}"
BUILD={build}
OUT="$here/{addon}.node"
NODE_PREFIX=$("$NODE" -p "require('path').dirname(process.execPath)")
INCLUDE="$NODE_PREFIX/../include/node"
if [ ! -f "$INCLUDE/node_api.h" ]; then
  INCLUDE="$NODE_PREFIX/include/node"
fi
if [ ! -f "$INCLUDE/node_api.h" ]; then
  echo "cpp-anything: node_api.h not found under $NODE_PREFIX; install node headers" >&2
  exit 1
fi

if [ "${{CPPAN_USE_NODE_GYP:-0}}" = "1" ] && command -v node-gyp >/dev/null 2>&1; then
  cd "$here"
  node-gyp rebuild --nodedir="$NODE_PREFIX/.." >/dev/null
  cp "$here/build/Release/{addon}.node" "$OUT"
  echo "cpp-anything: built $OUT (node-gyp)"
  exit 0
fi

case "$(uname -s)" in
  Darwin)
    SHARED="-bundle -undefined dynamic_lookup"
    ;;
  *)
    SHARED="-shared"
    ;;
esac

"$CXX" -std=c++17 -O2 -fPIC $SHARED \
  -I"$INCLUDE" -DNODE_GYP_MODULE_NAME={addon} \
  "$here/addon.cc" -o "$OUT" \
  -L"$BUILD" -lcppan_shadow -Wl,-rpath,"$BUILD"
echo "cpp-anything: built $OUT"
"#,
        addon = addon_name(ns),
        build = project.dir.join("build").display()
    );

    let binding_gyp = format!(
        r#"{{
  "targets": [
    {{
      "target_name": "{addon}",
      "sources": ["addon.cc"],
      "libraries": ["-L{build}", "-lcppan_shadow", "-Wl,-rpath,{build}"],
      "defines": ["NODE_GYP_MODULE_NAME={addon}"]
    }}
  ]
}}
"#,
        addon = addon_name(ns),
        build = project.dir.join("build").display(),
    );

    vec![
        (format!("shadow/{ns}_shadow.mjs"), module),
        ("shadow/addon.cc".to_string(), addon_cc),
        ("shadow/build.sh".to_string(), build_sh),
        ("shadow/binding.gyp".to_string(), binding_gyp),
    ]
}
