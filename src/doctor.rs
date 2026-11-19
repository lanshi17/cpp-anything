//! `doctor`: the tools a port needs, whether they are there, and how to install them.

use std::path::Path;

use crate::cli::Parsed;
use crate::json::Json;
use crate::matrix::Matrix;
use crate::util;

struct Check {
    name: &'static str,
    ok: bool,
    detail: String,
    fix: &'static str,
}

pub fn command(args: &Parsed) -> Result<i32, String> {
    let mut checks = Vec::new();

    let cmake = util::which("cmake");
    checks.push(Check {
        name: "cmake",
        ok: cmake.is_some(),
        detail: cmake
            .as_ref()
            .map(|path| {
                let run = util::run(&path.display().to_string(), &["--version".into()], None);
                run.stdout.lines().next().unwrap_or("cmake").to_string()
            })
            .unwrap_or_else(|| "not found".into()),
        fix: "apt install cmake · brew install cmake · winget install Kitware.CMake",
    });

    let compiler = ["c++", "g++", "clang++", "cl"]
        .iter()
        .find(|candidate| util::which(candidate).is_some())
        .copied();
    checks.push(Check {
        name: "c++ compiler",
        ok: compiler.is_some(),
        detail: compiler
            .map(|name| {
                let run = util::run(name, &["--version".into()], None);
                let line = run.stdout.lines().next().unwrap_or("").to_string();
                if line.is_empty() {
                    name.to_string()
                } else {
                    line
                }
            })
            .unwrap_or_else(|| "no c++, g++, clang++ or cl on PATH".into()),
        fix: "apt install g++ · xcode-select --install · install Visual Studio Build Tools",
    });

    let python = crate::python::available();
    checks.push(Check {
        name: "python3",
        ok: python.is_some(),
        detail: match &python {
            Some(version) => format!("{} ({})", crate::python::interpreter(), version),
            None => format!("{} not found", crate::python::interpreter()),
        },
        fix: "set CPPAN_PYTHON=/path/to/python3 if it is installed under another name",
    });

    let matrix = Matrix::get();
    checks.push(Check {
        name: "matrix",
        ok: matrix.features.len() > 1000 || matrix.features.len() > 100,
        detail: format!(
            "{} features over {} standards, generated {}",
            matrix.features.len(),
            matrix.standards.len(),
            matrix.generated_at
        ),
        fix: "run scripts/sync_matrix.py, then rebuild cpp-anything",
    });

    let guides = crate::docs::check(Path::new("docs/guides")).unwrap_or_default();
    checks.push(Check {
        name: "guides",
        ok: guides.is_empty(),
        detail: if guides.is_empty() {
            "docs/guides matches data/matrix.json".into()
        } else {
            format!("{} drifted: {}", guides.len(), guides.join(", "))
        },
        fix: "cpp-anything matrix docs",
    });

    if args.flag("json") {
        let doc = Json::obj([
            ("schema", Json::str("cpp-anything/doctor@1")),
            ("ok", Json::Bool(checks.iter().all(|check| check.ok))),
            (
                "checks",
                Json::Array(
                    checks
                        .iter()
                        .map(|check| {
                            Json::obj([
                                ("name", Json::str(check.name)),
                                ("ok", Json::Bool(check.ok)),
                                ("detail", Json::str(check.detail.clone())),
                                ("fix", Json::str(check.fix)),
                            ])
                        })
                        .collect(),
                ),
            ),
        ]);
        println!("{}", doc.to_string_pretty());
    } else {
        println!("cpp-anything doctor\n");
        let width = checks
            .iter()
            .map(|check| check.name.len())
            .max()
            .unwrap_or(12);
        for check in &checks {
            println!(
                "  {}  {:<width$}  {}",
                if check.ok { "✓" } else { "✗" },
                check.name,
                check.detail,
                width = width + 2
            );
            if !check.ok {
                println!("     {}  fix: {}", " ".repeat(width + 2), check.fix);
            }
        }
        if checks.iter().all(|check| check.ok) {
            println!("\nready: analyze, bridge, record, diff, bench and gate can run.");
        } else {
            println!("\nnot ready: fix the ✗ lines above.");
        }
    }
    Ok(if checks.iter().all(|check| check.ok) {
        0
    } else {
        1
    })
}
