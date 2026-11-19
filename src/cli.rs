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
    /// Everything after `--`, verbatim: workloads and test commands.
    pub trailing: Vec<String>,
    /// Repeated options (`--agent claude --agent codex`), in order.
    pub multi: BTreeMap<String, Vec<String>>,
}

impl Parsed {
    pub fn value(&self, name: &str) -> Option<&str> {
        self.values.get(name).map(String::as_str)
    }

    pub fn flag(&self, name: &str) -> bool {
        self.flags.contains(name)
    }

    pub fn many(&self, name: &str) -> Vec<String> {
        self.multi.get(name).cloned().unwrap_or_default()
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
        trailing: Vec::new(),
        multi: BTreeMap::new(),
    };
    let mut index = 0;
    while index < argv.len() {
        let arg = &argv[index];
        if arg == "--" {
            parsed.trailing = argv[index + 1..].to_vec();
            break;
        }
        if let Some(body) = arg.strip_prefix("--") {
            if let Some((key, value)) = body.split_once('=') {
                parsed.values.insert(key.to_string(), value.to_string());
                parsed
                    .multi
                    .entry(key.to_string())
                    .or_default()
                    .push(value.to_string());
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
                    | "offline"
            ) {
                parsed.flags.insert(body.to_string());
            } else {
                let next = argv.get(index + 1);
                match next {
                    Some(value) if !value.starts_with("--") => {
                        parsed.values.insert(body.to_string(), value.clone());
                        parsed
                            .multi
                            .entry(body.to_string())
                            .or_default()
                            .push(value.clone());
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
        "app" => crate::app::command(&args),
        "profile" => profile_command(&args),
        "analyze" => analyze_command(&args),
        "bridge" => bridge_command(&args),
        "record" => record_command(&args),
        "build" => build_command(&args),
        "diff" => diff_command(&args),
        "bench" => bench_command(&args),
        "gate" => gate_command(&args),
        "refresh" => refresh_command(&args),
        "shadow" => shadow_command(&args),
        "rsi" => crate::rsi::command(&args),
        "setup" => crate::setup::command(&args),
        "map" => map_command(&args),
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
  profile   what the workload actually costs, and which function to move first
  app       record a whole CLI, replay any build against it, and put a C++ front door in front
  analyze   the units in a repository and what porting each one costs
  bridge    generate the C++ project for one unit (port skeleton + driver)
  record    run the original on generated edge cases (and optionally its test suite) and save what it returns
  build     configure and build the C++ project with CMake
  diff      replay every recorded case through the port and compare
  bench     the original's per-call cost against the port's
  gate      build + diff + matrix + dispatch checks; done means this passes
  shadow    generate a drop-in module that runs the original and the port side by side
  refresh   regenerate the generated files after a signature change
  map       every unit, its port, its cases and last diff, and which originals changed
  setup     register the MCP server (and skill) with Claude Code, Codex, Cursor, Gemini CLI, OpenCode
  rsi       verify cpp-anything itself with its own loop
  doctor    check the tools a port needs
  mcp       run as an MCP server on stdio

Run `cpp-anything <command> --help` for one command's options.

  matrix features [--toolchain gcc] [--version 13] [--std c++23] [--kind core|library] [--json]
  matrix explain <feature|macro> [--json]
  matrix profile  [--toolchain gcc] [--version 13] [--std c++23] [-o FILE]
  matrix docs     [--out DIR] [--check]
  matrix sync     [--offline]
  profile <repo> [--json] [--top N] -- python3 bench.py
  app record  <repo> [--inputs FILE] [--normalize RULE] [--no-traps] [--timeout S] [-- python3 cli.py]
  app replay  <repo> [--build CMD] [--case NAME] [--json]
  app front-door <repo> [--no-check] [--json]
  app wire    <repo> <unit> --command NAME
  app lift    <repo>
  analyze <repo>  [-o plan.md] [--json] [--whole-app] [--lang python]
  bridge  <repo> <unit> [--out DIR] [--toolchain gcc] [--version 13] [--std c++23]
                        [--module-root DIR] [--force]
  record  <project> [--count N] [--seed N] [--inputs FILE] [--float-tolerance F]
                    [--from-tests -- pytest -q]
  diff    <project> [--json] [--strict-errors] [--tolerance F] [--only FN] [--max-report N]
  shadow  <project> [--report] [--check] [--json]
  gate    <project> [--json] [--clean] [--cxx PATH] [--jobs N] [--bench]
  map     [<repo>] [--crates DIR] [--write] [--check] [--json]
  setup   [--agent claude|codex|cursor|gemini|opencode] [--remove] [--dry-run] [--json]
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

fn check_lang(args: &Parsed) -> Result<(), String> {
    let Some(lang) = args.value("lang") else {
        return Ok(());
    };
    if lang == "python" || lang == "py" {
        Ok(())
    } else {
        Err(format!(
            "cpp-anything reads Python today; {lang:?} is a language plugin that does not exist yet \
             (analyze/bridge/record for C, TypeScript, Go, Ruby, Java are the next plugins)"
        ))
    }
}

fn profile_command(args: &Parsed) -> Result<i32, String> {
    let root = args.positional.first().map(String::as_str).unwrap_or(".");
    check_lang(args)?;
    let root = Path::new(root);
    let report = crate::profile::run(
        root,
        &args.trailing,
        args.number("top").unwrap_or(12) as usize,
    )?;
    if args.flag("json") {
        println!(
            "{}",
            crate::profile::to_json(&report, root).to_string_pretty()
        );
    } else {
        print!("{}", crate::profile::render(&report));
    }
    Ok(0)
}

fn map_command(args: &Parsed) -> Result<i32, String> {
    let repo = PathBuf::from(args.positional.first().map(String::as_str).unwrap_or("."));
    let extra: Vec<PathBuf> = args.many("crates").into_iter().map(PathBuf::from).collect();
    let map = crate::portmap::build(&repo, &extra)?;
    if args.flag("json") {
        println!("{}", map.to_json().to_string_pretty());
    } else {
        print!("{}", map.markdown());
    }
    if args.flag("write") {
        let path = map.root.join(crate::portmap::MAP_FILE);
        util::write(&path, &map.markdown())?;
        println!("Wrote {}", util::short_path(&path));
    }
    if args.flag("check") && map.changed() > 0 {
        return Ok(1);
    }
    Ok(0)
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
    let whole_app = args.flag("whole-app");
    let doc = analyze::analyze(repo)?;
    let json = analyze::to_json(&doc, repo, whole_app);
    if args.flag("json") {
        println!("{}", json.to_string_pretty());
        return Ok(0);
    }
    let plan = analyze::render_plan(&doc, repo, whole_app);
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
    let mut project = project_arg(args)?;
    let cases = args
        .number("count")
        .or_else(|| args.number("cases"))
        .unwrap_or(64) as usize;
    let seed = args.number("seed").unwrap_or(20260819);
    let inputs = args.value("inputs").map(PathBuf::from);
    let count = crate::python::record(&project, cases, seed, inputs.as_deref())?;
    if let Some(tolerance) = args.value("float-tolerance") {
        project.tolerance = tolerance
            .parse()
            .map_err(|_| format!("--float-tolerance {tolerance:?} is not a number"))?;
    }
    let mut suite = None;
    if args.flag("from-tests") {
        if args.trailing.is_empty() {
            return Err(
                "--from-tests needs the test command after `--`, e.g. `record . --from-tests -- pytest -q`"
                    .into(),
            );
        }
        suite = Some(crate::python::record_from_tests(&project, &args.trailing)?);
    }
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
    if let Some(suite) = suite {
        println!(
            "  from tests ({}): {} call(s) captured, {} new case(s), suite {}",
            suite.command.join(" "),
            suite.calls,
            suite.added,
            if suite.suite_ok {
                "passed".to_string()
            } else {
                format!("failed ({})", suite.suite_error)
            }
        );
        if let Some((reached, lines)) = suite.coverage {
            println!("  coverage: {reached} of {lines} source lines reached by the suite");
        }
        if !suite.suite_ok {
            println!(
                "  the cases the suite did reach were kept; fix the suite to capture the rest"
            );
        }
    }
    if project.tolerance > 0.0 {
        println!(
            "  float tolerance: {} (saved with the project; diff and gate use it)",
            project.tolerance
        );
    }
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

fn shadow_command(args: &Parsed) -> Result<i32, String> {
    let project = project_arg(args)?;
    if args.flag("report") || args.flag("check") {
        let report = crate::shadow::read_report(&project)?;
        if args.flag("json") {
            println!(
                "{}",
                crate::shadow::to_json(&report, &project).to_string_pretty()
            );
        } else {
            print!("{}", crate::shadow::render(&report, &project));
        }
        return Ok(if args.flag("check") && !report.clean() {
            1
        } else {
            0
        });
    }
    crate::codegen::refresh(&project)?;
    let outcome = crate::build::build(
        &project,
        false,
        args.value("cxx"),
        args.number("jobs").unwrap_or_else(default_jobs) as usize,
    )?;
    println!(
        "cpp-anything shadow: {} shadowed by {}",
        project.module, project.unit
    );
    println!(
        "  library: {}",
        util::short_path(&outcome.driver.with_file_name(library_name()))
    );
    println!(
        "  module:  {}",
        util::short_path(&project.dir.join("shadow").join(format!(
            "{}_shadow.py",
            crate::codegen::namespace_of(&project)
        )))
    );
    println!(
        "\nimport it where the original was imported:\n  sys.path.insert(0, \"{}\")\n  import {}_shadow as {}",
        project.dir.join("shadow").display(),
        crate::codegen::namespace_of(&project),
        crate::codegen::namespace_of(&project)
    );
    println!("\ncallers keep the original's results; disagreements go to shadow/{}.shadow.jsonl\n  cpp-anything shadow {} --report", crate::codegen::namespace_of(&project), project.dir.display());
    Ok(0)
}

fn library_name() -> &'static str {
    if cfg!(windows) {
        "cppan_shadow.dll"
    } else if cfg!(target_os = "macos") {
        "libcppan_shadow.dylib"
    } else {
        "libcppan_shadow.so"
    }
}

fn refresh_command(args: &Parsed) -> Result<i32, String> {
    let mut project = project_arg(args)?;
    // The Python signature is the contract: re-read it, then regenerate everything
    // that derives from it. `src/port.cpp` is never touched.
    let unit = crate::python::inventory(
        &project.source_file,
        &project.module,
        project.package_root.as_deref(),
    )?;
    project.functions = unit.functions;
    project.skipped = unit.skipped;
    project.risks = unit.risks;
    project.imports = unit.imports;
    project.lines = unit.lines;
    let written = crate::codegen::refresh(&project)?;
    project.save()?;
    println!(
        "cpp-anything refresh: {} function(s), {} file(s) regenerated, src/port.cpp untouched",
        project.functions.len(),
        written.len()
    );
    for function in &project.functions {
        println!("  · {}", function.signature_line());
    }
    Ok(0)
}
