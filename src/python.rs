//! Running the Python probe: analysis, recording, and benchmarking the original.

use std::path::{Path, PathBuf};

use crate::inventory::Unit;
use crate::json::Json;
use crate::project::Project;
use crate::util;

const PROBE: &str = include_str!("probe.py");

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

fn probe(args: &[String], cwd: Option<&Path>) -> Result<String, String> {
    let script = probe_path()?;
    let mut argv = vec![script.display().to_string()];
    argv.extend(args.iter().cloned());
    let run = util::run(&interpreter(), &argv, cwd);
    if !run.ok {
        let stderr: String = run.stderr.lines().take(30).collect::<Vec<_>>().join("\n");
        return Err(format!(
            "python probe failed ({})\n{}",
            run.failure().split(':').next().unwrap_or("error"),
            util::indent(&stderr, 2)
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
