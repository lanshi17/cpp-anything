//! `diff`: replay every recorded case through the C++ port and compare.
//!
//! Errors are compared by exception type, not message: messages are rarely a
//! contract, and a port would fail on wording forever. `--strict-errors` opts in.

use std::collections::BTreeMap;
use std::path::Path;

use crate::json::Json;
use crate::project::Project;
use crate::util;

#[derive(Clone, Debug)]
pub struct Mismatch {
    pub id: String,
    pub function: String,
    pub args: Json,
    pub expected: Json,
    pub got: Json,
    pub reason: String,
}

#[derive(Clone, Debug, Default)]
pub struct DiffReport {
    pub total: usize,
    pub matched: usize,
    pub skipped: usize,
    pub missing: Vec<String>,
    pub mismatches: Vec<Mismatch>,
    pub per_function: Vec<(String, usize, usize)>,
}

impl DiffReport {
    pub fn passed(&self) -> bool {
        self.total > 0 && self.matched + self.skipped == self.total && self.missing.is_empty()
    }
}

pub struct Options {
    pub tolerance: f64,
    pub strict_errors: bool,
    pub only: Option<String>,
    pub max_report: usize,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            tolerance: 0.0,
            strict_errors: false,
            only: None,
            max_report: 8,
        }
    }
}

pub struct Case {
    pub id: String,
    pub function: String,
    pub args: Json,
    pub expect: Json,
}

pub fn load_cases(path: &Path) -> Result<Vec<Case>, String> {
    let text = util::read(path).map_err(|error| {
        format!(
            "{}: {error} (run `cpp-anything record` first)",
            path.display()
        )
    })?;
    let mut cases = Vec::new();
    for (number, line) in text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)
            .map_err(|error| format!("{}:{}: {error}", path.display(), number + 1))?;
        let id = doc
            .get("id")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let function = doc
            .get("fn")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let args = doc.get("args").cloned().unwrap_or(Json::Array(Vec::new()));
        let expect = doc.get("expect").cloned().unwrap_or(Json::Null);
        if id.is_empty() || function.is_empty() {
            return Err(format!(
                "{}:{}: case without id or fn",
                path.display(),
                number + 1
            ));
        }
        cases.push(Case {
            id,
            function,
            args,
            expect,
        });
    }
    if cases.is_empty() {
        return Err(format!("{}: no cases", path.display()));
    }
    Ok(cases)
}

pub fn run(project: &Project, driver: &Path, options: &Options) -> Result<DiffReport, String> {
    let cases_path = project.dir.join("cases.jsonl");
    let cases = load_cases(&cases_path)?;
    let results_path = project.dir.join("build").join("results.jsonl");
    let mut args = vec![
        "--cases".to_string(),
        cases_path.display().to_string(),
        "--out".to_string(),
        results_path.display().to_string(),
        "--quiet".to_string(),
    ];
    if let Some(only) = &options.only {
        args.push("--only".to_string());
        args.push(only.clone());
    }
    let run = util::run(&driver.display().to_string(), &args, Some(&project.dir));
    if !run.ok {
        return Err(format!(
            "the driver failed: {}\n{}",
            run.failure(),
            util::indent(&run.stderr, 2)
        ));
    }

    let text = util::read(&results_path)
        .map_err(|error| format!("{}: {error}", results_path.display()))?;
    let mut results: BTreeMap<String, Json> = BTreeMap::new();
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line).map_err(|error| format!("results: {error}"))?;
        if let Some(id) = doc.get("id").and_then(Json::as_str) {
            if let Some(result) = doc.get("result") {
                results.insert(id.to_string(), result.clone());
            }
        }
    }

    let mut report = DiffReport::default();
    let mut counts: BTreeMap<String, (usize, usize)> = BTreeMap::new();
    for case in &cases {
        if options
            .only
            .as_deref()
            .is_some_and(|only| only != case.function)
        {
            continue;
        }
        report.total += 1;
        let entry = counts.entry(case.function.clone()).or_default();
        entry.0 += 1;
        if case.expect.is_null() {
            report.skipped += 1;
            continue;
        }
        let Some(got) = results.get(&case.id) else {
            report.missing.push(case.id.clone());
            continue;
        };
        match compare(&case.expect, got, options) {
            Ok(()) => {
                report.matched += 1;
                entry.1 += 1;
            }
            Err(reason) => report.mismatches.push(Mismatch {
                id: case.id.clone(),
                function: case.function.clone(),
                args: case.args.clone(),
                expected: case.expect.clone(),
                got: got.clone(),
                reason,
            }),
        }
    }
    report.per_function = counts
        .into_iter()
        .map(|(name, (total, passed))| (name, total, passed))
        .collect();
    Ok(report)
}

pub fn compare(expected: &Json, got: &Json, options: &Options) -> Result<(), String> {
    let expected_err = expected.get("err");
    let got_err = got.get("err");
    match (expected_err, got_err) {
        (Some(expected), Some(actual)) => {
            let expected_type = expected.get("type").and_then(Json::as_str).unwrap_or("?");
            let actual_type = actual.get("type").and_then(Json::as_str).unwrap_or("?");
            if expected_type != actual_type {
                return Err(format!(
                    "error type: the original raises {expected_type}, the port raises {actual_type}"
                ));
            }
            if options.strict_errors {
                let expected_msg = expected.get("msg").and_then(Json::as_str).unwrap_or("");
                let actual_msg = actual.get("msg").and_then(Json::as_str).unwrap_or("");
                if expected_msg != actual_msg {
                    return Err(format!(
                        "error message: expected {:?}, got {:?}",
                        util::truncate_text(expected_msg, 80),
                        util::truncate_text(actual_msg, 80)
                    ));
                }
            }
            Ok(())
        }
        (Some(expected), None) => Err(format!(
            "the original raises {}, the port returns {}",
            expected
                .get("type")
                .and_then(Json::as_str)
                .unwrap_or("an error"),
            util::truncate_text(
                &got.get("ok")
                    .map_or_else(|| got.to_string(), |value| value.to_string()),
                60
            )
        )),
        (None, Some(actual)) => Err(format!(
            "the port raises {}, the original returns a value",
            actual
                .get("type")
                .and_then(Json::as_str)
                .unwrap_or("an error")
        )),
        (None, None) => {
            let expected_value = expected.get("ok").unwrap_or(expected);
            let actual_value = got.get("ok").unwrap_or(got);
            if expected_value.equal(actual_value, options.tolerance) {
                Ok(())
            } else {
                Err(describe(expected_value, actual_value))
            }
        }
    }
}

