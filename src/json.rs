//! JSON, the wire format between the recorder, the C++ driver and the differ.
//!
//! It is deliberately small: objects keep insertion order (compare is
//! order-insensitive, because a Python dict becomes `std::map`), integers and
//! reals stay distinct, and `NaN` / `Infinity` / `-Infinity` round-trip because
//! Python's `json` writes them and a float port can return them.

use std::fmt;

#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Real(f64),
    Str(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl Json {
    pub fn obj<I, K>(pairs: I) -> Json
    where
        I: IntoIterator<Item = (K, Json)>,
        K: Into<String>,
    {
        Json::Object(pairs.into_iter().map(|(k, v)| (k.into(), v)).collect())
    }

    pub fn str(value: impl Into<String>) -> Json {
        Json::Str(value.into())
    }

    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(pairs) => pairs.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Json::Bool(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Json::Int(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Json::Int(value) => Some(*value as f64),
            Json::Real(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Json]> {
        match self {
            Json::Array(items) => Some(items),
            _ => None,
        }
    }

    pub fn as_object(&self) -> Option<&[(String, Json)]> {
        match self {
            Json::Object(pairs) => Some(pairs),
            _ => None,
        }
    }

    pub fn is_null(&self) -> bool {
        matches!(self, Json::Null)
    }

    /// Objects compare without regard to key order; arrays keep their order.
    pub fn equal(&self, other: &Json, tolerance: f64) -> bool {
        match (self, other) {
            (Json::Null, Json::Null) => true,
            (Json::Bool(a), Json::Bool(b)) => a == b,
            (Json::Int(a), Json::Int(b)) => a == b,
            (Json::Real(a), Json::Real(b)) => {
                if a.is_nan() || b.is_nan() {
                    a.is_nan() && b.is_nan()
                } else if tolerance > 0.0 {
                    if a.is_infinite() || b.is_infinite() {
                        a == b
                    } else {
                        (a - b).abs() <= tolerance * a.abs().max(b.abs()).max(1.0)
                    }
                } else {
                    a.to_bits() == b.to_bits()
                }
            }
            (Json::Str(a), Json::Str(b)) => a == b,
            (Json::Array(a), Json::Array(b)) => {
                a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.equal(y, tolerance))
            }
            (Json::Object(a), Json::Object(b)) => {
                a.len() == b.len()
                    && a.iter().all(|(key, value)| {
                        b.iter()
                            .find(|(other, _)| other == key)
                            .is_some_and(|(_, other)| value.equal(other, tolerance))
                    })
            }
            // Python's `1 == 1.0` is true; the recorder keeps the distinction, and
            // the differ only treats them equal when a tolerance was asked for.
            (Json::Int(a), Json::Real(b)) | (Json::Real(b), Json::Int(a)) => {
                tolerance > 0.0 && (*a as f64 - b).abs() <= tolerance * b.abs().max(1.0)
            }
            _ => false,
        }
    }

    pub fn to_string_compact(&self) -> String {
        let mut out = String::new();
        self.write_compact(&mut out);
        out
    }

    fn write_compact(&self, out: &mut String) {
        match self {
            Json::Null => out.push_str("null"),
            Json::Bool(true) => out.push_str("true"),
            Json::Bool(false) => out.push_str("false"),
            Json::Int(value) => out.push_str(&value.to_string()),
            Json::Real(value) => out.push_str(&format_real(*value)),
            Json::Str(value) => out.push_str(&escape_json(value)),
            Json::Array(items) => {
                out.push('[');
                for (index, item) in items.iter().enumerate() {
                    if index > 0 {
                        out.push(',');
                    }
                    item.write_compact(out);
                }
                out.push(']');
            }
            Json::Object(pairs) => {
                out.push('{');
                for (index, (key, value)) in pairs.iter().enumerate() {
                    if index > 0 {
                        out.push(',');
                    }
                    out.push_str(&escape_json(key));
                    out.push(':');
                    value.write_compact(out);
                }
                out.push('}');
            }
        }
    }

    /// Indented rendering, for humans reading a report or a fixture.
    pub fn to_string_pretty(&self) -> String {
        let mut out = String::new();
        self.write_pretty(&mut out, 0);
        out
    }

    fn write_pretty(&self, out: &mut String, depth: usize) {
        let pad = |out: &mut String, depth: usize| {
            out.push('\n');
            for _ in 0..depth {
                out.push_str("  ");
            }
        };
        match self {
            Json::Array(items) if !items.is_empty() => {
                out.push('[');
                for (index, item) in items.iter().enumerate() {
                    if index > 0 {
                        out.push(',');
                    }
                    pad(out, depth + 1);
                    item.write_pretty(out, depth + 1);
                }
                pad(out, depth);
                out.push(']');
            }
            Json::Object(pairs) if !pairs.is_empty() => {
                out.push('{');
                for (index, (key, value)) in pairs.iter().enumerate() {
                    if index > 0 {
                        out.push(',');
                    }
                    pad(out, depth + 1);
                    out.push_str(&escape_json(key));
                    out.push_str(": ");
                    value.write_pretty(out, depth + 1);
                }
                pad(out, depth);
                out.push('}');
            }
            other => other.write_compact(out),
        }
    }

    pub fn parse(text: &str) -> Result<Json, String> {
        let mut parser = Parser::new(text);
        let value = parser.value()?;
        parser.skip_whitespace();
        if !parser.done() {
            return Err(format!("trailing input at offset {}", parser.pos));
        }
        Ok(value)
    }
}

pub fn format_real(value: f64) -> String {
    if value.is_nan() {
        return "NaN".to_string();
    }
    if value.is_infinite() {
        return if value > 0.0 { "Infinity" } else { "-Infinity" }.to_string();
    }
    if value == 0.0 && value.is_sign_negative() {
        return "-0.0".to_string();
    }
    let text = format!("{value:?}");
    if text.contains('.') || text.contains('e') || text.contains('E') {
        text
    } else {
        format!("{text}.0")
    }
}

pub fn escape_json(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0c}' => out.push_str("\\f"),
            ch if (ch as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", ch as u32)),
            ch => out.push(ch),
        }
    }
    out.push('"');
    out
}

