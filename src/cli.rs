//! The command line. No argument-parser crate: the surface is small enough to own,
//! and `--help` stays in one place next to the code that implements it.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use crate::analyze;
use crate::json::Json;
use crate::matrix::{Kind, Matrix};
use crate::project::{Project, Target};
use crate::util;

pub struct Parsed {
    pub positional: Vec<String>,
    pub values: BTreeMap<String, String>,
    pub flags: BTreeSet<String>,
}

impl Parsed {
    pub fn value(&self, name: &str) -> Option<&str> {
        self.values.get(name).map(String::as_str)
    }

    pub fn flag(&self, name: &str) -> bool {
        self.flags.contains(name)
    }

    pub fn number(&self, name: &str) -> Option<u64> {
        self.value(name).and_then(|value| value.parse().ok())
    }
}

pub fn parse_args(argv: &[String]) -> Result<Parsed, String> {
    let mut parsed = Parsed {
        positional: Vec::new(),
        values: BTreeMap::new(),
        flags: BTreeSet::new(),
    };
    let mut index = 0;
    while index < argv.len() {
        let arg = &argv[index];
        if let Some(body) = arg.strip_prefix("--") {
            if let Some((key, value)) = body.split_once('=') {
                parsed.values.insert(key.to_string(), value.to_string());
            } else if matches!(
                body,
                "json"
                    | "force"
                    | "clean"
                    | "check"
                    | "strict-errors"
                    | "help"
                    | "list"
                    | "bench"
                    | "refresh"
            ) {
                parsed.flags.insert(body.to_string());
            } else {
                let next = argv.get(index + 1);
                match next {
                    Some(value) if !value.starts_with("--") => {
                        parsed.values.insert(body.to_string(), value.clone());
                        index += 1;
                    }
                    _ => {
                        parsed.flags.insert(body.to_string());
                    }
                }
            }
        } else if arg == "-h" {
            parsed.flags.insert("help".into());
        } else if arg == "-o" {
            if let Some(value) = argv.get(index + 1) {
                parsed.values.insert("out".into(), value.clone());
                index += 1;
            } else {
                return Err("-o needs a file name".into());
            }
        } else {
            parsed.positional.push(arg.clone());
        }
        index += 1;
    }
    Ok(parsed)
}

pub fn run(argv: &[String]) -> Result<i32, String> {
    if argv.is_empty() {
        print!("{HELP}");
        return Ok(0);
    }
    let command = argv[0].as_str();
    let rest = &argv[1..];
    let args = parse_args(rest)?;
    if args.flag("help") {
        print!("{HELP}");
        return Ok(0);
    }
    match command {
        "matrix" => matrix_command(&args),
        "analyze" => analyze_command(&args),
        "bridge" => bridge_command(&args),
        "record" => record_command(&args),
        "build" => build_command(&args),
        "diff" => diff_command(&args),
        "bench" => bench_command(&args),
        "gate" => gate_command(&args),
        "refresh" => refresh_command(&args),
        "rsi" => crate::rsi::command(&args),
        "doctor" => crate::doctor::command(&args),
        "mcp" => crate::mcp::serve(),
        "help" | "--help" => {
            print!("{HELP}");
            Ok(0)
        }
        other => Err(format!(
            "unknown command {other:?}; run `cpp-anything help`"
        )),
    }
}

const HELP: &str = "\
cpp-anything — rewrite your app in C++ (C++20/23) and prove it still behaves the same

  matrix    what compilers and standards support, from cppreference's tables
  analyze   the units in a repository and what porting each one costs
  bridge    generate the C++ project for one unit (port skeleton + driver)
  record    run the original on generated edge cases and save what it returns
  build     configure and build the C++ project with CMake
  diff      replay every recorded case through the port and compare
  bench     the original's per-call cost against the port's
  gate      build + diff + matrix + dispatch checks; done means this passes
  refresh   regenerate the generated files after a signature change
  rsi       verify cpp-anything itself with its own loop
  doctor    check the tools a port needs
  mcp       run as an MCP server on stdio

