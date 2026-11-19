//! `shadow`: the port running beside the original on real traffic.
//!
//! The generated `shadow/<unit>.shadow.py` calls the original first and returns its
//! result; the C++ port runs second, and every disagreement is appended to a log.
//! Nothing the port does can reach a caller, so shipping shadow code cannot change
//! behaviour — that is the property that makes it safe to run in production.

use std::collections::BTreeMap;

use crate::json::Json;
use crate::project::Project;
use crate::util;

pub struct Mismatch {
    pub function: String,
    pub args: Json,
    pub expected: Json,
    pub got: Json,
    pub reason: String,
    pub at: String,
}

pub struct Report {
    pub lines: usize,
    pub mismatches: Vec<Mismatch>,
    pub by_function: Vec<(String, usize)>,
}

impl Report {
    pub fn clean(&self) -> bool {
        self.mismatches.is_empty()
    }
}

pub fn log_path(project: &Project) -> std::path::PathBuf {
    project.dir.join("shadow").join(format!(
        "{}.shadow.jsonl",
        crate::codegen::namespace_of(project)
    ))
}

pub fn read_report(project: &Project) -> Result<Report, String> {
    let path = log_path(project);
    if !path.exists() {
        return Ok(Report {
            lines: 0,
            mismatches: Vec::new(),
            by_function: Vec::new(),
        });
    }
    let text = util::read(&path)?;
    let mut mismatches = Vec::new();
    let mut counts: BTreeMap<String, usize> = BTreeMap::new();
    let mut lines = 0;
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        lines += 1;
        let Ok(doc) = Json::parse(line) else { continue };
        let function = doc
            .get("fn")
            .and_then(Json::as_str)
            .unwrap_or("?")
            .to_string();
        *counts.entry(function.clone()).or_default() += 1;
        mismatches.push(Mismatch {
            function,
            args: doc.get("args").cloned().unwrap_or(Json::Null),
            expected: doc.get("expected").cloned().unwrap_or(Json::Null),
            got: doc.get("got").cloned().unwrap_or(Json::Null),
            reason: doc
                .get("reason")
                .and_then(Json::as_str)
                .unwrap_or("")
                .to_string(),
            at: doc
                .get("at")
                .and_then(Json::as_str)
                .unwrap_or("")
                .to_string(),
        });
    }
    Ok(Report {
        lines,
        mismatches,
        by_function: counts.into_iter().collect(),
    })
}

pub fn render(report: &Report, project: &Project) -> String {
    let mut out = String::new();
    if report.lines == 0 {
        out.push_str(&format!(
            "cpp-anything shadow: {} — no mismatches logged yet\n\n\
             import the generated module where the original was imported and run the app:\n  \
             sys.path.insert(0, \"{}\")\n  import {}_shadow as ...\n",
            project.module,
            project.dir.join("shadow").display(),
            crate::codegen::namespace_of(project)
        ));
        return out;
    }
    out.push_str(&format!(
        "cpp-anything shadow: {} — {} mismatch(es) over {} call(s)\n\n",
        project.module,
        report.mismatches.len(),
        report.lines
    ));
    let width = report
        .by_function
        .iter()
        .map(|(name, _)| name.len())
        .max()
        .unwrap_or(8);
    for (name, count) in &report.by_function {
        out.push_str(&format!("  {:<width$}  {}\n", name, count, width = width));
    }
    out.push_str("\nfirst mismatches:\n");
    for mismatch in report.mismatches.iter().take(5) {
        out.push_str(&format!(
            "\n  {} {}({})\n    {}\n    expected: {}\n    got:      {}\n",
            mismatch.at,
            mismatch.function,
            mismatch
                .args
                .as_array()
                .map(|args| args
                    .iter()
                    .map(Json::to_string_compact)
                    .collect::<Vec<_>>()
                    .join(", "))
                .unwrap_or_default(),
            mismatch.reason,
            util::truncate_text(&mismatch.expected.to_string_compact(), 140),
            util::truncate_text(&mismatch.got.to_string_compact(), 140),
        ));
    }
    out.push_str(
        "\nshadow traffic never replaces the original's result: fix the port, keep the traffic on, \
         then `gate`.\n",
    );
    out
}

pub fn to_json(report: &Report, project: &Project) -> Json {
    Json::obj([
        ("schema", Json::str("cpp-anything/shadow@1")),
        ("unit", Json::str(project.unit.clone())),
        ("calls", Json::Int(report.lines as i64)),
        ("mismatches", Json::Int(report.mismatches.len() as i64)),
        (
            "by_function",
            Json::Array(
                report
                    .by_function
                    .iter()
                    .map(|(name, count)| {
                        Json::obj([
                            ("name", Json::str(name.clone())),
                            ("count", Json::Int(*count as i64)),
                        ])
                    })
                    .collect(),
            ),
        ),
        (
            "first",
            Json::Array(
                report
                    .mismatches
                    .iter()
                    .take(20)
                    .map(|mismatch| {
                        Json::obj([
                            ("at", Json::str(mismatch.at.clone())),
                            ("fn", Json::str(mismatch.function.clone())),
                            ("args", mismatch.args.clone()),
                            ("expected", mismatch.expected.clone()),
                            ("got", mismatch.got.clone()),
                            ("reason", Json::str(mismatch.reason.clone())),
                        ])
                    })
                    .collect(),
            ),
        ),
    ])
}
