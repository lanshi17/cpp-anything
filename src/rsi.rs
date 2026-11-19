//! `rsi`: recursive self-improvement, made checkable.
//!
//! The tool that proves ports behave like their originals uses the same trick on
//! itself: the matrix must still agree with the guides it generated, the CLI must
//! still print what was recorded, and the generator must still produce a project
//! that compiles and replays byte-for-byte. A change that breaks any of that fails
//! `rsi check` before it can reach a user's port.

use std::path::Path;

use crate::cli::Parsed;
use crate::json::Json;
use crate::project::Project;
use crate::util;

pub struct Step {
    pub name: &'static str,
    pub ok: bool,
    pub detail: String,
}

/// Deterministic commands whose exact output is recorded as the tool's contract.
const GOLDEN: &[(&str, &[&str])] = &[
    (
        "matrix-features-gcc-13",
        &[
            "matrix",
            "features",
            "--toolchain",
            "gcc",
            "--version",
            "13",
            "--std",
            "c++23",
        ],
    ),
    (
        "matrix-features-clang-20-json",
        &[
            "matrix",
            "features",
            "--toolchain",
            "clang",
            "--version",
            "20",
            "--std",
            "c++23",
            "--json",
        ],
    ),
    (
        "matrix-features-msvc-19.44",
        &[
            "matrix",
            "features",
            "--toolchain",
            "msvc",
            "--version",
            "19.44",
            "--std",
            "c++23",
        ],
    ),
    (
        "matrix-explain-expected",
        &["matrix", "explain", "__cpp_lib_expected"],
    ),
    (
        "matrix-explain-print-json",
        &["matrix", "explain", "std::print", "--json"],
    ),
    (
        "matrix-profile-gcc-13",
        &[
            "matrix",
            "profile",
            "--toolchain",
            "gcc",
            "--version",
            "13",
            "--std",
            "c++23",
        ],
    ),
    (
        "matrix-profile-msvc-19.44",
        &[
            "matrix",
            "profile",
            "--toolchain",
            "msvc",
            "--version",
            "19.44",
            "--std",
            "c++23",
        ],
    ),
    (
        "analyze-fixture-json",
        &["analyze", "tests/fixtures/python/textkit", "--json"],
    ),
    ("matrix-docs-check", &["matrix", "docs", "--check"]),
];

pub fn command(args: &Parsed) -> Result<i32, String> {
    let sub = args
        .positional
        .first()
        .map(String::as_str)
        .unwrap_or("check");
    let root = util::repo_root().ok_or(
        "rsi runs inside the cpp-anything checkout: no data/matrix.json found above the current \
         directory",
    )?;
    match sub {
        "check" => check(&root, args),
        "record" => record(&root),
        other => Err(format!(
            "unknown rsi command {other:?}; try `rsi check` or `rsi record`"
        )),
    }
}