fn describe(expected: &Json, got: &Json) -> String {
    let kind = |value: &Json| match value {
        Json::Null => "null",
        Json::Bool(_) => "bool",
        Json::Int(_) => "int",
        Json::Real(_) => "float",
        Json::Str(_) => "string",
        Json::Array(_) => "list",
        Json::Object(_) => "object",
    };
    if kind(expected) != kind(got) {
        return format!(
            "type: expected a {}, the port returns a {}",
            kind(expected),
            kind(got)
        );
    }
    match (expected, got) {
        (Json::Str(a), Json::Str(b)) => format!(
            "text: expected {:?}, got {:?}",
            util::truncate_text(a, 70),
            util::truncate_text(b, 70)
        ),
        (Json::Array(a), Json::Array(b)) if a.len() != b.len() => {
            format!("length: expected {} items, got {}", a.len(), b.len())
        }
        (Json::Real(a), Json::Real(b)) if a.to_bits() != b.to_bits() && (a == b) => {
            format!("-0.0 vs 0.0: expected {a:?}, got {b:?} (bit-exact floats; use --tolerance)")
        }
        _ => format!(
            "value: expected {}, got {}",
            util::truncate_text(&expected.to_string_compact(), 90),
            util::truncate_text(&got.to_string_compact(), 90)
        ),
    }
}

pub fn render(report: &DiffReport, project: &Project, options: &Options) -> String {
    let mut out = String::new();
    let verdict = if report.passed() { "match" } else { "MISMATCH" };
    out.push_str(&format!(
        "cpp-anything diff: {} — {}/{} cases {verdict}",
        project.module, report.matched, report.total
    ));
    if report.skipped > 0 {
        out.push_str(&format!(" ({} skipped: nothing recorded)", report.skipped));
    }
    out.push('\n');
    let width = report
        .per_function
        .iter()
        .map(|(name, _, _)| name.chars().count())
        .max()
        .unwrap_or(8)
        .max(8);
    out.push_str(&format!(
        "\n  {:<width$}  cases  pass\n",
        "function",
        width = width
    ));
    for (name, total, passed) in &report.per_function {
        out.push_str(&format!(
            "  {:<width$}  {:>5}  {:>4}\n",
            name,
            total,
            passed,
            width = width
        ));
    }
    if !report.missing.is_empty() {
        out.push_str(&format!(
            "\n  {} cases produced no result\n",
            report.missing.len()
        ));
    }
    if !report.mismatches.is_empty() {
        out.push_str(&format!(
            "\n{} mismatches (first {}):\n",
            report.mismatches.len(),
            options.max_report.min(report.mismatches.len())
        ));
        for mismatch in report.mismatches.iter().take(options.max_report) {
            out.push_str(&format!(
                "\n  {}  {}({})\n    {}\n    expected: {}\n    got:      {}\n",
                mismatch.id,
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
        if report.mismatches.len() > options.max_report {
            out.push_str(&format!(
                "\n  … {} more (raise --max-report to see them)\n",
                report.mismatches.len() - options.max_report
            ));
        }
    }
    if report.passed() {
        out.push_str("\nevery recorded case matches: the port behaves like the original on what was recorded.\n");
    }
    out
}

pub fn to_json(report: &DiffReport, project: &Project) -> Json {
    let mismatches = report
        .mismatches
        .iter()
        .map(|mismatch| {
            Json::obj([
                ("id", Json::str(mismatch.id.clone())),
                ("fn", Json::str(mismatch.function.clone())),
                ("args", mismatch.args.clone()),
                ("expected", mismatch.expected.clone()),
                ("got", mismatch.got.clone()),
                ("reason", Json::str(mismatch.reason.clone())),
            ])
        })
        .collect::<Vec<_>>();
    Json::obj([
        ("schema", Json::str("cpp-anything/diff@1")),
        ("unit", Json::str(project.unit.clone())),
        ("module", Json::str(project.module.clone())),
        ("cases", Json::Int(report.total as i64)),
        ("matched", Json::Int(report.matched as i64)),
        ("skipped", Json::Int(report.skipped as i64)),
        ("passed", Json::Bool(report.passed())),
        (
            "missing",
            Json::Array(
                report
                    .missing
                    .iter()
                    .map(|id| Json::str(id.clone()))
                    .collect(),
            ),
        ),
        (
            "functions",
            Json::Array(
                report
                    .per_function
                    .iter()
                    .map(|(name, total, passed)| {
                        Json::obj([
                            ("name", Json::str(name.clone())),
                            ("cases", Json::Int(*total as i64)),
                            ("passed", Json::Int(*passed as i64)),
                        ])
                    })
                    .collect(),
            ),
        ),
        ("mismatches", Json::Array(mismatches)),
    ])
}