Run `cpp-anything <command> --help` for one command's options.

  matrix features [--toolchain gcc] [--version 13] [--std c++23] [--kind core|library] [--json]
  matrix explain <feature|macro> [--json]
  matrix profile  [--toolchain gcc] [--version 13] [--std c++23] [-o FILE]
  matrix docs     [--out DIR] [--check]
  matrix sync     [--offline]
  analyze <repo>  [-o plan.md] [--json]
  bridge  <repo> <unit> [--out DIR] [--toolchain gcc] [--version 13] [--std c++23]
                        [--module-root DIR] [--force]
  record  <project> [--cases N] [--seed N] [--inputs FILE]
  diff    <project> [--json] [--strict-errors] [--tolerance F] [--only FN] [--max-report N]
  gate    <project> [--json] [--clean] [--cxx PATH] [--jobs N] [--bench]
  rsi     check|record [--json]
";

fn matrix_command(args: &Parsed) -> Result<i32, String> {
    let sub = args
        .positional
        .first()
        .map(String::as_str)
        .unwrap_or("features");
    let matrix = Matrix::get();
    match sub {
        "features" | "select" | "support" => {
            let toolchain = matrix
                .resolve_toolchain(args.value("toolchain").unwrap_or("gcc"))
                .ok_or_else(|| {
                    format!(
                        "unknown toolchain {:?}",
                        args.value("toolchain").unwrap_or("gcc")
                    )
                })?;
            let latest = matrix
                .features
                .iter()
                .filter_map(|feature| feature.first_release(&toolchain).map(str::to_string))
                .max_by(|a, b| util::version_key(a).cmp(&util::version_key(b)))
                .unwrap_or_else(|| "15".into());
            let version = args.value("version").unwrap_or(&latest).to_string();
            let std = args.value("std").unwrap_or("c++23").to_string();
            let kind = args.value("kind").unwrap_or("all");
            let audit = matrix.audit(&toolchain, &version, &std);
            let keep = |kind_wanted: &str| kind == "all" || kind_wanted == kind;
            if args.flag("json") {
                let features = |list: &[&'static crate::matrix::Feature]| {
                    Json::Array(
                        list.iter()
                            .filter(|feature| keep(feature.kind.label()))
                            .map(|feature| {
                                Json::obj([
                                    ("id", Json::str(feature.id.clone())),
                                    ("name", Json::str(feature.name.clone())),
                                    ("kind", Json::str(feature.kind.label())),
                                    ("std", Json::str(feature.std.clone())),
                                    ("macro", Json::str(feature.macro_id.clone())),
                                    ("paper", Json::str(feature.paper.clone())),
                                ])
                            })
                            .collect(),
                    )
                };
                let doc = Json::obj([
                    ("schema", Json::str("cpp-anything/matrix-features@1")),
                    ("toolchain", Json::str(toolchain.clone())),
                    (
                        "toolchain_label",
                        Json::str(matrix.toolchain_label(&toolchain)),
                    ),
                    ("version", Json::str(version.clone())),
                    ("std", Json::str(std.clone())),
                    ("supported", features(&audit.supported)),
                    ("partial", features(&audit.partial)),
                    ("missing", features(&audit.missing)),
                ]);
                println!("{}", doc.to_string_pretty());
            } else {
                let total = audit.supported.len() + audit.partial.len() + audit.missing.len();
                let core = audit
                    .supported
                    .iter()
                    .filter(|feature| feature.kind == Kind::Core)
                    .count();
                let library = audit.supported.len() - core;
                println!(
                    "{} {} · {} — {}/{} features supported ({} core, {} library), {} partial, {} missing",
                    matrix.toolchain_label(&toolchain),
                    version,
                    std,
                    audit.supported.len(),
                    total,
                    core,
                    library,
                    audit.partial.len(),
                    audit.missing.len()
                );
                let mut rows: Vec<&'static crate::matrix::Feature> = audit
                    .supported
                    .iter()
                    .copied()
                    .filter(|feature| keep(feature.kind.label()))
                    .collect();
                rows.sort_by(|a, b| (a.kind.label(), &a.name).cmp(&(b.kind.label(), &b.name)));
                println!("\nsupported ({}):", rows.len());
                for feature in &rows {
                    println!(
                        "  {:<60} {}",
                        util::truncate_text(&feature.name, 60),
                        feature.macro_id.split_whitespace().next().unwrap_or("")
                    );
                }
                let mut rows: Vec<&'static crate::matrix::Feature> = audit
                    .missing
                    .iter()
                    .copied()
                    .filter(|feature| keep(feature.kind.label()))
                    .collect();
                rows.sort_by(|a, b| (a.kind.label(), &a.name).cmp(&(b.kind.label(), &b.name)));
                if !rows.is_empty() {
                    println!("\nnot in this release ({}):", rows.len());
                    for feature in rows.iter().take(25) {
                        println!(
                            "  {:<60} {}",
                            util::truncate_text(&feature.name, 60),
                            feature.kind.label()
                        );
                    }
                    if rows.len() > 25 {
                        println!("  … {} more (--json lists all)", rows.len() - 25);
                    }
                }
            }
            Ok(0)
        }
        "explain" => {
            let query = args.positional.get(1).ok_or(
                "matrix explain needs a feature name or macro, e.g. `matrix explain std::expected`",
            )?;
            let found = matrix.lookup(query);
            if found.is_empty() {
                return Err(format!("nothing in the matrix matches {query:?}"));
            }
            if args.flag("json") {
                let items = found
                    .iter()
                    .map(|feature| {
                        let support = feature
                            .support
                            .iter()
                            .map(|(toolchain, support)| {
                                (
                                    toolchain.clone(),
                                    Json::obj([
                                        ("status", Json::str(support.status.label())),
                                        (
                                            "min",
                                            support
                                                .min
                                                .as_ref()
                                                .map(|min| Json::str(min.clone()))
                                                .unwrap_or(Json::Null),
                                        ),
                                        (
                                            "notes",
                                            Json::Array(
                                                support
                                                    .notes
                                                    .iter()
                                                    .map(|note| Json::str(note.clone()))
                                                    .collect(),
                                            ),
                                        ),
                                    ]),
                                )
                            })
                            .collect::<Vec<_>>();
                        Json::obj([
                            ("id", Json::str(feature.id.clone())),
                            ("name", Json::str(feature.name.clone())),
                            ("kind", Json::str(feature.kind.label())),
                            ("std", Json::str(feature.std.clone())),
                            ("macro", Json::str(feature.macro_id.clone())),
                            ("paper", Json::str(feature.paper.clone())),
                            ("support", Json::Object(support)),
                        ])
                    })
                    .collect::<Vec<_>>();
                println!("{}", Json::Array(items).to_string_pretty());
            } else {
                for feature in found.iter().take(6) {
                    println!(
                        "{} — {} {}, {}",
                        feature.name,
                        feature.kind.label(),
                        feature.std,
                        if feature.paper.is_empty() {
                            String::new()
                        } else {
                            format!("({})", feature.paper)
                        }
                    );
                    if !feature.macro_id.is_empty() {
                        println!("  macro   {}", feature.macro_id);
                    }
                    for (toolchain, support) in &feature.support {
                        if !matches!(toolchain.as_str(), "gcc" | "clang" | "msvc" | "aclang") {
                            continue;
                        }
                        let mut line = format!(
                            "  {:<7} {}",
                            toolchain,
                            match (&support.status, &support.min) {
                                (crate::matrix::Status::Yes, Some(min)) => min.clone(),
                                (crate::matrix::Status::Yes, None) => "yes".into(),
                                (crate::matrix::Status::Partial, Some(min)) =>
                                    format!("{min} (partial)"),
                                _ => "—".into(),
                            }
                        );
                        if toolchain == "msvc" {
                            if let Some(title) = support
                                .min
                                .as_ref()
                                .and_then(|min| matrix.msvc_titles.get(min))
                            {
                                line.push_str(&format!("  ({title})"));
                            }
                        }
                        println!("{line}");
                        for note in &support.notes {
                            println!("          note: {note}");
                        }
                    }
                    println!();
                }
                if found.len() > 6 {
                    println!("… {} more matches; narrow the query", found.len() - 6);
                }
            }
            Ok(0)
        }
        "profile" => {
            let target = target_from(args, matrix)?;
            let profile =
                crate::matrix::render_profile(&target.toolchain, &target.version, &target.std);
            match args.value("out") {
                Some(path) => {
                    util::write(Path::new(path), &profile)?;
                    println!("wrote {path}");
                }
                None => print!("{profile}"),
            }
            Ok(0)
        }
        "sync" => {
            // The data lives in the checkout and the binary embeds it, so syncing is
            // a two-step operation: refresh data/matrix.json, re-render the guides,
            // then rebuild to embed the new matrix.
            let root = util::repo_root().ok_or(
                "matrix sync needs the cpp-anything checkout: no data/matrix.json found above \
                 the current directory",
            )?;
            let script = root.join("scripts").join("sync_matrix.py");
            if !script.exists() {
                return Err(format!("{} is missing", script.display()));
            }
            let mut argv = vec![script.display().to_string()];
            if args.flag("offline") {
                argv.push("--offline".to_string());
            }
            let run = util::run(&crate::python::interpreter(), &argv, Some(&root));
            print!("{}", run.stdout);
            if !run.ok {
                return Err(format!(
                    "sync failed: {}\n{}",
                    run.failure(),
                    util::indent(&run.stderr, 2)
                ));
            }
            let guides = crate::docs::write(&root.join("docs").join("guides"))?;
            println!("re-rendered {guides} guides under docs/guides");
            println!("rebuild to embed the new matrix: `cargo build --release` (or `cargo install --path .`)");
            Ok(0)
        }
        "docs" => {
            let root = PathBuf::from(args.value("out").unwrap_or("docs/guides"));
            if args.flag("check") {
                let drifted = crate::docs::check(&root)?;
                if drifted.is_empty() {
                    println!("{}: up to date", root.display());
                    return Ok(0);
                }
                println!("{}: {} file(s) drifted:", root.display(), drifted.len());
                for path in &drifted {
                    println!("  {path}");
                }
                println!("\nrun `cpp-anything matrix docs` to regenerate them");
                return Ok(1);
            }
            let count = crate::docs::write(&root)?;
            println!("wrote {count} guides under {}", root.display());
            Ok(0)
        }
        "check" => {
            let drifted = crate::docs::check(Path::new("docs/guides"))?;
            if drifted.is_empty() {
                println!(
                    "guides match data/matrix.json ({} features)",
                    matrix.features.len()
                );
                Ok(0)
            } else {
                println!("{} guide(s) drifted: {drifted:?}", drifted.len());
                Ok(1)
            }
        }
        other => Err(format!(
            "unknown matrix command {other:?}; try features, explain, profile, docs, check"
        )),
    }
}

