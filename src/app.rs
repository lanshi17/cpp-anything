//! `app`: record a CLI black-box, replay any build against it, and put a C++ front
//! door in front of the original so the app can ship before every unit is ported.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use crate::appcase::{self, Case, Config, RunOptions};
use crate::apphttp;
use crate::cli::Parsed;
use crate::json::Json;
use crate::util;

pub fn command(args: &Parsed) -> Result<i32, String> {
    let sub = args
        .positional
        .first()
        .map(String::as_str)
        .unwrap_or("record");
    let rest = ParsedView::new(args, 1);
    match sub {
        "record" => record(&rest),
        "replay" => replay(&rest),
        "front-door" | "front_door" => front_door(&rest),
        "wire" => wire(&rest),
        "lift" => lift(&rest),
        other => Err(format!(
            "unknown app command {other:?}; try record, replay, front-door, wire, lift"
        )),
    }
}

/// Positional arguments after the subcommand, keeping the same option maps.
struct ParsedView<'a> {
    args: &'a Parsed,
    skip: usize,
}

impl<'a> ParsedView<'a> {
    fn new(args: &'a Parsed, skip: usize) -> Self {
        ParsedView { args, skip }
    }

    fn positional(&self, index: usize) -> Option<&str> {
        self.args
            .positional
            .get(index + self.skip)
            .map(String::as_str)
    }

    fn value(&self, name: &str) -> Option<&str> {
        self.args.value(name)
    }

    fn flag(&self, name: &str) -> bool {
        self.args.flag(name)
    }

    fn number(&self, name: &str) -> Option<u64> {
        self.args.number(name)
    }

    fn values(&self, name: &str) -> Vec<String> {
        self.args.many(name)
    }

    fn trailing(&self) -> &[String] {
        &self.args.trailing
    }
}

fn repo_of<'a>(view: &ParsedView<'a>) -> PathBuf {
    view.positional(0)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."))
}

// ---------------------------------------------------------------------------
// record
// ---------------------------------------------------------------------------

