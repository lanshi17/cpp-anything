//! `profile`: run a real workload under the language's profiler and say what is
//! worth moving, with the advice an agent would otherwise guess.

use std::path::Path;

use crate::json::Json;
use crate::util;

pub struct Entry {
    pub harness: bool,
    pub function: String,
    pub file: String,
    pub line: i64,
    pub calls: i64,
    pub self_seconds: f64,
    pub cum_seconds: f64,
    pub per_call_ns: f64,
    pub share: f64,
    pub callers: Vec<String>,
    pub advice: String,
    pub external: bool,
}

pub struct Report {
    pub workload: Vec<String>,
    pub total_seconds: f64,
    pub entries: Vec<Entry>,
}

/// Runs the workload through the probe's profiler and turns the raw stats into a
/// ranked report. `python3 bench.py` and `bench.py` both work; the probe strips
/// the interpreter so cProfile runs the script itself.
pub fn run(_root: &Path, command: &[String], top: usize) -> Result<Report, String> {
    if command.is_empty() {
        return Err(
            "profile needs the workload after `--`, e.g. `cpp-anything profile . -- python3 bench.py`"
                .into(),
        );
    }
    let mut args = vec!["profile".to_string()];
    args.extend(command.iter().cloned());
    // The workload's paths are relative to the caller's cwd, like every other
    // command the user would type; `root` is only for detection and the report.
    let output = crate::python::probe_command(&args)?;
    let doc = Json::parse(output.trim())?;
    if let Some(error) = doc.get("error").and_then(Json::as_str) {
        let stderr = doc.get("stderr").and_then(Json::as_str).unwrap_or("");
        return Err(format!("{error}\n{}", util::indent(stderr, 2)));
    }
    Ok(rank(&doc, top))
}

fn rank(doc: &Json, top: usize) -> Report {
    let workload: Vec<String> = doc
        .get("workload")
        .and_then(Json::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default();
    let mut entries = Vec::new();
    let mut total_seconds = 0.0;
    let script = script_of(&workload);
    for item in doc
        .get("functions")
        .and_then(Json::as_array)
        .unwrap_or_default()
    {
        let file = item
            .get("file")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let function = item
            .get("function")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let self_seconds = item
            .get("self_seconds")
            .and_then(Json::as_f64)
            .unwrap_or(0.0);
        if self_seconds <= 0.0 || function.is_empty() {
            continue;
        }
        // Frozen modules and C builtins have no file to port, and the string pstats
        // builds for them is unreadable: drop them.
        if file.starts_with('<') {
            continue;
        }
        total_seconds += self_seconds;
        let calls = item.get("calls").and_then(Json::as_i64).unwrap_or(0);
        let external = file.contains("site-packages")
            || file.contains("/lib/python")
            || file.starts_with("<frozen");
        let harness = file == script;
        entries.push(Entry {
            harness,
            function,
            file: file.clone(),
            line: item.get("line").and_then(Json::as_i64).unwrap_or(0),
            calls,
            self_seconds,
            cum_seconds: item
                .get("cum_seconds")
                .and_then(Json::as_f64)
                .unwrap_or(0.0),
            per_call_ns: item
                .get("per_call_seconds")
                .and_then(Json::as_f64)
                .unwrap_or(0.0)
                * 1e9,
            share: 0.0,
            callers: item
                .get("callers")
                .and_then(Json::as_array)
                .map(|callers| {
                    callers
                        .iter()
                        .filter_map(|caller| caller.get("function").and_then(Json::as_str))
                        .filter(|name| *name != "<module>")
                        .map(str::to_string)
                        .collect()
                })
                .unwrap_or_default(),
            advice: String::new(),
            external,
        });
    }
    for entry in &mut entries {
        entry.share = if total_seconds > 0.0 {
            entry.self_seconds / total_seconds
        } else {
            0.0
        };
        entry.advice = advice(entry);
    }
    entries.truncate(top.max(1));
    Report {
        workload,
        total_seconds,
        entries,
    }
}

/// The rules an experienced porter applies, written down:
/// a cheap leaf called in a loop is not worth one call per item; batching is.
fn advice(entry: &Entry) -> String {
    if entry.harness {
        return "the workload harness: not part of the port; profile what it calls".into();
    }
    if entry.external {
        return "third-party or stdlib code: not yours to port; port your callers instead".into();
    }
    if entry.calls >= 500 && entry.per_call_ns < 2_000.0 {
        return format!(
            "called {} times at {:.0} ns each: one call per item costs more than the work, port it \
             with its callers or batch it (`bridge <repo> <unit> --batch {}:<param>`)",
            entry.calls, entry.per_call_ns, entry.function
        );
    }
    if entry.calls >= 500 {
        return format!(
            "hot leaf ({} calls, {:.0} ns each): batch it (`--batch {}:<param>`) so the loop stays on \
             the C++ side",
            entry.calls, entry.per_call_ns, entry.function
        );
    }
    if entry.share >= 0.10 {
        return format!(
            "top cost ({:.0}% of the workload): port this first, then `diff` and `bench`",
            entry.share * 100.0
        );
    }
    "port with its callers; alone it is unlikely to pay for the crossing".into()
}

pub fn render(report: &Report) -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "cpp-anything profile: {} — {:.2} s of self time across {} function(s)\n",
        if report.workload.is_empty() {
            "workload".to_string()
        } else {
            report.workload.join(" ")
        },
        report.total_seconds,
        report.entries.len()
    ));
    out.push_str("\n   self%     calls     per call   function (file:line)\n");
    for entry in &report.entries {
        out.push_str(&format!(
            "  {:>5.1}%  {:>9}  {:>10}   {}.{}  {}:{}\n",
            entry.share * 100.0,
            entry.calls,
            format_ns(entry.per_call_ns),
            module_of(&entry.file),
            entry.function,
            util::short_path(Path::new(&entry.file)),
            entry.line
        ));
        if entry.external || entry.harness {
            out.push_str(&format!("            {}\n", entry.advice));
            continue;
        }
        if !entry.callers.is_empty() {
            out.push_str(&format!(
                "            called from: {}\n",
                entry
                    .callers
                    .iter()
                    .take(3)
                    .cloned()
                    .collect::<Vec<_>>()
                    .join(", ")
            ));
        }
        out.push_str(&format!("            {}\n", entry.advice));
    }
    if report.entries.iter().all(|entry| entry.external) {
        out.push_str(
            "\nEverything hot is in a dependency: profile the code that calls it, or the work is \
             not in the functions you own.\n",
        );
    }
    out.push_str(
        "\nNext: `cpp-anything analyze .`, then `cpp-anything bridge . <unit>` for the unit the hot \
         function lives in.\n",
    );
    out
}

