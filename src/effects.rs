//! Side effects: what left the process while a case ran.
//!
//! Two local sinks are enough for the cases that matter: an HTTP proxy that answers
//! everything and records the request, and an SMTP server that accepts a message and
//! records it. `--db-reset`/`--db-dump` bracket each case with the app's database so
//! what it wrote is part of the recording too.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub struct HttpSink {
    pub port: u16,
    log: Arc<Mutex<Vec<String>>>,
    stop: Arc<AtomicBool>,
    handle: Option<std::thread::JoinHandle<()>>,
}

impl HttpSink {
    pub fn start() -> Result<HttpSink, String> {
        let listener =
            TcpListener::bind("127.0.0.1:0").map_err(|error| format!("bind: {error}"))?;
        let port = listener
            .local_addr()
            .map_err(|error| format!("{error}"))?
            .port();
        listener
            .set_nonblocking(true)
            .map_err(|error| format!("{error}"))?;
        let log = Arc::new(Mutex::new(Vec::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let log_thread = Arc::clone(&log);
        let stop_thread = Arc::clone(&stop);
        let handle = std::thread::spawn(move || {
            while !stop_thread.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((stream, _)) => {
                        let _ = stream.set_nonblocking(false);
                        handle_http(stream, &log_thread);
                    }
                    Err(_) => std::thread::sleep(Duration::from_millis(10)),
                }
            }
        });
        Ok(HttpSink {
            port,
            log,
            stop,
            handle: Some(handle),
        })
    }

    pub fn url(&self) -> String {
        format!("http://127.0.0.1:{}", self.port)
    }

    pub fn take(&mut self) -> Vec<String> {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
        self.log
            .lock()
            .map(|lines| lines.clone())
            .unwrap_or_default()
    }
}