fn record(view: &ParsedView<'_>) -> Result<i32, String> {
    let repo = repo_of(view);
    if view.flag("http")
        || view.number("port").is_some()
        || Config::load(&repo.join(crate::appcase::CASES_DIR))
            .map(|config| config.http)
            .unwrap_or(false)
    {
        return record_http(view, &repo);
    }
    let dir = repo.join(appcase::CASES_DIR);
    let mut config = Config::load(&dir).unwrap_or_default();

    let mut run: Vec<String> = view.trailing().to_vec();
    if run.is_empty() {
        run = config.run.clone();
    }
    if run.is_empty() {
        return Err(
            "app record needs the command that runs the original, after `--`: \
             `cpp-anything app record . -- python3 cli.py`"
                .into(),
        );
    }
    // The recorded command keeps its repository-relative words: a checkout at another
    // path still runs it, and `observe` resolves them against the repository.
    config.run = run.clone();
    config.timeout_secs = view.number("timeout").unwrap_or(config.timeout_secs).max(1);
    if !view.values("normalize").is_empty() {
        config.normalize = view.values("normalize");
    }
    if let Some(tolerance) = view.value("float-tolerance") {
        config.float_tolerance = tolerance
            .parse()
            .map_err(|_| "--float-tolerance is not a number".to_string())?;
    }
    if !view.values("effects").is_empty() {
        config.effects = view
            .values("effects")
            .into_iter()
            .flat_map(|value| value.split(',').map(str::to_string).collect::<Vec<_>>())
            .collect();
    }
    if !view.values("effect-env").is_empty() {
        config.effect_env = view.values("effect-env");
    }
    if let Some(command) = view.value("db-reset") {
        config.db_reset = Some(command.to_string());
    }
    if let Some(command) = view.value("db-dump") {
        config.db_dump = Some(command.to_string());
    }
    if view.flag("http") || view.number("port").is_some() {
        config.http = true;
    }
    if let Some(port) = view.number("port") {
        config.port = Some(port as u16);
    }
    if !view.values("arg").is_empty() {
        config.args = view.values("arg");
    }
    if !view.values("fresh").is_empty() {
        config.fresh = view.values("fresh");
    }
    if let Some(pace) = view.number("pace") {
        config.pace_ms = pace;
    }
    if let Some(name) = view.value("name") {
        config.name = name.to_string();
    } else if config.name.is_empty() {
        config.name = discover_name(&repo, &run);
    }

    let mut cases: Vec<Case> = Vec::new();
    let mut sources: Vec<(String, Case)> = Vec::new();
    if !view.flag("no-traps") {
        let found = discover_cases(&repo, &config)?;
        for (source, case) in found.examples.into_iter().chain(found.inputs) {
            sources.push((source, case));
        }
    } else {
        let found = discover_cases(&repo, &config)?;
        for (source, case) in found.inputs {
            sources.push((source, case));
        }
    }
    if sources.is_empty() {
        sources.push((
            "no-args".to_string(),
            Case {
                name: String::new(),
                args: Vec::new(),
                stdin: String::new(),
                env: Vec::new(),
                files: Vec::new(),
                expect: Default::default(),
                unstable: false,
                source: "no-args".into(),
                request: None,
            },
        ));
    }

    // Semantic traps: vary each argument and the stdin, so the app meets values the
    // docs never show. This is where whole-app ports break.
    let mut trapped: Vec<(String, Case)> = Vec::new();
    for (source, case) in &sources {
        trapped.push((source.clone(), case.clone()));
    }
    if !view.flag("no-traps") {
        for (_, case) in &sources {
            trapped.extend(trap_cases(case));
        }
    }
    trapped.truncate(300);

    let mut taken: BTreeSet<String> = BTreeSet::new();
    for (index, (source, mut case)) in trapped.into_iter().enumerate() {
        let base = format!(
            "{}-{:03}-{}",
            label_of(&source),
            index + 1,
            slug_of(case.args.first().map(String::as_str).unwrap_or("no-args"))
        );
        let mut name = base.clone();
        let mut suffix = 2;
        while !taken.insert(name.clone()) {
            name = format!("{base}-{suffix}");
            suffix += 1;
        }
        case.name = name;
        case.source = source;

        let mut sinks = crate::effects::Sinks::start(&config.effects)?;
        let extra_env = sinks.env(&resolved_effect_env(&sinks, &config.effect_env));
        let options = RunOptions {
            repo: &repo,
            command: &config.run,
            timeout: config.timeout_secs,
            extra_env,
            pre_command: config.db_reset.clone(),
            post_command: config.db_dump.clone(),
        };
        let first = appcase::observe(&case, &options)?;
        let first_database = first.database.clone();
        let second = if config.effects.is_empty() && config.db_dump.is_none() {
            appcase::observe(&case, &options)?
        } else {
            // Side effects are not idempotent: a second run would double them, so the
            // case is recorded once and marked stable.
            first.clone()
        };
        let mut settled =
            appcase::settle(&case.name, &case.source, first, second, &config.normalize);
        let ports = sinks.ports();
        let mut effects: Vec<String> = sinks
            .take()
            .into_iter()
            .map(|line| mask_ports(&appcase::normalize(&line, &config.normalize), &ports))
            .collect();
        effects.extend(
            first_database
                .into_iter()
                .map(|line| appcase::normalize(&line, &config.normalize)),
        );
        settled.expect.effects = effects;
        // `settle` only describes what it saw; the case's own inputs are what ran.
        settled.args = case.args.clone();
        settled.stdin = case.stdin.clone();
        settled.env = case.env.clone();
        settled.files = case.files.clone();
        if view.flag("verbose") {
            println!(
                "  {:<40} exit {:<4} {:>6} bytes out{}",
                settled.name,
                settled.expect.exit,
                settled.expect.stdout.len(),
                if settled.unstable { "  (unstable)" } else { "" }
            );
        }
        cases.push(settled);
    }

    std::fs::create_dir_all(&dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    config.save(&dir)?;
    appcase::save_cases(&repo, &cases)?;
    if view.flag("json") {
        let doc = Json::obj([
            ("schema", Json::str("cpp-anything/app-record@1")),
            ("app", Json::str(config.name.clone())),
            (
                "run",
                Json::Array(
                    config
                        .run
                        .iter()
                        .map(|part| Json::str(part.clone()))
                        .collect(),
                ),
            ),
            ("cases", Json::Int(cases.len() as i64)),
            (
                "unstable",
                Json::Int(cases.iter().filter(|case| case.unstable).count() as i64),
            ),
            ("dir", Json::str(dir.display().to_string())),
        ]);
        println!("{}", doc.to_string_pretty());
    } else {
        println!(
            "cpp-anything app record: {} — {} case(s) in {} ({} unstable)",
            config.name,
            cases.len(),
            dir.display(),
            cases.iter().filter(|case| case.unstable).count()
        );
        println!("  next: `cpp-anything app replay .` to check the original against its own cases");
        println!("        `cpp-anything app front-door .` to put a C++ front door in front of it");
    }
    Ok(0)
}

fn label_of(source: &str) -> String {
    match source {
        "docs" => "example".into(),
        "inputs" => "input".into(),
        "trap" => "trap".into(),
        other => other.to_string(),
    }
}

fn slug_of(text: &str) -> String {
    let slug: String = text
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() {
                ch.to_ascii_lowercase()
            } else {
                '-'
            }
        })
        .collect();
    let trimmed = slug.trim_matches('-').to_string();
    if trimmed.is_empty() {
        "no-args".into()
    } else {
        trimmed.chars().take(40).collect()
    }
}

const INT_TRAPS: &[&str] = &[
    "-7",
    "0",
    "-1",
    "2147483648",
    "9007199254740993",
    "9223372036854775807",
];
const FLOAT_TRAPS: &[&str] = &[
    "-0.0",
    "0.1",
    "1e21",
    "1e-7",
    "5e-324",
    "1.7976931348623157e308",
];
const STRING_TRAPS: &[&str] = &[
    "héllo wörld",
    "👍🏽 thumbs",
    "日本語",
    "",
    "İstanbul",
    "straße",
    "👨‍👩‍👧 family",
    "a\u{a0}b",
];
const STDIN_TRAPS: &[&str] = &["", "naïve café 👍🏽\nsecond line\n", "  spaced   out  \n\n"];

