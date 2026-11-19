//! Running the Python probe: analysis, recording, and benchmarking the original.

use std::path::{Path, PathBuf};

use crate::inventory::Unit;
use crate::json::Json;
use crate::project::Project;
use crate::util;

const PROBE: &str = include_str!("probe.py");
const SITE_HOOK: &str = include_str!("cpp_runtime/sitecustomize.py.in");

pub fn interpreter() -> String {
    std::env::var("CPPAN_PYTHON").unwrap_or_else(|_| "python3".to_string())
}

/// The probe is written to a stable path keyed by its own hash, so a run never
/// leaves a new file behind and an edit to probe.py shows up immediately.
fn probe_path() -> Result<PathBuf, String> {
    let dir = std::env::temp_dir().join("cpp-anything");
    std::fs::create_dir_all(&dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    let path = dir.join(format!("probe-{}.py", util::hash64(PROBE)));
    if !path.exists() {
        std::fs::write(&path, PROBE).map_err(|error| format!("{}: {error}", path.display()))?;
    }
    Ok(path)
}

pub fn available() -> Option<String> {
    let run = util::run(
        &interpreter(),
        &[
            "-c".to_string(),
            "import sys; print(sys.version.split()[0])".to_string(),
        ],
        None,
    );
    run.ok.then(|| run.stdout.trim().to_string())
}

pub fn probe_command(args: &[String]) -> Result<String, String> {
    probe(args, None)
}

fn probe(args: &[String], cwd: Option<&Path>) -> Result<String, String> {
    let script = probe_path()?;
    let mut argv = vec![script.display().to_string()];
    argv.extend(args.iter().cloned());
    let run = util::run(&interpreter(), &argv, cwd);
    if !run.ok {
        let stderr: String = run.stderr.lines().take(30).collect::<Vec<_>>().join("\n");
        let stdout: String = run.stdout.lines().take(10).collect::<Vec<_>>().join("\n");
        let detail = if stderr.trim().is_empty() {
            stdout
        } else {
            stderr
        };
        return Err(format!(
            "python probe failed ({}):\n{}",
            run.failure().split(':').next().unwrap_or("error"),
            util::indent(&detail, 2)
        ));
    }
    Ok(run.stdout)
}

pub fn analyze(root: &Path) -> Result<Json, String> {
    // Absolute, and no cwd override: a relative root plus a cwd would make the
    // probe walk `<root>/<root>`.
    let root = root
        .canonicalize()
        .map_err(|error| format!("{}: {error}", root.display()))?;
    let output = probe(
        &[
            "analyze".to_string(),
            "--root".to_string(),
            root.display().to_string(),
        ],
        None,
    )?;
    Json::parse(output.trim())
}

pub fn inventory(path: &Path, module: &str, package_root: Option<&Path>) -> Result<Unit, String> {
    let mut args = vec![
        "inventory".to_string(),
        "--path".to_string(),
        path.display().to_string(),
        "--module".to_string(),
        module.to_string(),
        "--unit".to_string(),
        module.to_string(),
    ];
    if let Some(root) = package_root {
        args.push("--package-root".to_string());
        args.push(root.display().to_string());
    }
    let output = probe(&args, None)?;
    let doc = Json::parse(output.trim())?;
    Unit::parse(&doc)
}

pub fn record(
    project: &Project,
    cases: usize,
    seed: u64,
    inputs: Option<&Path>,
) -> Result<usize, String> {
    let out = project.dir.join("cases.jsonl");
    let mut args = vec![
        "record".to_string(),
        "--path".to_string(),
        project.source_file.display().to_string(),
        "--module".to_string(),
        project.module.clone(),
        "--out".to_string(),
        out.display().to_string(),
        "--cases".to_string(),
        cases.to_string(),
        "--seed".to_string(),
        seed.to_string(),
    ];
    if let Some(root) = &project.package_root {
        args.push("--package-root".to_string());
        args.push(root.display().to_string());
    }
    if let Some(inputs) = inputs {
        args.push("--inputs".to_string());
        args.push(inputs.display().to_string());
    }
    let output = probe(&args, Some(&project.dir))?;
    let doc = Json::parse(output.trim())?;
    Ok(doc.get("cases").and_then(Json::as_i64).unwrap_or(0) as usize)
}

pub struct FromTests {
    pub command: Vec<String>,
    pub calls: usize,
    pub added: usize,
    pub coverage: Option<(usize, usize)>,
    pub suite_ok: bool,
    pub suite_error: String,
}

/// Runs the original's own test suite with a hook that records every call it makes
/// to the unit. The hook is injected through PYTHONPATH (`sitecustomize.py`), so it
/// works for pytest, unittest and plain scripts without touching the project.
pub fn record_from_tests(project: &Project, command: &[String]) -> Result<FromTests, String> {
    if command.is_empty() {
        return Err(
            "--from-tests needs the test command after `--`, e.g. `record . --from-tests -- pytest -q`"
                .into(),
        );
    }
    let scratch = std::env::temp_dir()
        .join("cpp-anything")
        .join(format!("from-tests-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&scratch);
    std::fs::create_dir_all(&scratch).map_err(|error| format!("{}: {error}", scratch.display()))?;
    let calls_path = scratch.join("calls.jsonl");
    let coverage_path = scratch.join("coverage.json");
    let hook = SITE_HOOK
        .replace("{{MODULE}}", &project.module)
        .replace("{{FILE}}", &project.source_file.display().to_string())
        .replace("{{OUT}}", &calls_path.display().to_string())
        .replace("{{COVERAGE}}", &coverage_path.display().to_string());
    util::write(&scratch.join("sitecustomize.py"), &hook)?;

    let existing_path = std::env::var("PYTHONPATH").unwrap_or_default();
    let pythonpath = if existing_path.is_empty() {
        scratch.display().to_string()
    } else {
        format!(
            "{}{}{}",
            scratch.display(),
            if cfg!(windows) { ";" } else { ":" },
            existing_path
        )
    };
    let (program, rest) = command.split_first().ok_or("empty test command")?;
    let cwd = project
        .package_root
        .clone()
        .or_else(|| project.source_file.parent().map(Path::to_path_buf));
    let run = util::run_with_env(
        program,
        rest,
        cwd.as_deref(),
        &[("PYTHONPATH".to_string(), pythonpath)],
    );

    let mut calls = 0usize;
    let mut added = 0usize;
    let mut seen: std::collections::BTreeSet<String> = std::collections::BTreeSet::new();
    let cases_path = project.dir.join("cases.jsonl");
    if let Ok(existing) = util::read(&cases_path) {
        for line in existing.lines() {
            if let Ok(doc) = Json::parse(line) {
                let key = format!(
                    "{}\x01{}",
                    doc.get("fn").and_then(Json::as_str).unwrap_or(""),
                    doc.get("args").cloned().unwrap_or(Json::Null).canonical()
                );
                seen.insert(key);
            }
        }
    }
    let mut appended = String::new();
    if calls_path.exists() {
        for line in util::read(&calls_path)?.lines() {
            let Ok(doc) = Json::parse(line) else { continue };
            let Some(function) = doc.get("fn").and_then(Json::as_str) else {
                continue;
            };
            let args = doc.get("args").cloned().unwrap_or(Json::Array(Vec::new()));
            let key = format!("{function}\x01{}", args.canonical());
            calls += 1;
            if !seen.insert(key) {
                continue;
            }
            added += 1;
            let entry = Json::obj([
                ("id", Json::str(format!("{function}:suite-{added}"))),
                ("fn", Json::str(function)),
                ("args", args),
                ("expect", doc.get("expect").cloned().unwrap_or(Json::Null)),
            ]);
            appended.push_str(&entry.to_string_compact());
            appended.push('\n');
        }
    }
    if !appended.is_empty() {
        let mut text = util::read(&cases_path).unwrap_or_default();
        if !text.is_empty() && !text.ends_with('\n') {
            text.push('\n');
        }
        text.push_str(&appended);
        util::write(&cases_path, &text)?;
    }

    let coverage = util::read(&coverage_path).ok().and_then(|text| {
        Json::parse(&text).ok().and_then(|doc| {
            doc.get("reached")
                .and_then(Json::as_array)
                .map(|reached| (reached.len(), source_lines(&project.source_file)))
        })
    });

    let recorded_from = Json::obj([
        ("schema", Json::str("cpp-anything/recorded-from@1")),
        (
            "source_hash",
            Json::str(util::hash64(
                &util::read(&project.source_file).unwrap_or_default(),
            )),
        ),
        (
            "from_tests",
            Json::obj([
                (
                    "command",
                    Json::Array(command.iter().map(|part| Json::str(part.clone())).collect()),
                ),
                ("calls", Json::Int(calls as i64)),
                ("cases", Json::Int(added as i64)),
                ("at", Json::str(util::now_iso8601())),
            ]),
        ),
        (
            "coverage",
            coverage
                .map(|(reached, lines)| {
                    Json::obj([
                        ("reached", Json::Int(reached as i64)),
                        ("lines", Json::Int(lines as i64)),
                    ])
                })
                .unwrap_or(Json::Null),
        ),
    ]);
    util::write(
        &project
            .dir
            .join(crate::project::META_DIR)
            .join("recorded-from.json"),
        &recorded_from.to_string_pretty(),
    )?;

    Ok(FromTests {
        command: command.to_vec(),
        calls,
        added,
        coverage,
        suite_ok: run.ok,
        suite_error: if run.ok { String::new() } else { run.failure() },
    })
}

/// Non-blank, non-comment lines: what "coverage" is a fraction of.
fn source_lines(path: &Path) -> usize {
    util::read(path)
        .unwrap_or_default()
        .lines()
        .filter(|line| {
            let trimmed = line.trim();
            !trimmed.is_empty() && !trimmed.starts_with('#')
        })
        .count()
}

pub fn bench(project: &Project, repeat: usize) -> Result<f64, String> {
    let cases = project.dir.join("cases.jsonl");
    let mut args = vec![
        "bench".to_string(),
        "--path".to_string(),
        project.source_file.display().to_string(),
        "--module".to_string(),
        project.module.clone(),
        "--cases".to_string(),
        cases.display().to_string(),
        "--repeat".to_string(),
        repeat.to_string(),
    ];
    if let Some(root) = &project.package_root {
        args.push("--package-root".to_string());
        args.push(root.display().to_string());
    }
    let output = probe(&args, Some(&project.dir))?;
    let doc = Json::parse(output.trim())?;
    doc.get("ns_per_call")
        .and_then(Json::as_f64)
        .ok_or_else(|| format!("probe bench returned no ns_per_call: {output}"))
}