fn target_from(args: &Parsed, matrix: &Matrix) -> Result<Target, String> {
    let default = Target::default();
    let toolchain = matrix
        .resolve_toolchain(args.value("toolchain").unwrap_or(&default.toolchain))
        .ok_or_else(|| {
            format!(
                "unknown toolchain {:?}",
                args.value("toolchain").unwrap_or(&default.toolchain)
            )
        })?;
    Ok(Target {
        toolchain,
        version: args
            .value("version")
            .unwrap_or(&default.version)
            .to_string(),
        std: args.value("std").unwrap_or(&default.std).to_string(),
    })
}

fn analyze_command(args: &Parsed) -> Result<i32, String> {
    let repo = args
        .positional
        .first()
        .ok_or("analyze needs a repository path")?;
    let repo = Path::new(repo);
    let doc = analyze::analyze(repo)?;
    let json = analyze::to_json(&doc, repo);
    if args.flag("json") {
        println!("{}", json.to_string_pretty());
        return Ok(0);
    }
    let plan = analyze::render_plan(&doc, repo);
    match args.value("out") {
        Some(path) => {
            analyze::write_plan(Path::new(path), &plan)?;
            println!("wrote {path} ({})", analyze::summarize(&doc));
        }
        None => print!("{plan}"),
    }
    Ok(0)
}