/// Copies the shipped CLI example, rebuilds its front door from the committed cases
/// and glue, and requires `app lift` to reach zero fallback.
fn app_step(root: &Path) -> Result<Step, String> {
    let example = root.join("examples").join("slugify-cli");
    if !example.join("cpp").join("app-cases").exists() {
        return Ok(Step {
            name: "self app",
            ok: false,
            detail: "examples/slugify-cli has no recorded app cases".into(),
        });
    }
    let scratch = std::env::temp_dir().join(format!("cpp-anything-rsi-app-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&scratch);
    let _ = std::fs::remove_dir_all(&scratch);
    std::fs::create_dir_all(&scratch).map_err(|error| format!("{}: {error}", scratch.display()))?;
    copy_dir(&example, &scratch)?;
    // The copied glue links the unit by absolute path: keep that path.
    let front = run_cli(&scratch, &["app", "front-door", "."])?;
    if front.contains("✗") || !front.contains("passed") {
        return Ok(Step {
            name: "self app",
            ok: false,
            detail: format!(
                "app front-door did not replay: {}",
                util::truncate_text(&front, 200)
            ),
        });
    }
    let lift = run_cli(&scratch, &["app", "lift", "."])?;
    let ok = lift.contains("standalone") && lift.contains("0 fallback");
    let detail = if ok {
        let _ = std::fs::remove_dir_all(&scratch);
        "recorded the CLI, rebuilt the front door and lifted it with 0 fallback".to_string()
    } else {
        format!(
            "app lift did not reach standalone: {}",
            util::truncate_text(&lift, 200)
        )
    };
    Ok(Step {
        name: "self app",
        ok,
        detail,
    })
}

fn http_step(root: &Path) -> Result<Step, String> {
    let example = root.join("examples").join("http-app");
    if !example.join("cpp").join("app-cases").exists() {
        return Ok(Step {
            name: "self http",
            ok: false,
            detail: "examples/http-app has no recorded cases".into(),
        });
    }
    let output = run_cli(&example, &["app", "replay", "."])?;
    let ok = output.contains("cases passed") && !output.contains("✗");
    Ok(Step {
        name: "self http",
        ok,
        detail: if ok {
            "started the server, sent every recorded request and matched the responses".to_string()
        } else {
            util::truncate_text(&output, 200)
        },
    })
}

fn copy_dir(from: &Path, to: &Path) -> Result<(), String> {
    for entry in std::fs::read_dir(from).map_err(|error| format!("{}: {error}", from.display()))? {
        let entry = entry.map_err(|error| format!("{}: {error}", from.display()))?;
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if name == "build" || name == "__pycache__" || name == ".git" {
            continue; // build trees and caches belong to the machine that made them
        }
        let target = to.join(entry.file_name());
        if entry.path().is_dir() {
            std::fs::create_dir_all(&target)
                .map_err(|error| format!("{}: {error}", target.display()))?;
            copy_dir(&entry.path(), &target)?;
        } else {
            std::fs::copy(entry.path(), &target)
                .map_err(|error| format!("{}: {error}", target.display()))?;
        }
    }
    Ok(())
}

fn check(root: &Path, args: &Parsed) -> Result<i32, String> {
    let mut steps = Vec::new();

    // 1. The data behind every claim still parses and is complete enough to trust.
    let matrix = crate::matrix::Matrix::get();
    let sourced = matrix
        .sources
        .iter()
        .filter(|source| source.revision.is_some())
        .count();
    steps.push(Step {
        name: "matrix data",
        ok: matrix.features.len() >= 600 && sourced == matrix.sources.len(),
        detail: format!(
            "{} features, {} standard(s), {}/{} source pages with a revision id",
            matrix.features.len(),
            matrix.standards.len(),
            sourced,
            matrix.sources.len()
        ),
    });

    // 2. The guides are exactly what the data renders to.
    let drifted = crate::docs::check(&root.join("docs/guides"))?;
    steps.push(Step {
        name: "guides",
        ok: drifted.is_empty(),
        detail: if drifted.is_empty() {
            format!("{} guides match data/matrix.json", crate::docs::all().len())
        } else {
            format!(
                "run `cpp-anything matrix docs`; drifted: {}",
                drifted.join(", ")
            )
        },
    });

    // 3. The CLI still prints what it printed when the goldens were recorded.
    steps.push(golden_step(root)?);

    // 4. The generator still emits a project that compiles and passes its own diff.
    steps.push(self_port_step(root)?);

    // 5. The whole-app loop still records, builds a front door and lifts.
    steps.push(app_step(root)?);

    // 6. Server mode still starts a server, sends the recorded requests and compares.
    steps.push(http_step(root)?);

    if args.flag("json") {
        let doc = Json::obj([
            ("schema", Json::str("cpp-anything/rsi@1")),
            ("passed", Json::Bool(steps.iter().all(|step| step.ok))),
            (
                "steps",
                Json::Array(
                    steps
                        .iter()
                        .map(|step| {
                            Json::obj([
                                ("name", Json::str(step.name)),
                                ("ok", Json::Bool(step.ok)),
                                ("detail", Json::str(step.detail.clone())),
                            ])
                        })
                        .collect(),
                ),
            ),
        ]);
        println!("{}", doc.to_string_pretty());
    } else {
        println!(
            "cpp-anything rsi: {}\n",
            if steps.iter().all(|step| step.ok) {
                "PASS"
            } else {
                "FAIL"
            }
        );
        let width = steps.iter().map(|step| step.name.len()).max().unwrap_or(10);
        for step in &steps {
            println!(
                "  {}  {:<width$}  {}",
                if step.ok { "✓" } else { "✗" },
                step.name,
                step.detail,
                width = width + 2
            );
        }
    }
    Ok(if steps.iter().all(|step| step.ok) {
        0
    } else {
        1
    })
}

fn normalize(text: &str, root: &Path) -> String {
    let mut out = text.replace(&root.display().to_string(), "<repo>");
    if let Ok(canonical) = root.canonicalize() {
        out = out.replace(&canonical.display().to_string(), "<repo>");
    }
    let temp = std::env::temp_dir().display().to_string();
    out = out.replace(&temp, "<tmp>");
    // A re-sync dates the matrix; that is not a change in what the tool prints.
    out.lines()
        .map(|line| {
            if line.starts_with("#define CPPAN_MATRIX_GENERATED_AT") {
                "#define CPPAN_MATRIX_GENERATED_AT \"<date>\""
            } else {
                line.trim_end()
            }
        })
        .collect::<Vec<_>>()
        .join("\n")
}

fn run_cli(root: &Path, argv: &[&str]) -> Result<String, String> {
    let exe = std::env::current_exe().map_err(|error| format!("current_exe: {error}"))?;
    let args: Vec<String> = argv.iter().map(|arg| arg.to_string()).collect();
    let run = util::run(&exe.display().to_string(), &args, Some(root));
    if run.ok {
        Ok(run.stdout)
    } else {
        // A failing command can be part of the contract too (matrix docs --check
        // exits 1 on drift); record stdout as-is and let the comparison decide.
        Ok(format!("{}{}", run.stdout, run.stderr))
    }
}

fn golden_path(root: &Path) -> std::path::PathBuf {
    root.join("tests").join("golden").join("cli.jsonl")
}

fn record(root: &Path) -> Result<i32, String> {
    let mut lines = Vec::new();
    for (name, argv) in GOLDEN {
        let output = run_cli(root, argv)?;
        let entry = Json::obj([
            ("name", Json::str(*name)),
            (
                "argv",
                Json::Array(argv.iter().map(|arg| Json::str(*arg)).collect()),
            ),
            ("stdout", Json::str(normalize(&output, root))),
        ]);
        lines.push(entry.to_string_compact());
    }
    util::write(&golden_path(root), &(lines.join("\n") + "\n"))?;
    println!(
        "recorded {} golden transcripts in {}",
        lines.len(),
        util::short_path(&golden_path(root))
    );
    Ok(0)
}

fn golden_step(root: &Path) -> Result<Step, String> {
    let path = golden_path(root);
    if !path.exists() {
        return Ok(Step {
            name: "cli goldens",
            ok: false,
            detail: "tests/golden/cli.jsonl is missing: run `cpp-anything rsi record`".into(),
        });
    }
    let text = util::read(&path)?;
    let stored: Vec<Json> = text
        .lines()
        .filter(|line| !line.trim().is_empty())
        .map(Json::parse)
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| format!("{}: {error}", path.display()))?;
    let mut drifted = Vec::new();
    for entry in &stored {
        let name = entry.get("name").and_then(Json::as_str).unwrap_or("?");
        let argv: Vec<&str> = entry
            .get("argv")
            .and_then(Json::as_array)
            .unwrap_or_default()
            .iter()
            .filter_map(Json::as_str)
            .collect();
        if argv.is_empty() {
            continue;
        }
        let expected = entry.get("stdout").and_then(Json::as_str).unwrap_or("");
        let actual = normalize(&run_cli(root, &argv)?, root);
        if actual != expected {
            drifted.push(name.to_string());
        }
    }
    Ok(Step {
        name: "cli goldens",
        ok: drifted.is_empty(),
        detail: if drifted.is_empty() {
            format!("{} commands print what was recorded", stored.len())
        } else {
            format!(
                "output changed for {}: run `cpp-anything rsi record` if the change is intended",
                drifted.join(", ")
            )
        },
    })
}

/// Regenerates the shipped example into a temp directory, drops the reference port
/// in, builds it and replays the recorded cases. This is cpp-anything's own loop,
/// applied to its own generator.
fn self_port_step(root: &Path) -> Result<Step, String> {
    let example = root.join("examples").join("textkit-cpp");
    if !example.exists() {
        return Ok(Step {
            name: "self port",
            ok: false,
            detail: format!("{} is missing", util::short_path(&example)),
        });
    }
    let project = Project::load(&example)?;
    let scratch = std::env::temp_dir().join(format!("cpp-anything-rsi-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&scratch);
    std::fs::create_dir_all(&scratch).map_err(|error| format!("{}: {error}", scratch.display()))?;

    let mut rebuilt = project.clone();
    rebuilt.dir = scratch.clone();
    crate::codegen::write_project(&rebuilt, true)?;

    // The reference port and the recorded cases are the example's content.
    for relative in ["src/port.cpp", "cases.jsonl"] {
        let from = example.join(relative);
        let to = scratch.join(relative);
        std::fs::copy(&from, &to).map_err(|error| format!("{}: {error}", from.display()))?;
    }
    let outcome = crate::build::build(&rebuilt, true, None, 4)?;
    let options = crate::diff::Options {
        max_report: 3,
        ..Default::default()
    };
    let report = crate::diff::run(&rebuilt, &outcome.driver, &options)?;
    let ok = report.passed();
    let detail = if ok {
        let _ = std::fs::remove_dir_all(&scratch);
        format!(
            "generated {}, built it ({} warnings) and replayed {}/{} cases of the shipped port",
            project.module, outcome.warnings, report.matched, report.total
        )
    } else {
        format!(
            "{} of {} cases mismatched; output kept in {}",
            report.mismatches.len(),
            report.total,
            scratch.display()
        )
    };
    Ok(Step {
        name: "self port",
        ok,
        detail,
    })
}