fn trap_cases(case: &Case) -> Vec<(String, Case)> {
    let mut out = Vec::new();
    for (index, arg) in case.args.iter().enumerate() {
        if arg.starts_with('-') && arg.parse::<f64>().is_err() {
            continue; // a flag, not a value
        }
        let values: Vec<String> = if arg.parse::<i128>().is_ok() {
            INT_TRAPS.iter().map(|value| value.to_string()).collect()
        } else if arg.contains('.') && arg.parse::<f64>().is_ok() {
            FLOAT_TRAPS.iter().map(|value| value.to_string()).collect()
        } else {
            STRING_TRAPS.iter().map(|value| value.to_string()).collect()
        };
        for value in values {
            if value == *arg {
                continue;
            }
            let mut clone = case.clone();
            clone.args[index] = value;
            clone.source = "trap".into();
            out.push(("trap".to_string(), clone));
        }
    }
    for stdin in STDIN_TRAPS {
        if *stdin == case.stdin {
            continue;
        }
        let mut clone = case.clone();
        clone.stdin = (*stdin).to_string();
        clone.source = "trap".into();
        out.push(("trap".to_string(), clone));
    }
    out
}

// ---------------------------------------------------------------------------
// discovery: docs examples and --inputs
// ---------------------------------------------------------------------------

type Found = Vec<(String, Case)>;

struct Discovered {
    examples: Found,
    inputs: Found,
}

fn discover_cases(repo: &Path, config: &Config) -> Result<Discovered, String> {
    let mut examples = Vec::new();
    for path in walk_docs(repo, 0) {
        let text = util::read(&path).unwrap_or_default();
        let is_shell = path.extension().map(|ext| ext == "sh").unwrap_or(false);
        for line in commands_in(&text, is_shell) {
            let Some(case) = case_from_command(&line, &config.name) else {
                continue;
            };
            examples.push(("docs".to_string(), case));
        }
    }
    let mut inputs = Vec::new();
    if let Some(path) = std::env::var_os("CPPAN_APP_INPUTS").map(PathBuf::from) {
        inputs.extend(read_inputs(&path)?);
    }
    Ok(Discovered { examples, inputs })
}

fn walk_docs(directory: &Path, depth: usize) -> Vec<PathBuf> {
    if depth > 4 {
        return Vec::new();
    }
    let skip = [
        ".git",
        "node_modules",
        "target",
        "vendor",
        "dist",
        "build",
        "cpp",
        "out",
        ".venv",
        "venv",
        "__pycache__",
    ];
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir(directory) else {
        return out;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().into_owned();
        if path.is_dir() {
            if skip.contains(&name.as_str()) || name.starts_with('.') {
                continue;
            }
            out.extend(walk_docs(&path, depth + 1));
            continue;
        }
        if matches!(
            path.extension().and_then(|ext| ext.to_str()),
            Some("md") | Some("markdown") | Some("sh")
        ) {
            out.push(path);
        }
    }
    out
}

fn commands_in(text: &str, shell_file: bool) -> Vec<String> {
    let mut out = Vec::new();
    let mut in_fence = false;
    let mut fence_shell = false;
    for line in text.lines() {
        let trimmed = line.trim();
        if let Some(info) = trimmed.strip_prefix("```") {
            if in_fence {
                in_fence = false;
                fence_shell = false;
            } else {
                in_fence = true;
                fence_shell = matches!(info.trim(), "sh" | "bash" | "shell" | "zsh");
            }
            continue;
        }
        let command = if shell_file || (in_fence && fence_shell) {
            trimmed.strip_prefix("$ ").unwrap_or(trimmed)
        } else {
            match trimmed.strip_prefix("$ ") {
                Some(command) => command,
                None => continue,
            }
        };
        if command.is_empty() || command.starts_with('#') {
            continue;
        }
        if command.contains("$(")
            || command.contains('`')
            || command.contains('|')
            || command.contains('>')
            || command.contains('<')
            || command.contains("&&")
            || command.contains(';')
        {
            continue;
        }
        out.push(command.to_string());
    }
    out
}

fn case_from_command(command: &str, app: &str) -> Option<Case> {
    let words = tokenize(command)?;
    let mut env = Vec::new();
    let mut index = 0;
    while index < words.len() {
        if let Some((key, value)) = words[index].split_once('=') {
            if !key.is_empty()
                && key
                    .chars()
                    .all(|ch| ch.is_ascii_alphanumeric() || ch == '_')
            {
                env.push((key.to_string(), value.to_string()));
                index += 1;
                continue;
            }
        }
        break;
    }
    let program = words.get(index)?.clone();
    let stem = Path::new(&program)
        .file_stem()?
        .to_string_lossy()
        .into_owned();
    if !app.is_empty() && stem != app {
        return None;
    }
    let args = words[index + 1..].to_vec();
    Some(Case {
        name: String::new(),
        args,
        stdin: String::new(),
        env,
        files: Vec::new(),
        expect: Default::default(),
        unstable: false,
        source: "docs".into(),
        request: None,
    })
}