impl fmt::Display for Json {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_string_compact())
    }
}

struct Parser<'a> {
    text: &'a [u8],
    source: &'a str,
    pos: usize,
}

impl<'a> Parser<'a> {
    fn new(source: &'a str) -> Self {
        Parser {
            text: source.as_bytes(),
            source,
            pos: 0,
        }
    }

    fn done(&self) -> bool {
        self.pos >= self.text.len()
    }

    fn peek(&self) -> Option<u8> {
        self.text.get(self.pos).copied()
    }

    fn skip_whitespace(&mut self) {
        while let Some(byte) = self.peek() {
            if byte == b' ' || byte == b'\t' || byte == b'\n' || byte == b'\r' {
                self.pos += 1;
            } else {
                break;
            }
        }
    }

    fn error(&self, message: &str) -> String {
        let start = self.source[..self.pos]
            .rfind('\n')
            .map_or(0, |index| index + 1);
        let snippet: String = self.source[start..].chars().take(40).collect();
        format!("{message} at offset {} near {snippet:?}", self.pos)
    }

    fn value(&mut self) -> Result<Json, String> {
        self.skip_whitespace();
        match self.peek() {
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b't') => self.literal("true", Json::Bool(true)),
            Some(b'f') => self.literal("false", Json::Bool(false)),
            Some(b'n') => self.literal("null", Json::Null),
            Some(b'N') => self.literal("NaN", Json::Real(f64::NAN)),
            Some(b'I') => self.literal("Infinity", Json::Real(f64::INFINITY)),
            Some(_) => self.number(),
            None => Err(self.error("expected a value")),
        }
    }

    fn literal(&mut self, text: &str, value: Json) -> Result<Json, String> {
        if self.source[self.pos..].starts_with(text) {
            self.pos += text.len();
            Ok(value)
        } else {
            Err(self.error("expected a literal"))
        }
    }

    fn object(&mut self) -> Result<Json, String> {
        self.pos += 1; // '{'
        let mut pairs = Vec::new();
        self.skip_whitespace();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(Json::Object(pairs));
        }
        loop {
            self.skip_whitespace();
            if self.peek() != Some(b'"') {
                return Err(self.error("expected a key"));
            }
            let key = self.string()?;
            self.skip_whitespace();
            if self.peek() != Some(b':') {
                return Err(self.error("expected ':'"));
            }
            self.pos += 1;
            let value = self.value()?;
            pairs.push((key, value));
            self.skip_whitespace();
            match self.peek() {
                Some(b',') => {
                    self.pos += 1;
                }
                Some(b'}') => {
                    self.pos += 1;
                    return Ok(Json::Object(pairs));
                }
                _ => return Err(self.error("expected ',' or '}'")),
            }
        }
    }

    fn array(&mut self) -> Result<Json, String> {
        self.pos += 1; // '['
        let mut items = Vec::new();
        self.skip_whitespace();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(Json::Array(items));
        }
        loop {
            items.push(self.value()?);
            self.skip_whitespace();
            match self.peek() {
                Some(b',') => {
                    self.pos += 1;
                }
                Some(b']') => {
                    self.pos += 1;
                    return Ok(Json::Array(items));
                }
                _ => return Err(self.error("expected ',' or ']'")),
            }
        }
    }

    fn string(&mut self) -> Result<String, String> {
        self.pos += 1; // '"'
        let mut out = String::new();
        loop {
            let Some(byte) = self.peek() else {
                return Err(self.error("unterminated string"));
            };
            match byte {
                b'"' => {
                    self.pos += 1;
                    return Ok(out);
                }
                b'\\' => {
                    self.pos += 1;
                    let Some(escape) = self.peek() else {
                        return Err(self.error("unterminated escape"));
                    };
                    self.pos += 1;
                    match escape {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{08}'),
                        b'f' => out.push('\u{0c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut code = self.hex4()?;
                            if (0xD800..0xDC00).contains(&code)
                                && self.source[self.pos..].starts_with("\\u")
                            {
                                self.pos += 2;
                                let low = self.hex4()?;
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            }
                            match char::from_u32(code) {
                                Some(ch) => out.push(ch),
                                None => return Err(self.error("invalid \\u escape")),
                            }
                        }
                        _ => return Err(self.error("unknown escape")),
                    }
                }
                _ => {
                    let rest = &self.source[self.pos..];
                    let ch = rest.chars().next().ok_or_else(|| self.error("bad utf-8"))?;
                    out.push(ch);
                    self.pos += ch.len_utf8();
                }
            }
        }
    }

    fn hex4(&mut self) -> Result<u32, String> {
        if self.pos + 4 > self.text.len() {
            return Err(self.error("truncated \\u escape"));
        }
        let slice = &self.source[self.pos..self.pos + 4];
        let value = u32::from_str_radix(slice, 16).map_err(|_| self.error("bad \\u escape"))?;
        self.pos += 4;
        Ok(value)
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.pos;
        if self.peek() == Some(b'-') {
            self.pos += 1;
            if self.source[self.pos..].starts_with("Infinity") {
                self.pos += 8;
                return Ok(Json::Real(f64::NEG_INFINITY));
            }
        }
        let mut is_real = false;
        while let Some(byte) = self.peek() {
            match byte {
                b'0'..=b'9' => self.pos += 1,
                b'.' | b'e' | b'E' | b'+' | b'-' => {
                    is_real = true;
                    self.pos += 1;
                }
                _ => break,
            }
        }
        let token = &self.source[start..self.pos];
        if token.is_empty() || token == "-" {
            return Err(self.error("expected a number"));
        }
        if !is_real {
            if let Ok(value) = token.parse::<i64>() {
                return Ok(Json::Int(value));
            }
        }
        token
            .parse::<f64>()
            .map(Json::Real)
            .map_err(|_| self.error("malformed number"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_and_round_trips() {
        let text = r#"{"id":"a:1","args":["x",-0.0,NaN,Infinity],"ok":true}"#;
        let value = Json::parse(text).unwrap();
        assert_eq!(value.get("id").unwrap().as_str(), Some("a:1"));
        let args = value.get("args").unwrap().as_array().unwrap();
        assert!(args[1].as_f64().unwrap().is_sign_negative());
        assert!(args[2].as_f64().unwrap().is_nan());
        let reparsed = Json::parse(&value.to_string_compact()).unwrap();
        assert!(value.equal(&reparsed, 0.0));
    }

    #[test]
    fn objects_compare_unordered_and_ints_are_not_reals() {
        let a = Json::parse(r#"{"a":1,"b":2}"#).unwrap();
        let b = Json::parse(r#"{"b":2,"a":1}"#).unwrap();
        assert!(a.equal(&b, 0.0));
        assert!(!Json::Int(1).equal(&Json::Real(1.0), 0.0));
        assert!(Json::Int(1).equal(&Json::Real(1.0), 1e-9));
    }

    #[test]
    fn escapes_control_characters() {
        let value = Json::Str("a\tb\nc\u{1}".to_string());
        assert_eq!(value.to_string_compact(), "\"a\\tb\\nc\\u0001\"");
        assert_eq!(
            Json::parse(&value.to_string_compact()).unwrap().as_str(),
            Some("a\tb\nc\u{1}")
        );
    }
}
