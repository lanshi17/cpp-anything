//! Small, dependency-free plumbing: processes, paths, text, versions.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};

pub struct Run {
    pub ok: bool,
    pub status: Option<i32>,
    pub stdout: String,
    pub stderr: String,
}

impl Run {
    pub fn failure(&self) -> String {
        let code = self
            .status
            .map_or_else(|| "signal".to_string(), |code| code.to_string());
        let stderr = self.stderr.trim();
        if stderr.is_empty() {
            format!("exited with {code}")
        } else {
            format!("exited with {code}: {stderr}")
        }
    }
}

/// Runs a command, capturing output. Missing binaries come back as a failure with
/// `status = None`, so callers can report "python3 not found" instead of panicking.
pub fn run(program: &str, args: &[String], cwd: Option<&Path>) -> Run {
    run_with_env(program, args, cwd, &[])
}

/// Same, with extra environment entries (the recorder's hook relies on PYTHONPATH).
pub fn run_with_env(
    program: &str,
    args: &[String],
    cwd: Option<&Path>,
    envs: &[(String, String)],
) -> Run {
    let mut command = Command::new(program);
    command.args(args).stdin(Stdio::null());
    for (key, value) in envs {
        command.env(key, value);
    }
    if let Some(cwd) = cwd {
        command.current_dir(cwd);
    }
    match command.output() {
        Ok(Output {
            status,
            stdout,
            stderr,
        }) => Run {
            ok: status.success(),
            status: status.code(),
            stdout: String::from_utf8_lossy(&stdout).into_owned(),
            stderr: String::from_utf8_lossy(&stderr).into_owned(),
        },
        Err(error) if error.kind() == io::ErrorKind::NotFound => Run {
            ok: false,
            status: None,
            stdout: String::new(),
            stderr: format!("{program}: command not found"),
        },
        Err(error) => Run {
            ok: false,
            status: None,
            stdout: String::new(),
            stderr: format!("{program}: {error}"),
        },
    }
}

pub fn which(program: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    for directory in std::env::split_paths(&path) {
        let candidate = directory.join(program);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

pub fn read(path: &Path) -> Result<String, String> {
    fs::read_to_string(path).map_err(|error| format!("{}: {error}", path.display()))
}

pub fn write(path: &Path, contents: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
    }
    fs::write(path, contents).map_err(|error| format!("{}: {error}", path.display()))
}

/// Creates a directory, refusing when it is neither absent nor empty unless
/// `force` is set. A port in progress is the user's work; never silently replace it.
pub fn prepare_dir(path: &Path, force: bool) -> Result<bool, String> {
    if path.exists() {
        let empty = fs::read_dir(path)
            .map_err(|error| format!("{}: {error}", path.display()))?
            .next()
            .is_none();
        if !empty && !force {
            return Err(format!(
                "{} already exists and is not empty (pass --force to write into it)",
                path.display()
            ));
        }
        fs::create_dir_all(path).map_err(|error| format!("{}: {error}", path.display()))?;
        Ok(!empty)
    } else {
        fs::create_dir_all(path).map_err(|error| format!("{}: {error}", path.display()))?;
        Ok(false)
    }
}

/// FNV-1a 64: enough to notice that generated docs drifted, without a hash crate.
pub fn hash64(text: &str) -> String {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for byte in text.as_bytes() {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x1000_0000_01b3);
    }
    format!("{hash:016x}")
}

/// Numeric-aware version compare: `19.9 < 19.10`, `15 < 15.2`.
pub fn version_key(text: &str) -> Vec<u64> {
    let mut out = Vec::new();
    let mut current = String::new();
    for ch in text.chars() {
        if ch.is_ascii_digit() {
            current.push(ch);
        } else if !current.is_empty() {
            out.push(current.parse().unwrap_or(0));
            current.clear();
        }
    }
    if !current.is_empty() {
        out.push(current.parse().unwrap_or(0));
    }
    out
}

pub fn version_at_least(version: &str, minimum: &str) -> bool {
    let left = version_key(version);
    let right = version_key(minimum);
    for index in 0..left.len().max(right.len()) {
        let a = left.get(index).copied().unwrap_or(0);
        let b = right.get(index).copied().unwrap_or(0);
        if a != b {
            return a > b;
        }
    }
    true
}

pub fn sanitize_identifier(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for ch in text.chars() {
        if ch.is_ascii_alphanumeric() {
            out.push(ch.to_ascii_lowercase());
        } else if !out.ends_with('_') {
            out.push('_');
        }
    }
    let trimmed = out.trim_matches('_').to_string();
    if trimmed.is_empty() {
        "unit".to_string()
    } else if trimmed.chars().next().is_some_and(|ch| ch.is_ascii_digit()) {
        format!("u_{trimmed}")
    } else {
        trimmed
    }
}

pub fn indent(text: &str, spaces: usize) -> String {
    let pad = " ".repeat(spaces);
    text.lines()
        .map(|line| {
            if line.is_empty() {
                String::new()
            } else {
                format!("{pad}{line}")
            }
        })
        .collect::<Vec<_>>()
        .join("\n")
}

pub fn truncate_text(text: &str, width: usize) -> String {
    let mut out = String::new();
    for ch in text.chars() {
        if out.chars().count() >= width {
            out.push('…');
            break;
        }
        out.push(ch);
    }
    out.replace('\n', "\\n")
}

/// Walks up from the current directory looking for the cpp-anything checkout.
pub fn repo_root() -> Option<std::path::PathBuf> {
    let mut directory = std::env::current_dir().ok()?;
    loop {
        if directory.join("data").join("matrix.json").is_file() {
            return Some(directory);
        }
        if !directory.pop() {
            return None;
        }
    }
}

pub fn short_path(path: &Path) -> String {
    let cwd = std::env::current_dir().ok();
    match cwd.and_then(|cwd| path.strip_prefix(cwd).ok().map(Path::to_path_buf)) {
        Some(relative) => relative.display().to_string(),
        None => path.display().to_string(),
    }
}

pub fn now_iso8601() -> String {
    // Seconds since the epoch, formatted by hand: no chrono, and the exact string
    // only has to be sortable and readable.
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or(0);
    let days = seconds / 86_400;
    let time = seconds % 86_400;
    let (year, month, day) = civil_from_days(days as i64);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        time / 3600,
        (time % 3600) / 60,
        time % 60
    )
}

// Howard Hinnant's days-to-civil algorithm, which is public domain.
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compares_versions_numerically() {
        assert!(version_at_least("19.10", "19.9"));
        assert!(version_at_least("15", "15"));
        assert!(!version_at_least("14.2", "15"));
        assert!(version_at_least("16", "15.2"));
    }

    #[test]
    fn sanitizes_identifiers() {
        assert_eq!(sanitize_identifier("textkit.words"), "textkit_words");
        assert_eq!(sanitize_identifier("My-Unit!"), "my_unit");
        assert_eq!(sanitize_identifier("9lives"), "u_9lives");
    }
}