fn bridge_command(args: &Parsed) -> Result<i32, String> {
    let repo = args
        .positional
        .first()
        .ok_or("bridge needs a repository path: `bridge <repo> <unit>`")?;
    let unit = args
        .positional
        .get(1)
        .ok_or("bridge needs a unit: a .py file or a dotted module (`bridge . textkit.words`)")?;
    let repo = Path::new(repo);
    let matrix = Matrix::get();
    let target = target_from(args, matrix)?;
    let resolved = analyze::resolve(repo, unit, args.value("module-root").map(Path::new))?;

    // C++20 or C++23, and a toolchain the matrix knows about. `std::expected` is
    // preferred, not required: C++20 targets get cppan::Result, same interface.
    let std_number: u32 = target.std.trim_start_matches("c++").parse().unwrap_or(0);
    if std_number < 20 {
        return Err(format!(
            "cpp-anything targets C++20 and C++23, not {}: pass `--std c++20` or `--std c++23`",
            target.std
        ));
    }
    if matrix.resolve_toolchain(&target.toolchain).is_none() {
        return Err(format!(
            "unknown toolchain {:?}: `cpp-anything matrix features --toolchain …` lists the ones \
             the matrix knows (gcc, clang, msvc, apple-clang, …)",
            target.toolchain
        ));
    }
    let expected = matrix.macro_available(
        "__cpp_lib_expected",
        &target.toolchain,
        &target.version,
        &target.std,
    );
    if !expected.available() {
        println!(
            "note: std::expected is not available on {} {} with {}; the generated code \
             uses cppan::Result (same interface) instead",
            target.toolchain, target.version, target.std
        );
    }

    let unit_doc = crate::python::inventory(
        &resolved.file,
        &resolved.module,
        resolved.package_root.as_deref(),
    )?;
    if unit_doc.functions.is_empty() {
        return Err(format!(
            "{} has no ported-able functions: the analyzer needs plain module-level functions with a \
             name not starting with '_' (decorated, async and *args functions are reported as skipped)",
            resolved.file.display()
        ));
    }
    let dir = PathBuf::from(args.value("out").unwrap_or(&format!(
        "{}/cpp/{}",
        repo.display(),
        util::sanitize_identifier(&unit_doc.unit).replace('_', "-")
    )));
    let created = util::prepare_dir(&dir, args.flag("force"))?;
    let dir = dir
        .canonicalize()
        .map_err(|error| format!("{}: {error}", dir.display()))?;
    let project = Project::from_unit(&unit_doc, target, dir.clone());
    let written = crate::codegen::write_project(&project, args.flag("force"))?;
    println!(
        "cpp-anything bridge: {} → {}\n  {} files written ({}){}",
        resolved.module,
        dir.display(),
        written.len(),
        project.functions.len(),
        if created {
            ", existing directory reused"
        } else {
            ""
        }
    );
    for function in &project.functions {
        println!("  · {}", function.signature_line());
    }
    if !project.skipped.is_empty() {
        println!("\n  skipped:");
        for (name, reason) in &project.skipped {
            println!("    · {name} ({reason})");
        }
    }
    println!(
        "\nnext:\n  cpp-anything record {dir}\n  # write src/port.cpp\n  cpp-anything build {dir}\n  cpp-anything diff {dir}\n  cpp-anything gate {dir}",
        dir = dir.display()
    );
    Ok(0)
}