fn tokenize(command: &str) -> Option<Vec<String>> {
    let mut out = Vec::new();
    let mut current = String::new();
    let mut quote: Option<char> = None;
    let mut chars = command.chars().peekable();
    while let Some(ch) = chars.next() {
        match quote {
            Some(q) if ch == q => quote = None,
            Some(_) => {
                if ch == '\\' {
                    if let Some(next) = chars.next() {
                        current.push(next);
                    }
                } else {
                    current.push(ch);
                }
            }
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
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

fn read_inputs(path: &Path) -> Result<Vec<(String, Case)>, String> {
    let text = util::read(path)?;
    let mut out = Vec::new();
    for (number, line) in text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)
            .map_err(|error| format!("{}:{}: {error}", path.display(), number + 1))?;
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
        let env = doc
            .get("env")
            .and_then(Json::as_object)
            .map(|entries| {
                entries
                    .iter()
                    .map(|(key, value)| (key.clone(), value.as_str().unwrap_or("").to_string()))
                    .collect()
            })
            .unwrap_or_default();
        let files = doc
            .get("files")
            .and_then(Json::as_object)
            .map(|entries| {
                entries
                    .iter()
                    .map(|(key, value)| (key.clone(), value.as_str().unwrap_or("").to_string()))
                    .collect()
            })
            .unwrap_or_default();
        out.push((
            "inputs".to_string(),
            Case {
                name: doc
                    .get("name")
                    .and_then(Json::as_str)
                    .unwrap_or("")
                    .to_string(),
                args: strings("args"),
                stdin: doc
                    .get("stdin")
                    .and_then(Json::as_str)
                    .unwrap_or("")
                    .to_string(),
                env,
                files,
                expect: Default::default(),
                unstable: false,
                source: "inputs".into(),
                request: None,
            },
        ));
    }
    Ok(out)
}

fn discover_name(repo: &Path, run: &[String]) -> String {
    for word in run {
        if word.starts_with('-') || word.contains('/') || word.contains('\\') {
            continue;
        }
        let stem = Path::new(word)
            .file_stem()
            .map(|stem| stem.to_string_lossy().into_owned());
        if let Some(stem) = stem {
            if !matches!(
                stem.as_str(),
                "python"
                    | "python3"
                    | "node"
                    | "sh"
                    | "bash"
                    | "ruby"
                    | "go"
                    | "java"
                    | "php"
                    | "dotnet"
            ) {
                return stem;
            }
        }
    }
    for manifest in ["pyproject.toml", "package.json"] {
        let path = repo.join(manifest);
        let Ok(text) = util::read(&path) else {
            continue;
        };
        if let Some(name) = script_name(&text) {
            return name;
        }
    }
    "app".to_string()
}

fn script_name(text: &str) -> Option<String> {
    // `[project.scripts]` or `"bin": { "name": ... }` without a TOML/JSON dependency:
    // find the first `name =` / `"name":` after the marker.
    let mut lines = text.lines().peekable();
    while let Some(line) = lines.next() {
        let trimmed = line.trim();
        if trimmed == "[project.scripts]"
            || trimmed == "[tool.poetry.scripts]"
            || trimmed.contains("\"bin\"")
        {
            for candidate in lines.by_ref().take(4) {
                let candidate = candidate.trim();
                if let Some((key, _)) = candidate.split_once('=') {
                    let key = key.trim().trim_matches('"');
                    if !key.is_empty() && key != "}" {
                        return Some(key.to_string());
                    }
                }
                if let Some((key, _)) = candidate.split_once(':') {
                    let key = key.trim().trim_matches('"');
                    if !key.is_empty() && key != "}" && key != "{" {
                        return Some(key.to_string());
                    }
                }
            }
        }
    }
    None
}

// ---------------------------------------------------------------------------
// replay
// ---------------------------------------------------------------------------

fn replay(view: &ParsedView<'_>) -> Result<i32, String> {
    let repo = repo_of(view);
    let dir = repo.join(appcase::CASES_DIR);
    let config = Config::load(&dir)?;
    if config.http {
        let build: Vec<String> = match view.value("build") {
            Some(command) => command.split_whitespace().map(str::to_string).collect(),
            None => config.run.clone(),
        };
        return replay_http(&repo, &config, &build, view);
    }
    let mut cases = appcase::load_cases(&repo)?;
    if let Some(only) = view.value("case") {
        cases.retain(|case| case.name == only);
        if cases.is_empty() {
            return Err(format!(
                "no case named {only:?}: `app replay --json` lists them"
            ));
        }
    }
    let build: Vec<String> = match view.value("build") {
        Some(command) => command.split_whitespace().map(str::to_string).collect(),
        None => config.run.clone(),
    };
    if build.is_empty() {
        return Err("no build command: pass `--build <command>` or record with one".into());
    }
    let options = RunOptions {
        repo: &repo,
        command: &build,
        timeout: view.number("timeout").unwrap_or(config.timeout_secs),
        extra_env: Vec::new(),
        pre_command: None,
        post_command: None,
    };

    let mut passed = 0usize;
    let mut unstable = 0usize;
    let mut fallbacks = 0usize;
    let mut failures = Vec::new();
    let mut effects_checked = 0usize;
    for case in &cases {
        let mut sinks = crate::effects::Sinks::start(&config.effects)?;
        let extra_env = sinks.env(&resolved_effect_env(&sinks, &config.effect_env));
        let case_options = RunOptions {
            repo: &repo,
            command: options.command,
            timeout: options.timeout,
            extra_env,
            pre_command: config.db_reset.clone(),
            post_command: config.db_dump.clone(),
        };
        let mut observed = appcase::observe(case, &case_options)?;
        let ports = sinks.ports();
        observed.effects = sinks
            .take()
            .into_iter()
            .map(|line| mask_ports(&appcase::normalize(&line, &config.normalize), &ports))
            .collect();
        observed.effects.extend(
            observed
                .database
                .iter()
                .map(|line| appcase::normalize(line, &config.normalize)),
        );
        if case.unstable {
            unstable += 1;
        }
        if observed.fallback {
            fallbacks += 1;
        }
        if !case.expect.effects.is_empty() {
            effects_checked += 1;
            if observed.effects != case.expect.effects {
                failures.push((
                    case.name.clone(),
                    format!("effects: expected {:?}", case.expect.effects),
                ));
                continue;
            }
        }
        match appcase::compare(case, &observed, &config.normalize) {
            None => passed += 1,
            Some(reason) => failures.push((case.name.clone(), reason)),
        }
    }

    let total = cases.len();
    if view.flag("json") {
        let doc = Json::obj([
            ("schema", Json::str("cpp-anything/app-replay@1")),
            ("app", Json::str(config.name.clone())),
            (
                "build",
                Json::Array(build.iter().map(|part| Json::str(part.clone())).collect()),
            ),
            ("cases", Json::Int(total as i64)),
            ("passed", Json::Int(passed as i64)),
            ("unstable", Json::Int(unstable as i64)),
            ("fallback", Json::Int(fallbacks as i64)),
            ("effects_checked", Json::Int(effects_checked as i64)),
            (
                "failures",
                Json::Array(
                    failures
                        .iter()
                        .map(|(name, reason)| {
                            Json::obj([
                                ("case", Json::str(name.clone())),
                                ("reason", Json::str(reason.clone())),
                            ])
                        })
                        .collect(),
                ),
            ),
        ]);
        println!("{}", doc.to_string_pretty());
    } else {
        println!(
            "cpp-anything app replay: {} — {}/{} cases passed{}",
            config.name,
            passed,
            total,
            if unstable > 0 {
                format!(" ({unstable} unstable)")
            } else {
                String::new()
            }
        );
        println!("  build: {}", build.join(" "));
        if fallbacks > 0 {
            println!("  fallback: {fallbacks}/{total} cases ran the original instead of the build");
        }
        for (name, reason) in failures.iter().take(10) {
            println!("  ✗ {name}\n    {reason}");
        }
        if failures.len() > 10 {
            println!("  … {} more", failures.len() - 10);
        }
    }
    Ok(if failures.is_empty() { 0 } else { 1 })
}

// ---------------------------------------------------------------------------
// HTTP: servers
// ---------------------------------------------------------------------------

fn record_http(view: &ParsedView<'_>, repo: &Path) -> Result<i32, String> {
    let dir = repo.join(crate::appcase::CASES_DIR);
    let mut config = Config::load(&dir).unwrap_or_default();
    let run: Vec<String> = if view.trailing().is_empty() {
        config.run.clone()
    } else {
        view.trailing().to_vec()
    };
    if run.is_empty() {
        return Err(
            "app record --http needs the command that starts the server, after `--`:              `cpp-anything app record . --http -- python3 server.py`"
                .into(),
        );
    }
    config.http = true;
    config.run = run;
    config.timeout_secs = view.number("timeout").unwrap_or(config.timeout_secs).max(1);
    if let Some(name) = view.value("name") {
        config.name = name.to_string();
    } else if config.name.is_empty() {
        config.name = discover_name(repo, &config.run);
    }
    if let Some(port) = view.number("port") {
        config.port = Some(port as u16);
    }
    if !view.values("arg").is_empty() {
        config.args = view.values("arg");
    }
    if !view.values("normalize").is_empty() {
        config.normalize = view.values("normalize");
    }
    if !view.values("fresh").is_empty() {
        config.fresh = view.values("fresh");
    }
    if let Some(pace) = view.number("pace") {
        config.pace_ms = pace;
    }

    let port = config.port.unwrap_or(apphttp::free_port()?);
    let sandbox = std::env::temp_dir()
        .join("cpp-anything")
        .join("app-http")
        .join(format!("{}-{}", std::process::id(), config.name));
    let _ = std::fs::remove_dir_all(&sandbox);
    std::fs::create_dir_all(&sandbox).map_err(|error| format!("{}: {error}", sandbox.display()))?;
    for path in &config.fresh {
        let source = repo.join(path);
        let target = sandbox.join(path);
        if source.is_dir() {
            copy_tree(&source, &target)?;
        } else if source.is_file() {
            if let Some(parent) = target.parent() {
                std::fs::create_dir_all(parent)
                    .map_err(|error| format!("{}: {error}", parent.display()))?;
            }
            std::fs::copy(&source, &target)
                .map_err(|error| format!("{}: {error}", source.display()))?;
        }
    }

    {
        let mut server = apphttp::start(
            &config.run,
            repo,
            &sandbox,
            port,
            config.timeout_secs,
            &[],
        )
        .or_else(|error| {
            // A server that ignores PORT can still be recorded on a fixed port.
            match config.port {
                Some(port) => {
                    apphttp::start(&config.run, repo, &sandbox, port, config.timeout_secs, &[])
                }
                None => Err(error),
            }
        })?;

        let mut requests: Vec<(String, apphttp::HttpRequest)> = Vec::new();
        for path in walk_docs(repo, 0) {
            let text = util::read(&path).unwrap_or_default();
            for request in apphttp::requests_in(&text) {
                if !request.path.is_empty() {
                    requests.push(("docs".to_string(), request));
                }
            }
        }
        requests.extend(read_http_inputs()?);
        if requests.is_empty() {
            requests.push((
                "no-request".to_string(),
                apphttp::HttpRequest {
                    method: "GET".into(),
                    path: "/".into(),
                    headers: Vec::new(),
                    body: String::new(),
                },
            ));
        }
        if !view.flag("no-traps") {
            let mut trapped = Vec::new();
            for (source, request) in &requests {
                trapped.push((source.clone(), request.clone()));
                for (path, body) in [
                    ("/%C3%BCnicode", String::new()),
                    ("/?q=%F0%9F%91%8D", String::new()),
                ] {
                    let mut clone = request.clone();
                    clone.path = path.to_string();
                    if body.is_empty() {
                        trapped.push(("trap".to_string(), clone));
                    }
                }
                if !request.body.is_empty() {
                    for body in ["{}", "{\"n\":1e21,\"neg\":-0.0,\"s\":\"café\"}", ""] {
                        let mut clone = request.clone();
                        clone.body = body.to_string();
                        trapped.push(("trap".to_string(), clone));
                    }
                }
            }
            requests = trapped;
        }

        let mut cases = Vec::new();
        let mut taken: std::collections::BTreeSet<String> = std::collections::BTreeSet::new();
        for (index, (source, request)) in requests.into_iter().enumerate() {
            let base = format!(
                "{}-{:03}-{}",
                label_of(&source),
                index + 1,
                slug_of(request.path.trim_start_matches('/'))
            );
            let mut name = base.clone();
            let mut suffix = 2;
            while !taken.insert(name.clone()) {
                name = format!("{base}-{suffix}");
                suffix += 1;
            }
            let response = apphttp::request(server.port, &request)?;
            let expect_http = apphttp::expectation(response);
            cases.push(Case {
                request: Some(request),
                name,
                args: Vec::new(),
                stdin: String::new(),
                env: Vec::new(),
                files: Vec::new(),
                expect: appcase::Expect {
                    http: Some(expect_http),
                    ..Default::default()
                },
                unstable: false,
                source,
            });
            if config.pace_ms > 0 {
                std::thread::sleep(std::time::Duration::from_millis(config.pace_ms));
            }
        }
        server.stop();
        config.save(&dir)?;
        appcase::save_cases(repo, &cases)?;
        if view.flag("json") {
            let doc = Json::obj([
                ("schema", Json::str("cpp-anything/app-record@1")),
                ("app", Json::str(config.name.clone())),
                ("http", Json::Bool(true)),
                ("port", Json::Int(i64::from(port))),
                ("cases", Json::Int(cases.len() as i64)),
            ]);
            println!("{}", doc.to_string_pretty());
        } else {
            println!(
                "cpp-anything app record: {} — {} HTTP case(s) on port {port} in {}",
                config.name,
                cases.len(),
                dir.display()
            );
            println!("  next: `cpp-anything app replay .` and `cpp-anything app front-door .`");
        }
    }
    Ok(0)
}

fn replay_http(
    repo: &Path,
    config: &Config,
    build: &[String],
    view: &ParsedView<'_>,
) -> Result<i32, String> {
    let cases = appcase::load_cases(repo)?;
    let port = config.port.unwrap_or(apphttp::free_port()?);
    let sandbox = std::env::temp_dir()
        .join("cpp-anything")
        .join("app-http-replay")
        .join(std::process::id().to_string());
    let _ = std::fs::remove_dir_all(&sandbox);
    std::fs::create_dir_all(&sandbox).map_err(|error| format!("{}: {error}", sandbox.display()))?;
    let mut server = apphttp::start(build, repo, &sandbox, port, config.timeout_secs, &[])?;
    let mut passed = 0usize;
    let mut failures = Vec::new();
    for case in &cases {
        if let Some(only) = view.value("case") {
            if case.name != only {
                continue;
            }
        }
        let Some(request) = &case.request else {
            continue;
        };
        match apphttp::request(server.port, request) {
            Ok(response) => {
                if let Some(expected) = &case.expect.http {
                    let body = appcase::normalize(&response.body, &config.normalize);
                    let expected_body = appcase::normalize(&expected.body, &config.normalize);
                    let content_type = apphttp::content_type(&response);
                    if response.status != expected.status {
                        failures.push((
                            case.name.clone(),
                            format!(
                                "status: expected {}, got {}",
                                expected.status, response.status
                            ),
                        ));
                    } else if !expected.content_type.is_empty()
                        && content_type != expected.content_type
                    {
                        failures.push((
                            case.name.clone(),
                            format!(
                                "content-type: expected {:?}, got {content_type:?}",
                                expected.content_type
                            ),
                        ));
                    } else if body != expected_body {
                        failures.push((
                            case.name.clone(),
                            format!(
                                "body: expected {:?}, got {:?}",
                                util::truncate_text(&expected_body, 120),
                                util::truncate_text(&body, 120)
                            ),
                        ));
                    } else {
                        passed += 1;
                    }
                }
            }
            Err(error) => failures.push((case.name.clone(), error)),
        }
        if config.pace_ms > 0 {
            std::thread::sleep(std::time::Duration::from_millis(config.pace_ms));
        }
    }
    server.stop();
    if view.flag("json") {
        let doc = Json::obj([
            ("schema", Json::str("cpp-anything/app-replay@1")),
            ("app", Json::str(config.name.clone())),
            ("http", Json::Bool(true)),
            ("cases", Json::Int(cases.len() as i64)),
            ("passed", Json::Int(passed as i64)),
            (
                "failures",
                Json::Array(
                    failures
                        .iter()
                        .map(|(name, reason)| {
                            Json::obj([
                                ("case", Json::str(name.clone())),
                                ("reason", Json::str(reason.clone())),
                            ])
                        })
                        .collect(),
                ),
            ),
        ]);
        println!("{}", doc.to_string_pretty());
    } else {
        println!(
            "cpp-anything app replay: {} (HTTP) — {}/{} cases passed",
            config.name,
            passed,
            cases.len()
        );
        for (name, reason) in failures.iter().take(10) {
            println!("  ✗ {name}\n    {reason}");
        }
    }
    Ok(if failures.is_empty() { 0 } else { 1 })
}

fn read_http_inputs() -> Result<Vec<(String, apphttp::HttpRequest)>, String> {
    let Some(path) = std::env::var_os("CPPAN_APP_INPUTS").map(PathBuf::from) else {
        return Ok(Vec::new());
    };
    let text = util::read(&path)?;
    let mut out = Vec::new();
    for (number, line) in text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)
            .map_err(|error| format!("{}:{}: {error}", path.display(), number + 1))?;
        let headers = doc
            .get("headers")
            .and_then(Json::as_object)
            .map(|entries| {
                entries
                    .iter()
                    .map(|(key, value)| (key.clone(), value.as_str().unwrap_or("").to_string()))
                    .collect()
            })
            .unwrap_or_default();
        out.push((
            "inputs".to_string(),
            apphttp::HttpRequest {
                method: doc
                    .get("method")
                    .and_then(Json::as_str)
                    .unwrap_or("GET")
                    .to_string(),
                path: doc
                    .get("path")
                    .and_then(Json::as_str)
                    .unwrap_or("/")
                    .to_string(),
                headers,
                body: doc
                    .get("body")
                    .map(|body| {
                        body.as_str()
                            .map(str::to_string)
                            .unwrap_or_else(|| body.to_string_compact())
                    })
                    .unwrap_or_default(),
            },
        ));
    }
    Ok(out)
}

