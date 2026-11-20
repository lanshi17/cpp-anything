//! `gate`: everything a finished port has to satisfy, in one exit code.

use crate::json::Json;
use crate::project::Project;
use crate::util;

pub struct Step {
    pub name: String,
    pub ok: bool,
    pub detail: String,
}

pub struct GateReport {
    pub steps: Vec<Step>,
}

impl GateReport {
    pub fn passed(&self) -> bool {
        self.steps.iter().all(|step| step.ok)
    }
}

pub struct Options {
    pub clean: bool,
    pub cxx: Option<String>,
    pub jobs: usize,
    pub relevance: Option<(usize, usize)>, // (repeat, minimum records) for --bench
}

pub fn run(project: &Project, options: &Options) -> Result<GateReport, String> {
    let mut steps = Vec::new();

    // 1. The target toolchain must exist in the matrix, and the standard must be one
    //    cpp-anything generates for.
    let matrix = crate::matrix::Matrix::get();
    let std_number: u32 = project
        .target
        .std
        .trim_start_matches("c++")
        .parse()
        .unwrap_or(0);
    let known = matrix.resolve_toolchain(&project.target.toolchain);
    let expected = matrix.macro_available(
        "__cpp_lib_expected",
        &project.target.toolchain,
        &project.target.version,
        &project.target.std,
    );
    let (ok, detail) = match (known, std_number >= 20) {
        (None, _) => (
            false,
            format!(
                "unknown toolchain {:?}; `cpp-anything matrix` lists the ones it knows",
                project.target.toolchain
            ),
        ),
        (Some(_toolchain), false) => (
            false,
            format!(
                "{} is below C++20; cpp-anything generates C++20/23",
                project.target.std
            ),
        ),
        (Some(toolchain), true) => (
            true,
            match &expected {
                crate::matrix::MacroStatus::Available(min, _) => format!(
                    "{toolchain} {} with {}: std::expected since {min}",
                    project.target.version, project.target.std
                ),
                _ => format!(
                    "{toolchain} {} with {}: no std::expected, the port uses cppan::Result",
                    project.target.version, project.target.std
                ),
            },
        ),
    };
    steps.push(Step {
        name: "matrix".into(),
        ok,
        detail,
    });

    // 2. The port must be a port: no stub marker left.
    let port_path = project.dir.join("src").join("port.cpp");
    let port =
        util::read(&port_path).map_err(|error| format!("{}: {error}", port_path.display()))?;
    // Comments mention the marker; only code that still returns it counts.
    let code: String = port
        .lines()
        .map(|line| line.split("//").next().unwrap_or(""))
        .collect::<Vec<_>>()
        .join("\n");
    let stubs = code.matches("CPPAN_STUB").count();
    steps.push(Step {
        name: "no stubs".into(),
        ok: stubs == 0,
        detail: if stubs == 0 {
            "every function has a real body".into()
        } else {
            format!("{stubs} function(s) still return CPPAN_STUB")
        },
    });

    // 2b. The original fails somewhere: each such function needs a recorded failure,
    //     and the port must not turn that into a crash.
    let cases = crate::diff::load_cases(&project.dir.join("cases.jsonl")).unwrap_or_default();
    let failing: Vec<&crate::inventory::Function> = project
        .functions
        .iter()
        .filter(|function| function.raises)
        .collect();
    if failing.is_empty() {
        steps.push(Step {
            name: "error cases".into(),
            ok: true,
            detail: "the original fails on purpose nowhere".into(),
        });
    } else {
        let missing: Vec<String> = failing
            .iter()
            .filter(|function| {
                !cases
                    .iter()
                    .any(|case| case.function == function.name && case.expect.get("err").is_some())
            })
            .map(|function| function.name.clone())
            .collect();
        steps.push(Step {
            name: "error cases".into(),
            ok: missing.is_empty(),
            detail: if missing.is_empty() {
                format!(
                    "each of the {} function(s) that fail in the original has a recorded failing case",
                    failing.len()
                )
            } else {
                format!(
                    "no recorded case reaches these error paths: {} (add inputs that fail: \
                     `record --from-tests` or `--inputs`)",
                    missing.join(", ")
                )
            },
        });
    }

    // 2c. A port must return an error where the original raises one, not abort.
    let mut crashes = Vec::new();
    if !failing.is_empty() {
        let lines: Vec<&str> = port.lines().collect();
        for (index, line) in lines.iter().enumerate() {
            let code = line.split("//").next().unwrap_or("");
            let token = ["std::abort", "std::terminate", "assert(", "throw "]
                .iter()
                .find(|token| code.contains(**token));
            let Some(token) = token else { continue };
            let explained = lines[..index]
                .iter()
                .rev()
                .take_while(|above| above.trim_start().starts_with("//"))
                .any(|above| above.contains("INFALLIBLE:"))
                || line.contains("INFALLIBLE:");
            if !explained {
                crashes.push(format!("src/port.cpp:{} `{token}`", index + 1));
            }
        }
    }
    steps.push(Step {
        name: "panics".into(),
        ok: crashes.is_empty(),
        detail: if crashes.is_empty() {
            if failing.is_empty() {
                "no abort/assert/throw to review".into()
            } else {
                "no abort, assert or throw where the original returns an error".into()
            }
        } else {
            format!(
                "return `cppan::unexpected(PortError{{...}})` instead, or mark an impossible state \
                 with `// INFALLIBLE: <why>`: {}",
                crashes.join(", ")
            )
        },
    });

    // 2c2. A C-style C++ port must be modern C++: the idioms are the job, not a
    //      preference. Anything kept deliberately is annotated `// C-STYLE: why`.
    if project.source_lang == "cpp" {
        let lines: Vec<&str> = port.lines().collect();
        let mut c_style = Vec::new();
        for (index, line) in lines.iter().enumerate() {
            let code = line.split("//").next().unwrap_or("");
            let Some(needle) = [
                "NULL",
                "malloc(",
                "free(",
                "strcpy",
                "strlen",
                "memcpy",
                "printf(",
                "sprintf(",
                "(char*)",
                "(int)",
                "(double)",
                "typedef struct",
            ]
            .iter()
            .find(|needle| code.contains(**needle)) else {
                continue;
            };
            let explained = lines[..index]
                .iter()
                .rev()
                .take_while(|above| above.trim_start().starts_with("//"))
                .any(|above| above.contains("C-STYLE:"))
                || line.contains("C-STYLE:");
            if !explained {
                c_style.push(format!("src/port.cpp:{} `{needle}`", index + 1));
            }
        }
        steps.push(Step {
            name: "modern".into(),
            ok: c_style.is_empty(),
            detail: if c_style.is_empty() {
                "no C idioms left in the port: string_view, spans, ranges and the profile-gated facilities"
                    .into()
            } else {
                format!(
                    "replace these with the modern facility the analyzer named, or annotate \
                     `// C-STYLE: why it stays`: {}",
                    c_style.join(", ")
                )
            },
        });
    }

    // 2d. A port that is much bigger than its source is usually a crate or a loop
    //     re-implemented by hand.
    let port_lines = port.lines().count();
    let source_lines = project.lines as usize;
    if source_lines > 0 {
        let growth = port_lines as f64 / source_lines as f64;
        let allowed = 3.0 * source_lines as f64 + 50.0;
        steps.push(Step {
            name: "size".into(),
            ok: (port_lines as f64) <= allowed,
            detail: format!(
                "{port_lines} lines of C++ for {source_lines} lines of {} ({growth:.1}x)",
                project.source_lang
            ),
        });
    }

    // 2e. The recording must describe the source as it is now.
    let recorded_from = project
        .dir
        .join(crate::project::META_DIR)
        .join("recorded-from.json");
    let recorded = util::read(&recorded_from)
        .ok()
        .and_then(|text| Json::parse(&text).ok());
    let source_hash = util::hash64(&util::read(&project.source_file).unwrap_or_default());
    let stored_hash = recorded
        .as_ref()
        .and_then(|doc| doc.get("source_hash"))
        .and_then(Json::as_str)
        .unwrap_or("")
        .to_string();
    steps.push(Step {
        name: "recording".into(),
        ok: stored_hash.is_empty() || stored_hash == source_hash,
        detail: if stored_hash.is_empty() {
            "no recorded-from.json: record again after editing the original".into()
        } else if stored_hash == source_hash {
            "the cases were recorded from this version of the original".into()
        } else {
            format!(
                "{} changed since it was recorded: `cpp-anything record {}` again",
                util::short_path(&project.source_file),
                project.dir.display()
            )
        },
    });

    // 2f. Coverage is information, never a failure: it says what the cases miss.
    if let Some(coverage) = recorded.as_ref().and_then(|doc| doc.get("coverage")) {
        let reached = coverage.get("reached").and_then(Json::as_i64).unwrap_or(0);
        let lines = coverage.get("lines").and_then(Json::as_i64).unwrap_or(0);
        if lines > 0 {
            let percent = (reached as f64 / lines as f64 * 100.0).round() as i64;
            steps.push(Step {
                name: "coverage".into(),
                ok: true,
                detail: format!(
                    "the recorded cases ran {reached} of {lines} source lines ({percent}%)"
                ),
            });
        }
    }

    // 3. It has to build.
    let build = crate::build::build(project, options.clean, options.cxx.as_deref(), options.jobs);
    let driver = match build {
        Ok(outcome) => {
            steps.push(Step {
                name: "build".into(),
                ok: true,
                detail: format!(
                    "{} ({} warning{})",
                    util::short_path(&outcome.driver),
                    outcome.warnings,
                    if outcome.warnings == 1 { "" } else { "s" }
                ),
            });
            Some(outcome.driver)
        }
        Err(error) => {
            steps.push(Step {
                name: "build".into(),
                ok: false,
                detail: error,
            });
            None
        }
    };

    // 4. The driver must expose exactly the ported functions.
    if let Some(driver) = &driver {
        let run = util::run(
            &driver.display().to_string(),
            &["--list".to_string()],
            Some(&project.dir),
        );
        let listed: Vec<String> = run
            .stdout
            .lines()
            .map(|line| line.trim().to_string())
            .filter(|line| !line.is_empty())
            .collect();
        let mut expected: Vec<String> = project
            .functions
            .iter()
            .map(|function| function.name.clone())
            .collect();
        let mut actual = listed.clone();
        expected.sort();
        actual.sort();
        let ok = run.ok && actual == expected;
        steps.push(Step {
            name: "dispatch".into(),
            ok,
            detail: if ok {
                format!("{} function(s) reachable from the driver", expected.len())
            } else {
                format!("expected {expected:?} from the driver, got {actual:?}")
            },
        });
    }

    // 5. Every recorded case must match.
    match (
        &driver,
        crate::diff::load_cases(&project.dir.join("cases.jsonl")),
    ) {
        (Some(driver), Ok(_)) => {
            let options = crate::diff::Options {
                max_report: 5,
                ..Default::default()
            };
            match crate::diff::run(project, driver, &options) {
                Ok(report) => {
                    let recorded = project
                        .recorded
                        .as_ref()
                        .map(|recorded| {
                            format!(" (recorded {}, {} cases)", recorded.at, recorded.cases)
                        })
                        .unwrap_or_default();
                    steps.push(Step {
                        name: "diff".into(),
                        ok: report.passed(),
                        detail: format!(
                            "{}/{} cases match{recorded}",
                            report.matched, report.total
                        ),
                    });
                }
                Err(error) => steps.push(Step {
                    name: "diff".into(),
                    ok: false,
                    detail: error,
                }),
            }
        }
        (_, Err(error)) => steps.push(Step {
            name: "diff".into(),
            ok: false,
            detail: error,
        }),
        (None, _) => steps.push(Step {
            name: "diff".into(),
            ok: false,
            detail: "skipped: no build".into(),
        }),
    }

    // 6. Optionally, speed.
    if let (Some((repeat, minimum)), Some(driver)) = (options.relevance, driver.as_deref()) {
        match crate::bench::run(project, driver, repeat) {
            Ok(report) => {
                let ok = report.speedup() >= 1.0;
                steps.push(Step {
                    name: "bench".into(),
                    ok,
                    detail: format!(
                        "{:.2}x ({:.0} -> {:.0} ns/call; asked for at least {minimum}x)",
                        report.speedup(),
                        report.original_ns,
                        report.cpp_ns
                    ),
                });
            }
            Err(error) => steps.push(Step {
                name: "bench".into(),
                ok: false,
                detail: error,
            }),
        }
    }

    Ok(GateReport { steps })
}