fn project_arg(args: &Parsed) -> Result<Project, String> {
    let dir = args
        .positional
        .first()
        .ok_or("this command needs a project directory")?;
    Project::load(Path::new(dir))
}

fn record_command(args: &Parsed) -> Result<i32, String> {
    let project = project_arg(args)?;
    let cases = args.number("cases").unwrap_or(16) as usize;
    let seed = args.number("seed").unwrap_or(20260819);
    let inputs = args.value("inputs").map(PathBuf::from);
    let count = crate::python::record(&project, cases, seed, inputs.as_deref())?;
    let mut project = project;
    project.recorded = Some(crate::project::Recorded {
        cases: count,
        at: util::now_iso8601(),
    });
    project.save()?;
    println!(
        "cpp-anything record: {} cases for {} → {}/cases.jsonl",
        count,
        project.module,
        project.dir.display()
    );
    println!(
        "  next: write src/port.cpp, then `cpp-anything diff {}`",
        project.dir.display()
    );
    Ok(0)
}

fn build_command(args: &Parsed) -> Result<i32, String> {
    let project = project_arg(args)?;
    let outcome = crate::build::build(
        &project,
        args.flag("clean"),
        args.value("cxx"),
        args.number("jobs").unwrap_or_else(default_jobs) as usize,
    )?;
    println!(
        "cpp-anything build: {} ({})\n  {} warning(s)\n  next: `cpp-anything diff {}`",
        project.module,
        util::short_path(&outcome.driver),
        outcome.warnings,
        project.dir.display()
    );
    Ok(0)
}