/// The sink's ephemeral port is not part of the app's behaviour: mask it before
/// storing or comparing an effect line.
fn mask_ports(line: &str, ports: &[String]) -> String {
    let mut out = line.to_string();
    for port in ports {
        out = out.replace(&format!("127.0.0.1:{port}"), "127.0.0.1:<sink>");
        out = out.replace(&format!("localhost:{port}"), "localhost:<sink>");
    }
    out
}

/// `--effect-env VAR=kind`: point one of the app's own settings at a sink.
fn resolved_effect_env(sinks: &crate::effects::Sinks, specs: &[String]) -> Vec<(String, String)> {
    let mut env = Vec::new();
    for spec in specs {
        let Ok((name, kind)) = crate::effects::effect_env(spec) else {
            continue;
        };
        let value = match kind.as_str() {
            "http-url" => sinks.http.as_ref().map(|sink| sink.url()),
            "smtp-host" => sinks.smtp.as_ref().map(|_| "127.0.0.1".to_string()),
            "smtp-port" => sinks.smtp.as_ref().map(|sink| sink.port.to_string()),
            "smtp-url" => sinks
                .smtp
                .as_ref()
                .map(|sink| format!("smtp://127.0.0.1:{}", sink.port)),
            _ => None,
        };
        if let Some(value) = value {
            env.push((name, value));
        }
    }
    env
}