pub fn render(report: &GateReport, project: &Project) -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "cpp-anything gate: {} — {}\n\n",
        project.module,
        if report.passed() { "PASS" } else { "FAIL" }
    ));
    let width = report
        .steps
        .iter()
        .map(|step| step.name.chars().count())
        .max()
        .unwrap_or(6);
    for step in &report.steps {
        out.push_str(&format!(
            "  {}  {:<width$}  {}\n",
            if step.ok { "✓" } else { "✗" },
            step.name,
            step.detail.lines().next().unwrap_or(""),
            width = width + 2
        ));
        for line in step.detail.lines().skip(1) {
            out.push_str(&format!("     {}  {}\n", " ".repeat(width + 2), line));
        }
    }
    if !report.passed() {
        out.push_str("\nnot done: fix the ✗ steps above, then run gate again.\n");
    } else {
        out.push_str("\ndone: the port matches every recorded case and the toolchain supports what it uses.\n");
    }
    out
}

pub fn to_json(report: &GateReport, project: &Project) -> Json {
    Json::obj([
        ("schema", Json::str("cpp-anything/gate@1")),
        ("unit", Json::str(project.unit.clone())),
        ("passed", Json::Bool(report.passed())),
        (
            "steps",
            Json::Array(
                report
                    .steps
                    .iter()
                    .map(|step| {
                        Json::obj([
                            ("name", Json::str(step.name.clone())),
                            ("ok", Json::Bool(step.ok)),
                            ("detail", Json::str(step.detail.clone())),
                        ])
                    })
                    .collect(),
            ),
        ),
    ])
}