fn default_jobs() -> u64 {
    std::thread::available_parallelism()
        .map(|value| value.get() as u64)
        .unwrap_or(4)
}

fn diff_command(args: &Parsed) -> Result<i32, String> {
    let project = project_arg(args)?;
    let driver = crate::build::driver_path(&project);
    if !driver.exists() {
        return Err(format!(
            "{} is not built yet: run `cpp-anything build {}` first",
            util::short_path(&driver),
            project.dir.display()
        ));
    }
    let options = crate::diff::Options {
        tolerance: args
            .value("tolerance")
            .and_then(|value| value.parse().ok())
            .unwrap_or(0.0),
        strict_errors: args.flag("strict-errors"),
        only: args.value("only").map(str::to_string),
        max_report: args.number("max-report").unwrap_or(8) as usize,
    };
    let report = crate::diff::run(&project, &driver, &options)?;
    if args.flag("json") {
        println!(
            "{}",
            crate::diff::to_json(&report, &project).to_string_pretty()
        );
    } else {
        print!("{}", crate::diff::render(&report, &project, &options));
    }
    Ok(if report.passed() { 0 } else { 1 })
}

fn bench_command(args: &Parsed) -> Result<i32, String> {
    let project = project_arg(args)?;
    let driver = crate::build::driver_path(&project);
    if !driver.exists() {
        return Err(format!(
            "{} is not built yet: run `cpp-anything build {}` first",
            util::short_path(&driver),
            project.dir.display()
        ));
    }
    let report = crate::bench::run(
        &project,
        &driver,
        args.number("repeat").unwrap_or(1000) as usize,
    )?;
    if args.flag("json") {
        println!(
            "{}",
            crate::bench::to_json(&report, &project).to_string_pretty()
        );
    } else {
        print!("{}", crate::bench::render(&report, &project));
    }
    Ok(0)
}

fn gate_command(args: &Parsed) -> Result<i32, String> {
    let project = project_arg(args)?;
    let options = crate::gate::Options {
        clean: args.flag("clean"),
        cxx: args.value("cxx").map(str::to_string),
        jobs: args.number("jobs").unwrap_or_else(default_jobs) as usize,
        relevance: args
            .flag("bench")
            .then(|| (args.number("repeat").unwrap_or(500) as usize, 1)),
    };
    let report = crate::gate::run(&project, &options)?;
    if args.flag("json") {
        println!(
            "{}",
            crate::gate::to_json(&report, &project).to_string_pretty()
        );
    } else {
        print!("{}", crate::gate::render(&report, &project));
    }
    Ok(if report.passed() { 0 } else { 1 })
}

fn refresh_command(args: &Parsed) -> Result<i32, String> {
    let project = project_arg(args)?;
    let written = crate::codegen::refresh(&project)?;
    println!(
        "cpp-anything refresh: {} regenerated, src/port.cpp untouched",
        written.join(", ")
    );
    Ok(0)
}