fn copy_tree(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|error| format!("{}: {error}", to.display()))?;
    for entry in std::fs::read_dir(from).map_err(|error| format!("{}: {error}", from.display()))? {
        let entry = entry.map_err(|error| format!("{}: {error}", from.display()))?;
        let target = to.join(entry.file_name());
        if entry.path().is_dir() {
            copy_tree(&entry.path(), &target)?;
        } else {
            std::fs::copy(entry.path(), &target)
                .map_err(|error| format!("{}: {error}", target.display()))?;
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// front door
// ---------------------------------------------------------------------------

fn front_door(view: &ParsedView<'_>) -> Result<i32, String> {
    let repo = repo_of(view);
    let dir = repo.join(appcase::CASES_DIR);
    let config = Config::load(&dir)?;
    // Servers get a proxy front door: it owns PORT and forwards to the original.
    let app_dir = repo.join("cpp").join("app");
    let routes = crate::appfront::write(&app_dir, &config)?;
    println!(
        "cpp-anything app front-door: {} — {} file(s) in {}, {} route(s)",
        config.name,
        routes,
        util::short_path(&app_dir),
        crate::appfront::route_count(&app_dir)
    );
    if view.flag("no-check") {
        return Ok(0);
    }
    let build = crate::appfront::build(&app_dir)?;
    println!("  built {}", util::short_path(&build));
    let dir = repo.join(appcase::CASES_DIR);
    let config = Config::load(&dir)?;
    let code = if config.http {
        let options = ParsedView::new(view.args, 0);
        replay_http(&repo, &config, &[build.display().to_string()], &options)?
    } else {
        replay_build(&repo, &build, view)?
    };
    Ok(code)
}

fn replay_build(repo: &Path, build: &Path, view: &ParsedView<'_>) -> Result<i32, String> {
    let dir = repo.join(appcase::CASES_DIR);
    let config = Config::load(&dir)?;
    let cases = appcase::load_cases(repo)?;
    let options = RunOptions {
        repo,
        command: &[build.display().to_string()],
        timeout: view.number("timeout").unwrap_or(config.timeout_secs),
        extra_env: Vec::new(),
        pre_command: None,
        post_command: None,
    };
    let mut passed = 0usize;
    let mut fallbacks = 0usize;
    let mut failures = Vec::new();
    for case in &cases {
        let observed = appcase::observe(case, &options)?;
        if observed.fallback {
            fallbacks += 1;
        }
        match appcase::compare(case, &observed, &config.normalize) {
            None => passed += 1,
            Some(reason) => failures.push((case.name.clone(), reason)),
        }
    }
    println!(
        "  replay: {}/{} passed, {} fallback",
        passed,
        cases.len(),
        fallbacks
    );
    for (name, reason) in failures.iter().take(5) {
        println!("  ✗ {name}\n    {reason}");
    }
    Ok(if failures.is_empty() { 0 } else { 1 })
}

// ---------------------------------------------------------------------------
// wire
// ---------------------------------------------------------------------------

fn wire(view: &ParsedView<'_>) -> Result<i32, String> {
    let repo = repo_of(view);
    let unit = view
        .positional(1)
        .ok_or("app wire needs a unit: the folder or unit id it was bridged from")?;
    let command = view
        .value("command")
        .ok_or("app wire needs `--command NAME`: the CLI command this unit answers")?;
    let app_dir = repo.join("cpp").join("app");
    if !app_dir.exists() {
        return Err("no front door yet: run `cpp-anything app front-door .` first".into());
    }
    let project = view.value("project").map(Path::new);
    let file = crate::appfront::wire(&app_dir, &repo, unit, command, project)?;
    println!(
        "cpp-anything app wire: {command} → {unit}\n  wrote {} — fill the handler, then `app front-door .`",
        util::short_path(&file)
    );
    Ok(0)
}

// ---------------------------------------------------------------------------
// lift
// ---------------------------------------------------------------------------

fn lift(view: &ParsedView<'_>) -> Result<i32, String> {
    let repo = repo_of(view);
    let dir = repo.join(appcase::CASES_DIR);
    let mut config = Config::load(&dir)?;
    if config.http {
        return Err(
            "servers ship behind the proxy front door, but no HTTP route is wired yet: `app lift` \
             needs the proxy to stop forwarding, and this build has no native route. Port the \
             server's units, then teach the front door the routes they answer (`app wire` on a server \
             is not implemented yet) — until then keep the proxy: every case passes and every \
             request is accounted for in the fallback log."
                .into(),
        );
    }
    let mut build = repo
        .join("cpp")
        .join("app")
        .join("build")
        .join(if cfg!(windows) {
            "cppan_app.exe"
        } else {
            "cppan_app"
        });
    if !build.exists() {
        // A fresh checkout has no build tree: build it here rather than make the user
        // discover the order.
        build = crate::appfront::build(&repo.join("cpp").join("app"))?;
    }
    let build = build
        .canonicalize()
        .map_err(|error| format!("{}: {error}", build.display()))?;
    let code = replay_build(&repo, &build, view)?;
    if code != 0 {
        return Ok(code);
    }
    // Lifting means the front door answered every case itself: no fallback.
    let cases = appcase::load_cases(&repo)?;
    let options = RunOptions {
        repo: &repo,
        command: &[build.display().to_string()],
        timeout: config.timeout_secs,
        extra_env: Vec::new(),
        pre_command: None,
        post_command: None,
    };
    let mut fallbacks = 0usize;
    for case in &cases {
        let observed = appcase::observe(case, &options)?;
        if observed.fallback {
            fallbacks += 1;
        }
    }
    if fallbacks > 0 {
        println!(
            "cpp-anything app lift: {} case(s) still run the original; wire the units they need \
             (`app wire`) and lift again",
            fallbacks
        );
        return Ok(1);
    }
    config.lifted = true;
    config.save(&dir)?;
    util::write(
        &dir.join("STANDALONE.md"),
        &format!(
            "# {} is standalone\n\nEvery recorded case is answered by the C++ front door in \
             `cpp/app/`; the original is no longer on any path a case takes. Keep it as reference, \
             keep `cpp-anything app replay .` in CI, and keep the cases: they are the contract.\n",
            config.name
        ),
    )?;
    println!(
        "cpp-anything app lift: {} is standalone — {} case(s) answered by the front door, 0 fallback\n  \
         wrote {}",
        config.name,
        cases.len(),
        util::short_path(&dir.join("STANDALONE.md"))
    );
    Ok(0)
}