fn script_of(workload: &[String]) -> String {
    workload
        .iter()
        .find(|part| part.ends_with(".py"))
        .cloned()
        .unwrap_or_default()
}

fn module_of(file: &str) -> String {
    Path::new(file)
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn format_ns(nanos: f64) -> String {
    if nanos >= 1_000_000.0 {
        format!("{:.2} ms", nanos / 1_000_000.0)
    } else if nanos >= 1_000.0 {
        format!("{:.2} µs", nanos / 1_000.0)
    } else {
        format!("{nanos:.0} ns")
    }
}

pub fn to_json(report: &Report, root: &Path) -> Json {
    Json::obj([
        ("schema", Json::str("cpp-anything/profile@1")),
        ("root", Json::str(root.display().to_string())),
        (
            "workload",
            Json::Array(
                report
                    .workload
                    .iter()
                    .map(|part| Json::str(part.clone()))
                    .collect(),
            ),
        ),
        ("self_seconds", Json::Real(report.total_seconds)),
        (
            "functions",
            Json::Array(
                report
                    .entries
                    .iter()
                    .map(|entry| {
                        Json::obj([
                            ("function", Json::str(entry.function.clone())),
                            ("file", Json::str(entry.file.clone())),
                            ("line", Json::Int(entry.line)),
                            ("calls", Json::Int(entry.calls)),
                            ("self_seconds", Json::Real(entry.self_seconds)),
                            ("cum_seconds", Json::Real(entry.cum_seconds)),
                            ("per_call_ns", Json::Real(entry.per_call_ns)),
                            ("share", Json::Real(entry.share)),
                            (
                                "callers",
                                Json::Array(
                                    entry
                                        .callers
                                        .iter()
                                        .map(|name| Json::str(name.clone()))
                                        .collect(),
                                ),
                            ),
                            ("external", Json::Bool(entry.external)),
                            ("advice", Json::str(entry.advice.clone())),
                        ])
                    })
                    .collect(),
            ),
        ),
    ])
}
