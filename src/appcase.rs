//! Whole-app cases: run a CLI black-box, twice, and store what it did.
//!
//! The unit loop records functions; this records the app: arguments, stdin, the
//! environment, files it was given, and everything observable afterwards — stdout,
//! stderr, exit code, files written, and whether the build answered from the port
//! or fell back to the original.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::json::Json;
use crate::util;

pub const SCHEMA_CONFIG: &str = "cpp-anything/app-cases@1";
pub const CASES_DIR: &str = "cpp/app-cases";

#[derive(Clone, Debug)]
pub struct Config {
    pub name: String,
    pub run: Vec<String>,
    pub timeout_secs: u64,
    pub normalize: Vec<String>,
    pub float_tolerance: f64,
    pub http: bool,
    pub port: Option<u16>,
    pub args: Vec<String>,
    pub fresh: Vec<String>,
    pub pace_ms: u64,
    pub effects: Vec<String>,
    pub lifted: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            name: String::new(),
            run: Vec::new(),
            timeout_secs: 30,
            normalize: Vec::new(),
            float_tolerance: 0.0,
            http: false,
            port: None,
            args: Vec::new(),
            fresh: Vec::new(),
            pace_ms: 0,
            effects: Vec::new(),
            lifted: false,
        }
    }
}

impl Config {
    pub fn load(dir: &Path) -> Result<Config, String> {
        let path = dir.join("config.json");
        let text = util::read(&path)
            .map_err(|error| format!("{error} (is this a recorded app? `app record` first)"))?;
        let doc = Json::parse(&text)?;
        let strings = |key: &str| -> Vec<String> {
            doc.get(key)
                .and_then(Json::as_array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(|item| item.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default()
        };
        Ok(Config {
            name: doc
                .get("name")
                .and_then(Json::as_str)
                .unwrap_or("app")
                .to_string(),
            run: strings("run"),
            timeout_secs: doc.get("timeout_secs").and_then(Json::as_i64).unwrap_or(30) as u64,
            normalize: strings("normalize"),
            float_tolerance: doc
                .get("float_tolerance")
                .and_then(Json::as_f64)
                .unwrap_or(0.0),
            http: doc.get("http").and_then(Json::as_bool).unwrap_or(false),
            port: doc
                .get("port")
                .and_then(Json::as_i64)
                .map(|port| port as u16),
            args: strings("args"),
            fresh: strings("fresh"),
            pace_ms: doc.get("pace_ms").and_then(Json::as_i64).unwrap_or(0) as u64,
            effects: strings("effects"),
            lifted: doc.get("lifted").and_then(Json::as_bool).unwrap_or(false),
        })
    }

    pub fn save(&self, dir: &Path) -> Result<(), String> {
        let doc = Json::obj([
            ("schema", Json::str(SCHEMA_CONFIG)),
            ("name", Json::str(self.name.clone())),
            (
                "run",
                Json::Array(
                    self.run
                        .iter()
                        .map(|part| Json::str(part.clone()))
                        .collect(),
                ),
            ),
            ("timeout_secs", Json::Int(self.timeout_secs as i64)),
            (
                "normalize",
                Json::Array(
                    self.normalize
                        .iter()
                        .map(|part| Json::str(part.clone()))
                        .collect(),
                ),
            ),
            ("float_tolerance", Json::Real(self.float_tolerance)),
            ("http", Json::Bool(self.http)),
            (
                "port",
                self.port
                    .map(|port| Json::Int(i64::from(port)))
                    .unwrap_or(Json::Null),
            ),
            (
                "args",
                Json::Array(
                    self.args
                        .iter()
                        .map(|part| Json::str(part.clone()))
                        .collect(),
                ),
            ),
            (
                "fresh",
                Json::Array(
                    self.fresh
                        .iter()
                        .map(|part| Json::str(part.clone()))
                        .collect(),
                ),
            ),
            ("pace_ms", Json::Real(self.pace_ms as f64)),
            (
                "effects",
                Json::Array(
                    self.effects
                        .iter()
                        .map(|part| Json::str(part.clone()))
                        .collect(),
                ),
            ),
            ("lifted", Json::Bool(self.lifted)),
        ]);
        util::write(&dir.join("config.json"), &doc.to_string_pretty())
    }
}

#[derive(Clone, Debug)]
pub struct Case {
    pub request: Option<HttpRequest>,
    pub name: String,
    pub args: Vec<String>,
    pub stdin: String,
    pub env: Vec<(String, String)>,
    pub files: Vec<(String, String)>,
    pub expect: Expect,
    /// The app did not produce the same output twice: replay reports it, never fails on it.
    pub unstable: bool,
    pub source: String,
}

#[derive(Clone, Debug, Default)]
pub struct HttpRequest {
    pub method: String,
    pub path: String,
    pub headers: Vec<(String, String)>,
    pub body: String,
}

#[derive(Clone, Debug, Default)]
pub struct HttpExpect {
    pub status: u16,
    pub content_type: String,
    pub body: String,
}

#[derive(Clone, Debug, Default)]
pub struct Expect {
    pub http: Option<HttpExpect>,
    pub stdout: String,
    pub stderr: String,
    pub exit: i32,
    pub files: Vec<(String, String)>,
    pub fallback: bool,
    pub effects: Vec<String>,
}

impl Case {
    pub fn to_json(&self) -> Json {
        Json::obj([
            ("name", Json::str(self.name.clone())),
            (
                "args",
                Json::Array(self.args.iter().map(|arg| Json::str(arg.clone())).collect()),
            ),
            ("stdin", Json::str(self.stdin.clone())),
            (
                "env",
                Json::Object(
                    self.env
                        .iter()
                        .map(|(key, value)| (key.clone(), Json::str(value.clone())))
                        .collect(),
                ),
            ),
            (
                "files",
                Json::Object(
                    self.files
                        .iter()
                        .map(|(key, value)| (key.clone(), Json::str(value.clone())))
                        .collect(),
                ),
            ),
            (
                "request",
                self.request
                    .as_ref()
                    .map(|request| {
                        Json::obj([
                            ("method", Json::str(request.method.clone())),
                            ("path", Json::str(request.path.clone())),
                            (
                                "headers",
                                Json::Object(
                                    request
                                        .headers
                                        .iter()
                                        .map(|(key, value)| (key.clone(), Json::str(value.clone())))
                                        .collect(),
                                ),
                            ),
                            ("body", Json::str(request.body.clone())),
                        ])
                    })
                    .unwrap_or(Json::Null),
            ),
            (
                "expect",
                Json::obj([
                    ("stdout", Json::str(self.expect.stdout.clone())),
                    ("stderr", Json::str(self.expect.stderr.clone())),
                    ("exit", Json::Int(i64::from(self.expect.exit))),
                    (
                        "files",
                        Json::Object(
                            self.expect
                                .files
                                .iter()
                                .map(|(key, value)| (key.clone(), Json::str(value.clone())))
                                .collect(),
                        ),
                    ),
                    ("fallback", Json::Bool(self.expect.fallback)),
                    (
                        "http",
                        self.expect
                            .http
                            .as_ref()
                            .map(|http| {
                                Json::obj([
                                    ("status", Json::Int(i64::from(http.status))),
                                    ("content_type", Json::str(http.content_type.clone())),
                                    ("body", Json::str(http.body.clone())),
                                ])
                            })
                            .unwrap_or(Json::Null),
                    ),
                    (
                        "effects",
                        Json::Array(
                            self.expect
                                .effects
                                .iter()
                                .map(|line| Json::str(line.clone()))
                                .collect(),
                        ),
                    ),
                ]),
            ),
            ("unstable", Json::Bool(self.unstable)),
            ("source", Json::str(self.source.clone())),
        ])
    }

    pub fn parse(doc: &Json) -> Result<Case, String> {
        let strings = |value: Option<&Json>| -> Vec<String> {
            value
                .and_then(Json::as_array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(|item| item.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default()
        };
        let pairs = |value: Option<&Json>| -> Vec<(String, String)> {
            value
                .and_then(Json::as_object)
                .map(|entries| {
                    entries
                        .iter()
                        .map(|(key, value)| (key.clone(), value.as_str().unwrap_or("").to_string()))
                        .collect()
                })
                .unwrap_or_default()
        };
        let expect = doc.get("expect").cloned().unwrap_or(Json::Null);
        let request = doc.get("request").and_then(Json::as_object).map(|_| {
            let query = doc.get("request").cloned().unwrap_or(Json::Null);
            HttpRequest {
                method: query
                    .get("method")
                    .and_then(Json::as_str)
                    .unwrap_or("GET")
                    .to_string(),
                path: query
                    .get("path")
                    .and_then(Json::as_str)
                    .unwrap_or("/")
                    .to_string(),
                headers: pairs(query.get("headers")),
                body: query
                    .get("body")
                    .and_then(Json::as_str)
                    .unwrap_or("")
                    .to_string(),
            }
        });
        let http = expect
            .get("http")
            .and_then(Json::as_object)
            .map(|_| HttpExpect {
                status: expect
                    .get("http")
                    .and_then(|http| http.get("status"))
                    .and_then(Json::as_i64)
                    .unwrap_or(0) as u16,
                content_type: expect
                    .get("http")
                    .and_then(|http| http.get("content_type"))
                    .and_then(Json::as_str)
                    .unwrap_or("")
                    .to_string(),
                body: expect
                    .get("http")
                    .and_then(|http| http.get("body"))
                    .and_then(Json::as_str)
                    .unwrap_or("")
                    .to_string(),
            });
        Ok(Case {
            request,
            name: doc
                .get("name")
                .and_then(Json::as_str)
                .unwrap_or("case")
                .to_string(),
            args: strings(doc.get("args")),
            stdin: doc
                .get("stdin")
                .and_then(Json::as_str)
                .unwrap_or("")
                .to_string(),
            env: pairs(doc.get("env")),
            files: pairs(doc.get("files")),
            expect: Expect {
                http,
                stdout: expect
                    .get("stdout")
                    .and_then(Json::as_str)
                    .unwrap_or("")
                    .to_string(),
                stderr: expect
                    .get("stderr")
                    .and_then(Json::as_str)
                    .unwrap_or("")
                    .to_string(),
                exit: expect.get("exit").and_then(Json::as_i64).unwrap_or(0) as i32,
                files: pairs(expect.get("files")),
                fallback: expect
                    .get("fallback")
                    .and_then(Json::as_bool)
                    .unwrap_or(false),
                effects: strings(expect.get("effects")),
            },
            unstable: doc.get("unstable").and_then(Json::as_bool).unwrap_or(false),
            source: doc
                .get("source")
                .and_then(Json::as_str)
                .unwrap_or("")
                .to_string(),
        })
    }
}

pub fn cases_path(repo: &Path) -> PathBuf {
    repo.join(CASES_DIR).join("cases.jsonl")
}

pub fn load_cases(repo: &Path) -> Result<Vec<Case>, String> {
    let path = cases_path(repo);
    let text = util::read(&path)
        .map_err(|error| format!("{error} (run `cpp-anything app record` first)"))?;
    let mut cases = Vec::new();
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        cases.push(Case::parse(&Json::parse(line)?)?);
    }
    if cases.is_empty() {
        return Err(format!("{}: no cases", path.display()));
    }
    Ok(cases)
}

pub fn save_cases(repo: &Path, cases: &[Case]) -> Result<(), String> {
    let mut text = String::new();
    for case in cases {
        text.push_str(&case.to_json().to_string_compact());
        text.push('\n');
    }
    util::write(&cases_path(repo), &text)
}

pub struct RunOptions<'a> {
    pub repo: &'a Path,
    pub command: &'a [String],
    pub timeout: u64,
    pub extra_env: Vec<(String, String)>,
}

/// A recorded command stores repository-relative words; running it must not depend
/// on where the tool was started.
pub fn resolve_command(repo: &Path, command: &[String]) -> Vec<String> {
    command
        .iter()
        .map(|word| {
            if word.starts_with('-') {
                return word.clone();
            }
            let candidate = repo.join(word);
            if candidate.exists() {
                return candidate
                    .canonicalize()
                    .unwrap_or(candidate)
                    .display()
                    .to_string();
            }
            let direct = Path::new(word);
            if direct.exists() {
                return direct
                    .canonicalize()
                    .unwrap_or_else(|_| direct.to_path_buf())
                    .display()
                    .to_string();
            }
            word.clone()
        })
        .collect()
}

#[derive(Clone, Debug)]
pub struct Observation {
    pub stdout: String,
    pub stderr: String,
    pub exit: i32,
    pub files: Vec<(String, String)>,
    pub fallback: bool,
    pub effects: Vec<String>,
}

/// Runs one case in a fresh sandbox: its files written, its stdin piped, its
/// environment merged over the caller's, and a `timeout` so a hung app is a case,
/// not a hang.
pub fn observe(case: &Case, options: &RunOptions<'_>) -> Result<Observation, String> {
    let sandbox = std::env::temp_dir()
        .join("cpp-anything")
        .join("app-run")
        .join(format!("{}-{}", std::process::id(), sanitize(&case.name)));
    let _ = std::fs::remove_dir_all(&sandbox);
    let cwd = sandbox.join("cwd");
    std::fs::create_dir_all(&cwd).map_err(|error| format!("{}: {error}", cwd.display()))?;
    for (relative, contents) in &case.files {
        let path = safe_join(&cwd, relative)?;
        util::write(&path, contents)?;
    }

    let resolved = resolve_command(options.repo, options.command);
    let mut args: Vec<String> = Vec::new();
    let mut program = resolved.first().cloned().unwrap_or_default();
    if program.is_empty() {
        return Err("no command to run: pass `-- <command>` or record with one".into());
    }
    if options.timeout > 0 && util::which("timeout").is_some() {
        // `timeout` gives process-group killing without a dependency; without it,
        // a hung app hangs the run (documented in docs/how-it-works.md).
        args.push(format!("{}s", options.timeout));
        args.push(program);
        program = "timeout".to_string();
    }
    args.extend(resolved.iter().skip(1).cloned());
    args.extend(case.args.iter().cloned());

    let fallback_log = sandbox.join("fallback.log");
    let mut envs: Vec<(String, String)> = options.extra_env.clone();
    envs.extend(case.env.iter().cloned());
    envs.push((
        "CPPAN_FALLBACK_LOG".to_string(),
        fallback_log.display().to_string(),
    ));
    let stdin_path = sandbox.join("stdin.txt");
    util::write(&stdin_path, &case.stdin)?;
    // Redirect stdin from a file without a shell: `sh -c 'exec "$@" < file'`.
    let mut shell: Vec<String> = vec![
        "-c".to_string(),
        "exec \"$@\" < \"$CPPAN_STDIN\"".to_string(),
        "cppan-app".to_string(),
    ];
    shell.extend(std::iter::once(program.clone()));
    shell.extend(args.iter().cloned());
    envs.push(("CPPAN_STDIN".to_string(), stdin_path.display().to_string()));

    let run = util::run_with_env("sh", &shell, Some(&cwd), &envs);
    let exit = run.status.unwrap_or(-1);
    let stdout = run.stdout;
    let stderr = run.stderr;

    let mut files = Vec::new();
    collect_files(&cwd, &cwd, &case.files, &mut files)?;
    files.sort();
    let fallback = util::read(&fallback_log)
        .map(|text| !text.trim().is_empty())
        .unwrap_or(false);
    let _ = std::fs::remove_dir_all(&sandbox);
    Ok(Observation {
        stdout,
        stderr,
        exit,
        files,
        fallback,
        effects: Vec::new(),
    })
}

fn collect_files(
    root: &Path,
    directory: &Path,
    given: &[(String, String)],
    out: &mut Vec<(String, String)>,
) -> Result<(), String> {
    let entries = std::fs::read_dir(directory)
        .map_err(|error| format!("{}: {error}", directory.display()))?;
    for entry in entries {
        let entry = entry.map_err(|error| format!("{}: {error}", directory.display()))?;
        let path = entry.path();
        if path.is_dir() {
            collect_files(root, &path, given, out)?;
            continue;
        }
        let relative = path
            .strip_prefix(root)
            .map_err(|_| "path outside the sandbox".to_string())?
            .to_string_lossy()
            .replace('\\', "/");
        let bytes = std::fs::read(&path).map_err(|error| format!("{}: {error}", path.display()))?;
        let text = String::from_utf8_lossy(&bytes).into_owned();
        let unchanged = given
            .iter()
            .any(|(name, contents)| *name == relative && *contents == text);
        if !unchanged {
            out.push((relative, text));
        }
    }
    Ok(())
}

fn safe_join(root: &Path, relative: &str) -> Result<PathBuf, String> {
    if relative.is_empty() || relative.starts_with('/') || relative.contains("..") {
        return Err(format!(
            "{relative:?}: case files must be relative paths inside the sandbox"
        ));
    }
    Ok(root.join(relative))
}

fn sanitize(name: &str) -> String {
    name.chars()
        .map(|ch| if ch.is_ascii_alphanumeric() { ch } else { '-' })
        .collect()
}

// ---------------------------------------------------------------------------
// normalisation: timestamps, uuids, json key order
// ---------------------------------------------------------------------------

pub fn normalize(text: &str, rules: &[String]) -> String {
    let mut out = text.to_string();
    for rule in rules {
        match rule.as_str() {
            "timestamps" => out = redact_timestamps(&out),
            "uuids" => out = redact_uuids(&out),
            "key-order" => out = sort_json_keys(&out),
            other => {
                if let Some((_, literal)) = other.split_once('=') {
                    out = out.replace(literal, "[..]");
                }
            }
        }
    }
    out
}

fn redact_uuids(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut index = 0;
    while index < bytes.len() {
        if let Some(end) = uuid_at(bytes, index) {
            out.push_str("[..]");
            index = end;
        } else {
            let ch = text[index..].chars().next().unwrap();
            out.push(ch);
            index += ch.len_utf8();
        }
    }
    out
}

fn uuid_at(bytes: &[u8], start: usize) -> Option<usize> {
    // 8-4-4-4-12 hex
    let groups = [8usize, 4, 4, 4, 12];
    let mut index = start;
    for (group, length) in groups.iter().enumerate() {
        for _ in 0..*length {
            if index >= bytes.len() || !bytes[index].is_ascii_hexdigit() {
                return None;
            }
            index += 1;
        }
        if group < groups.len() - 1 {
            if index >= bytes.len() || bytes[index] != b'-' {
                return None;
            }
            index += 1;
        }
    }
    Some(index)
}

fn redact_timestamps(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut index = 0;
    while index < bytes.len() {
        if let Some(end) = timestamp_at(bytes, index) {
            out.push_str("[..]");
            index = end;
            continue;
        }
        let ch = text[index..].chars().next().unwrap();
        out.push(ch);
        index += ch.len_utf8();
    }
    out
}

fn digits(bytes: &[u8], start: usize, count: usize) -> Option<usize> {
    if start + count > bytes.len() {
        return None;
    }
    if bytes[start..start + count].iter().all(u8::is_ascii_digit) {
        Some(start + count)
    } else {
        None
    }
}

fn timestamp_at(bytes: &[u8], start: usize) -> Option<usize> {
    // YYYY-MM-DD[T ]HH:MM[:SS[.fff]][Z|±HH:MM]
    let mut index = digits(bytes, start, 4)?;
    if bytes.get(index) != Some(&b'-') {
        return None;
    }
    index = digits(bytes, index + 1, 2)?;
    if bytes.get(index) != Some(&b'-') {
        return None;
    }
    index = digits(bytes, index + 1, 2)?;
    match bytes.get(index) {
        Some(b'T') | Some(b' ') => index += 1,
        _ => return None,
    }
    index = digits(bytes, index, 2)?;
    if bytes.get(index) != Some(&b':') {
        return None;
    }
    index = digits(bytes, index + 1, 2)?;
    if bytes.get(index) == Some(&b':') {
        index = digits(bytes, index + 1, 2)?;
        if bytes.get(index) == Some(&b'.') {
            let mut fraction = index + 1;
            while fraction < bytes.len() && bytes[fraction].is_ascii_digit() {
                fraction += 1;
            }
            if fraction > index + 1 {
                index = fraction;
            }
        }
    }
    match bytes.get(index) {
        Some(b'Z') => index += 1,
        Some(b'+') | Some(b'-') => {
            if let Some(after) = digits(bytes, index + 1, 2) {
                if bytes.get(after) == Some(&b':') {
                    if let Some(after_minutes) = digits(bytes, after + 1, 2) {
                        index = after_minutes;
                    }
                }
            }
        }
        _ => {}
    }
    Some(index)
}

fn sort_json_keys(text: &str) -> String {
    let trimmed = text.trim();
    let Ok(value) = Json::parse(trimmed) else {
        return text.to_string();
    };
    let mut sorted = value;
    sort_keys(&mut sorted);
    sorted.to_string_compact()
}

fn sort_keys(value: &mut Json) {
    match value {
        Json::Array(items) => items.iter_mut().for_each(sort_keys),
        Json::Object(pairs) => {
            for (_, item) in pairs.iter_mut() {
                sort_keys(item);
            }
            pairs.sort_by(|a, b| a.0.cmp(&b.0));
        }
        _ => {}
    }
}

/// Two runs must agree; when they do not, the case is kept but marked unstable and
/// replay reports it instead of failing on a value the app does not control.
pub fn settle(
    name: &str,
    source: &str,
    first: Observation,
    second: Observation,
    rules: &[String],
) -> Case {
    let same = first.stdout == second.stdout
        && first.stderr == second.stderr
        && first.exit == second.exit
        && first.files == second.files;
    let unstable = !same;
    let stdout = normalize(&first.stdout, rules);
    let stderr = normalize(&first.stderr, rules);
    let mut files: BTreeMap<String, String> = BTreeMap::new();
    for (path, contents) in first.files {
        files.insert(path, normalize(&contents, rules));
    }
    Case {
        name: name.to_string(),
        args: Vec::new(),
        stdin: String::new(),
        env: Vec::new(),
        files: Vec::new(),
        expect: Expect {
            http: None,
            stdout,
            stderr,
            exit: first.exit,
            files: files.into_iter().collect(),
            fallback: first.fallback,
            effects: first.effects,
        },
        unstable,
        source: source.to_string(),
        request: None,
    }
}

/// Text comparison for replay: a case that was unstable when recorded is reported,
/// not failed, because the app itself did not produce one answer.
pub fn compare(case: &Case, observed: &Observation, rules: &[String]) -> Option<String> {
    if case.unstable {
        return None;
    }
    let stdout = normalize(&observed.stdout, rules);
    if stdout != case.expect.stdout {
        return Some(format!(
            "stdout: expected {:?}, got {:?}",
            util::truncate_text(&case.expect.stdout, 120),
            util::truncate_text(&stdout, 120)
        ));
    }
    let stderr = normalize(&observed.stderr, rules);
    if stderr != case.expect.stderr {
        return Some(format!(
            "stderr: expected {:?}, got {:?}",
            util::truncate_text(&case.expect.stderr, 120),
            util::truncate_text(&stderr, 120)
        ));
    }
    if observed.exit != case.expect.exit {
        return Some(format!(
            "exit: expected {}, got {}",
            case.expect.exit, observed.exit
        ));
    }
    let mut files: BTreeMap<String, String> = BTreeMap::new();
    for (path, contents) in &case.expect.files {
        files.insert(path.clone(), normalize(contents, rules));
    }
    let observed_files: BTreeMap<String, String> = observed
        .files
        .iter()
        .map(|(path, contents)| (path.clone(), normalize(contents, rules)))
        .collect();
    if files != observed_files {
        let missing: Vec<&String> = files
            .keys()
            .filter(|key| !observed_files.contains_key(*key))
            .collect();
        let extra: Vec<&String> = observed_files
            .keys()
            .filter(|key| !files.contains_key(*key))
            .collect();
        if !missing.is_empty() || !extra.is_empty() {
            return Some(format!("files: missing {missing:?}, extra {extra:?}"));
        }
        return Some("files: contents differ".to_string());
    }
    None
}