fn handle_http(stream: TcpStream, log: &Arc<Mutex<Vec<String>>>) {
    let mut reader = BufReader::new(match stream.try_clone() {
        Ok(clone) => clone,
        Err(_) => return,
    });
    let mut request_line = String::new();
    if reader.read_line(&mut request_line).is_err() {
        return;
    }
    let mut content_length = 0usize;
    let mut headers = Vec::new();
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line).is_err() || line.trim().is_empty() {
            break;
        }
        if let Some((key, value)) = line.split_once(':') {
            if key.eq_ignore_ascii_case("content-length") {
                content_length = value.trim().parse().unwrap_or(0);
            }
            headers.push((key.trim().to_ascii_lowercase(), value.trim().to_string()));
        }
    }
    let mut body = vec![0u8; content_length.min(1 << 20)];
    let _ = reader.read_exact(&mut body);
    let host = headers
        .iter()
        .find(|(key, _)| key == "host")
        .map(|(_, value)| value.clone())
        .unwrap_or_default();
    let line = format!(
        "http {} {} {}",
        request_line.trim(),
        host,
        String::from_utf8_lossy(&body)
    );
    if let Ok(mut lines) = log.lock() {
        lines.push(line);
    }
    let mut stream = stream;
    let _ = stream
        .write_all(b"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

pub struct SmtpSink {
    pub port: u16,
    log: Arc<Mutex<Vec<String>>>,
    stop: Arc<AtomicBool>,
    handle: Option<std::thread::JoinHandle<()>>,
}

impl SmtpSink {
    pub fn start() -> Result<SmtpSink, String> {
        let listener =
            TcpListener::bind("127.0.0.1:0").map_err(|error| format!("bind: {error}"))?;
        let port = listener
            .local_addr()
            .map_err(|error| format!("{error}"))?
            .port();
        listener
            .set_nonblocking(true)
            .map_err(|error| format!("{error}"))?;
        let log = Arc::new(Mutex::new(Vec::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let log_thread = Arc::clone(&log);
        let stop_thread = Arc::clone(&stop);
        let handle = std::thread::spawn(move || {
            while !stop_thread.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((stream, _)) => {
                        let _ = stream.set_nonblocking(false);
                        handle_smtp(stream, &log_thread);
                    }
                    Err(_) => std::thread::sleep(Duration::from_millis(10)),
                }
            }
        });
        Ok(SmtpSink {
            port,
            log,
            stop,
            handle: Some(handle),
        })
    }

    pub fn take(&mut self) -> Vec<String> {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
        self.log
            .lock()
            .map(|lines| lines.clone())
            .unwrap_or_default()
    }
}

/// Just enough SMTP for a library to deliver a message: greet, take the envelope,
/// collect DATA until the lone dot, accept.
fn handle_smtp(stream: TcpStream, log: &Arc<Mutex<Vec<String>>>) {
    let mut writer = stream;
    let mut reader = BufReader::new(match writer.try_clone() {
        Ok(clone) => clone,
        Err(_) => return,
    });
    let _ = writer.write_all(b"220 cppan-sink ESMTP\r\n");
    let mut from = String::new();
    let mut to = Vec::new();
    let mut in_data = false;
    let mut message = Vec::new();
    let mut line = String::new();
    loop {
        line.clear();
        if reader.read_line(&mut line).is_err() {
            break;
        }
        let trimmed = line.trim_end_matches(['\r', '\n']);
        if in_data {
            if trimmed == "." {
                in_data = false;
                let text = String::from_utf8_lossy(&message).into_owned();
                if let Ok(mut lines) = log.lock() {
                    lines.push(format!("smtp from={from} to={} {text}", to.join(",")));
                }
                let _ = writer.write_all(b"250 2.0.0 queued\r\n");
                continue;
            }
            message.extend_from_slice(line.as_bytes());
            continue;
        }
        let upper = trimmed.to_ascii_uppercase();
        if upper.starts_with("EHLO") || upper.starts_with("HELO") {
            let _ = writer.write_all(b"250 cppan-sink\r\n");
        } else if upper.starts_with("MAIL FROM:") {
            from = trimmed["MAIL FROM:".len()..]
                .trim()
                .trim_matches(['<', '>'])
                .to_string();
            let _ = writer.write_all(b"250 2.1.0 ok\r\n");
        } else if upper.starts_with("RCPT TO:") {
            to.push(
                trimmed["RCPT TO:".len()..]
                    .trim()
                    .trim_matches(['<', '>'])
                    .to_string(),
            );
            let _ = writer.write_all(b"250 2.1.5 ok\r\n");
        } else if upper == "DATA" {
            in_data = true;
            message.clear();
            let _ = writer.write_all(b"354 end with <CRLF>.<CRLF>\r\n");
        } else if upper == "QUIT" {
            let _ = writer.write_all(b"221 bye\r\n");
            break;
        } else {
            let _ = writer.write_all(b"250 ok\r\n");
        }
    }
}

pub struct Sinks {
    pub http: Option<HttpSink>,
    pub smtp: Option<SmtpSink>,
}

impl Sinks {
    pub fn start(kinds: &[String]) -> Result<Sinks, String> {
        let mut http = None;
        let mut smtp = None;
        for kind in kinds {
            match kind.as_str() {
                "http" if http.is_none() => http = Some(HttpSink::start()?),
                "smtp" if smtp.is_none() => smtp = Some(SmtpSink::start()?),
                "http" | "smtp" => {}
                other => {
                    return Err(format!("unknown effect {other:?}: use http or smtp"));
                }
            }
        }
        Ok(Sinks { http, smtp })
    }

    /// Environment that points the app's own settings at the sinks.
    pub fn env(&self, extra: &[(String, String)]) -> Vec<(String, String)> {
        let mut env = Vec::new();
        if let Some(http) = &self.http {
            let url = http.url();
            for key in [
                "HTTP_PROXY",
                "http_proxy",
                "HTTPS_PROXY",
                "https_proxy",
                "CPPAN_HTTP_SINK",
            ] {
                env.push((key.to_string(), url.clone()));
            }
        }
        if let Some(smtp) = &self.smtp {
            env.push(("SMTP_HOST".to_string(), "127.0.0.1".to_string()));
            env.push(("SMTP_PORT".to_string(), smtp.port.to_string()));
            env.push(("CPPAN_SMTP".to_string(), format!("127.0.0.1:{}", smtp.port)));
        }
        env.extend(extra.iter().cloned());
        env
    }

    /// The ports this run used: an ephemeral port is not behaviour, so effects are
    /// stored with it masked.
    pub fn ports(&self) -> Vec<String> {
        let mut ports = Vec::new();
        if let Some(http) = &self.http {
            ports.push(http.port.to_string());
        }
        if let Some(smtp) = &self.smtp {
            ports.push(smtp.port.to_string());
        }
        ports
    }

    /// What reached the sinks, in order.
    pub fn take(&mut self) -> Vec<String> {
        let mut lines = Vec::new();
        if let Some(http) = self.http.as_mut() {
            lines.extend(http.take());
        }
        if let Some(smtp) = self.smtp.as_mut() {
            lines.extend(smtp.take());
        }
        lines
    }
}

/// A setting the app reads, pointed at a sink: `VAR=http-url` / `VAR=smtp-host` …
pub fn effect_env(spec: &str) -> Result<(String, String), String> {
    let (name, kind) = spec
        .split_once('=')
        .ok_or_else(|| format!("--effect-env {spec:?} needs VAR=KIND"))?;
    Ok((name.to_string(), kind.to_string()))
}
