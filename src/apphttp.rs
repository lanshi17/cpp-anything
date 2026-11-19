//! HTTP servers: start the original (or a build) on a port, send the recorded
//! requests, and stop it. No HTTP client crate: the requests a case stores are the
//! ones this module writes, so a raw socket keeps the comparison honest.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

pub use crate::appcase::{HttpExpect, HttpRequest};

pub struct Server {
    child: Child,
    pub port: u16,
}

pub struct Response {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: String,
}

impl Server {
    pub fn stop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop();
    }
}

/// A port nobody is listening on: bind to 0, read the port, let go of it.
pub fn free_port() -> Result<u16, String> {
    let listener = TcpListener::bind("127.0.0.1:0").map_err(|error| format!("bind: {error}"))?;
    let port = listener
        .local_addr()
        .map_err(|error| format!("local_addr: {error}"))?
        .port();
    drop(listener);
    Ok(port)
}

pub fn start(
    command: &[String],
    repo: &Path,
    cwd: &Path,
    port: u16,
    timeout_secs: u64,
    extra: &[(String, String)],
) -> Result<Server, String> {
    if command.is_empty() {
        return Err("no command to start: pass `-- <command>` or record one".into());
    }
    let resolved = crate::appcase::resolve_command(repo, command);
    let mut child = Command::new(&resolved[0])
        .args(&resolved[1..])
        .current_dir(cwd)
        .env("PORT", port.to_string())
        .env("CPPAN_PORT", port.to_string())
        .envs(
            extra
                .iter()
                .map(|(key, value)| (key.clone(), value.clone())),
        )
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("{}: {error}", resolved[0]))?;

    let deadline = Instant::now() + Duration::from_secs(timeout_secs.max(1));
    loop {
        if TcpStream::connect(("127.0.0.1", port)).is_ok() {
            return Ok(Server { child, port });
        }
        if let Ok(Some(status)) = child.try_wait() {
            return Err(format!(
                "the server exited before listening on {port} ({status}); is it reading PORT?"
            ));
        }
        if Instant::now() > deadline {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!(
                "the server did not listen on {port} within {timeout_secs}s; it must read the PORT \
                 environment variable"
            ));
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

pub fn request(port: u16, request: &HttpRequest) -> Result<Response, String> {
    let mut stream = TcpStream::connect(("127.0.0.1", port))
        .map_err(|error| format!("connect to {port}: {error}"))?;
    stream
        .set_read_timeout(Some(Duration::from_secs(30)))
        .map_err(|error| format!("read timeout: {error}"))?;
    let method = if request.method.is_empty() {
        "GET"
    } else {
        &request.method
    };
    let path = if request.path.is_empty() {
        "/"
    } else {
        &request.path
    };
    let body = request.body.as_bytes();
    let mut text =
        format!("{method} {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n");
    let mut has_content_type = false;
    for (key, value) in &request.headers {
        if key.eq_ignore_ascii_case("host") || key.eq_ignore_ascii_case("connection") {
            continue;
        }
        if key.eq_ignore_ascii_case("content-type") {
            has_content_type = true;
        }
        text.push_str(&format!("{key}: {value}\r\n"));
    }
    if !body.is_empty() {
        if !has_content_type {
            text.push_str("Content-Type: application/json\r\n");
        }
        text.push_str(&format!("Content-Length: {}\r\n", body.len()));
    }
    text.push_str("\r\n");
    stream
        .write_all(text.as_bytes())
        .map_err(|error| format!("write: {error}"))?;
    stream
        .write_all(body)
        .map_err(|error| format!("write: {error}"))?;

    let mut raw = Vec::new();
    stream
        .read_to_end(&mut raw)
        .map_err(|error| format!("read: {error}"))?;
    parse_response(&raw)
}

fn parse_response(raw: &[u8]) -> Result<Response, String> {
    let split = raw
        .windows(4)
        .position(|window| window == b"\r\n\r\n")
        .ok_or("the response had no header terminator")?;
    let head = String::from_utf8_lossy(&raw[..split]).into_owned();
    let mut body = raw[split + 4..].to_vec();
    let mut lines = head.lines();
    let status_line = lines.next().unwrap_or("");
    let status = status_line
        .split_whitespace()
        .nth(1)
        .and_then(|code| code.parse::<u16>().ok())
        .ok_or_else(|| format!("unreadable status line {status_line:?}"))?;
    let mut headers = Vec::new();
    let mut chunked = false;
    let mut content_length: Option<usize> = None;
    for line in lines {
        if let Some((key, value)) = line.split_once(':') {
            let key = key.trim().to_string();
            let value = value.trim().to_string();
            if key.eq_ignore_ascii_case("transfer-encoding")
                && value.to_ascii_lowercase().contains("chunked")
            {
                chunked = true;
            }
            if key.eq_ignore_ascii_case("content-length") {
                content_length = value.parse().ok();
            }
            headers.push((key, value));
        }
    }
    if chunked {
        body = decode_chunked(&body);
    } else if let Some(length) = content_length {
        body.truncate(length.min(body.len()));
    }
    Ok(Response {
        status,
        headers,
        body: String::from_utf8_lossy(&body).into_owned(),
    })
}

fn decode_chunked(body: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut index = 0;
    while index < body.len() {
        let Some(end) = body[index..]
            .windows(2)
            .position(|window| window == b"\r\n")
        else {
            break;
        };
        let size_text = String::from_utf8_lossy(&body[index..index + end]);
        let size = usize::from_str_radix(size_text.trim().split(';').next().unwrap_or("0"), 16)
            .unwrap_or(0);
        index += end + 2;
        if size == 0 {
            break;
        }
        let stop = (index + size).min(body.len());
        out.extend_from_slice(&body[index..stop]);
        index = stop + 2;
    }
    out
}

pub fn content_type(response: &Response) -> String {
    response
        .headers
        .iter()
        .find(|(key, _)| key.eq_ignore_ascii_case("content-type"))
        .map(|(_, value)| value.clone())
        .unwrap_or_default()
}

pub fn expectation(response: Response) -> HttpExpect {
    HttpExpect {
        status: response.status,
        content_type: content_type(&response),
        body: response.body,
    }
}

/// Requests written in the docs as `curl` lines, plus the JSONL `--inputs` shape.
pub fn requests_in(text: &str) -> Vec<HttpRequest> {
    let mut out = Vec::new();
    for line in text.lines() {
        let trimmed = line.trim().trim_start_matches("$ ").trim();
        if !trimmed.starts_with("curl ") {
            continue;
        }
        if let Some(request) = parse_curl(trimmed) {
            out.push(request);
        }
    }
    out
}

fn parse_curl(command: &str) -> Option<HttpRequest> {
    let words = shell_words(command)?;
    let mut method = String::new();
    let mut headers = Vec::new();
    let mut body = String::new();
    let mut url = String::new();
    let mut index = 1;
    while index < words.len() {
        match words[index].as_str() {
            "-X" | "--request" => {
                index += 1;
                method = words.get(index)?.clone();
            }
            "-H" | "--header" => {
                index += 1;
                if let Some((key, value)) = words.get(index)?.split_once(':') {
                    headers.push((key.trim().to_string(), value.trim().to_string()));
                }
            }
            "-d" | "--data" | "--data-raw" | "--data-binary" => {
                index += 1;
                body = words.get(index)?.clone();
                if method.is_empty() {
                    method = "POST".into();
                }
            }
            "-s" | "-S" | "--silent" | "--show-error" | "-i" | "-v" | "--location" | "-L" => {}
            other => {
                if !other.starts_with('-') {
                    url = other.to_string();
                }
            }
        }
        index += 1;
    }
    if url.is_empty() {
        return None;
    }
    let path = match url.find("://") {
        Some(at) => {
            let after = &url[at + 3..];
            match after.find('/') {
                Some(slash) => after[slash..].to_string(),
                None => "/".to_string(),
            }
        }
        None => url,
    };
    Some(HttpRequest {
        method: if method.is_empty() {
            "GET".into()
        } else {
            method
        },
        path,
        headers,
        body,
    })
}

fn shell_words(command: &str) -> Option<Vec<String>> {
    let mut out = Vec::new();
    let mut current = String::new();
    let mut quote: Option<char> = None;
    let mut chars = command.chars().peekable();
    while let Some(ch) = chars.next() {
        match quote {
            Some(q) if ch == q => quote = None,
            Some(_) if ch == '\\' => {
                if let Some(next) = chars.next() {
                    current.push(next);
                }
            }
            Some(_) => current.push(ch),
            None => match ch {
                '"' | '\'' => quote = Some(ch),
                '\\' => {
                    if let Some(next) = chars.next() {
                        current.push(next);
                    }
                }
                ' ' | '\t' => {
                    if !current.is_empty() {
                        out.push(std::mem::take(&mut current));
                    }
                }
                _ => current.push(ch),
            },
        }
    }
    if quote.is_some() {
        return None;
    }
    if !current.is_empty() {
        out.push(current);
    }
    Some(out)
}
