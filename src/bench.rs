//! `bench`: the original's per-call cost against the port's, on the same cases.

use std::path::Path;

use crate::json::Json;
use crate::project::Project;
use crate::util;

pub struct BenchReport {
    pub python_ns: f64,
    pub cpp_ns: f64,
    pub calls: usize,
    pub repeat: usize,
}

impl BenchReport {
    pub fn speedup(&self) -> f64 {
        if self.cpp_ns > 0.0 {
            self.python_ns / self.cpp_ns
        } else {
            f64::INFINITY
        }
    }

    pub fn verdict(&self) -> &'static str {
        let speedup = self.speedup();
        if speedup >= 1.05 {
            "faster"
        } else if speedup <= 0.95 {
            "slower"
        } else {
            "about the same"
        }
    }
}

pub fn run(project: &Project, driver: &Path, repeat: usize) -> Result<BenchReport, String> {
    let cases = project.dir.join("cases.jsonl");
    if !cases.exists() {
        return Err(format!(
            "{}: no recorded cases (run `cpp-anything record`)",
            cases.display()
        ));
    }
    let repeat = repeat.max(1);

    // C++: the driver keeps the decoded arguments and times only the calls.
    let args = vec![
        "--cases".to_string(),
        cases.display().to_string(),
        "--bench".to_string(),
        repeat.to_string(),
    ];
    let run = util::run(&driver.display().to_string(), &args, Some(&project.dir));
    if !run.ok {
        return Err(format!("the C++ driver's bench failed: {}", run.failure()));
    }
    let cpp =
        Json::parse(run.stdout.trim()).map_err(|error| format!("driver bench output: {error}"))?;
    let cpp_ns = cpp.get("ns_per_call").and_then(Json::as_f64).unwrap_or(0.0);

    // Python: the probe loads the module once and times the same calls.
    let python_ns = crate::python::bench(project, repeat)?;

    Ok(BenchReport {
        python_ns,
        cpp_ns,
        calls: cpp.get("calls").and_then(Json::as_i64).unwrap_or(0) as usize,
        repeat,
    })
}

pub fn render(report: &BenchReport, project: &Project) -> String {
    let mut out = String::new();
    let cases = report.calls.checked_div(report.repeat).unwrap_or(0);
    out.push_str(&format!(
        "cpp-anything bench: {} — {} calls over {} recorded cases, {}x repeated\n\n",
        project.module, report.calls, cases, report.repeat
    ));
    out.push_str(&format!(
        "  {:<22} {:>12} ns/call\n",
        "original (python)",
        format_ns(report.python_ns)
    ));
    out.push_str(&format!(
        "  {:<22} {:>12} ns/call\n",
        "port (C++)",
        format_ns(report.cpp_ns)
    ));
    out.push_str(&format!(
        "\n  speedup: {:.2}x — the port is {}\n",
        report.speedup(),
        report.verdict()
    ));
    if report.verdict() == "slower" {
        out.push_str(
            "\n  One call per small item usually costs more than the work: batch the loop\n  (call once over a list) before concluding the port is not worth it.\n",
        );
    }
    out
}

fn format_ns(nanos: f64) -> String {
    if nanos >= 1_000_000.0 {
        format!("{:.2} ms", nanos / 1_000_000.0)
    } else if nanos >= 1_000.0 {
        format!("{:.2} µs", nanos / 1_000.0)
    } else {
        format!("{:.1}", nanos)
    }
}

pub fn to_json(report: &BenchReport, project: &Project) -> Json {
    Json::obj([
        ("schema", Json::str("cpp-anything/bench@1")),
        ("unit", Json::str(project.unit.clone())),
        ("calls", Json::Int(report.calls as i64)),
        ("repeat", Json::Int(report.repeat as i64)),
        ("original_ns_per_call", Json::Real(report.python_ns)),
        ("port_ns_per_call", Json::Real(report.cpp_ns)),
        ("speedup", Json::Real(report.speedup())),
        ("verdict", Json::str(report.verdict())),
    ])
}
