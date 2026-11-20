//! `doctor`: the tools a port needs, whether they are there, and how to install them.

use std::path::Path;

use crate::cli::Parsed;
use crate::json::Json;
use crate::matrix::Matrix;
use crate::setup::{self, Agent};
use crate::util;

struct Check {
    name: &'static str,
    status: &'static str,
    required: bool,
    detail: String,
    fix: Option<&'static str>,
}

pub fn command(args: &Parsed) -> Result<i32, String> {
    let mut checks = Vec::new();

    checks.push(program_check(
        "cmake",
        "cmake",
        Some("3.28.0"),
        true,
        &["--version"],
    ));
    checks.push(Check {
        name: "c++ compiler",
        status: if util::which("c++").is_some()
            || util::which("g++").is_some()
            || util::which("clang++").is_some()
        {
            "ok"
        } else {
            "missing"
        },
        required: true,
        detail: ["c++", "g++", "clang++", "cl"]
            .iter()
            .find(|name| util::which(name).is_some())
            .map(|name| format!("{name} on PATH"))
            .unwrap_or_else(|| "no c++, g++, clang++ or cl on PATH".into()),
        fix: Some("apt install g++ · xcode-select --install · install Visual Studio Build Tools"),
    });

    let ninja = crate::modules::ninja();
    checks.push(Check {
        name: "ninja",
        status: if ninja.is_some() { "ok" } else { "missing" },
        required: false,
        detail: match &ninja {
            Some(path) => path.display().to_string(),
            None => "only needed to build projects generated with --modules (gcc/clang)".into(),
        },
        fix: Some(
            "python3 -m pip install --user ninja · apt install ninja-build · brew install ninja",
        ),
    });

    let python = crate::python::available();
    checks.push(Check {
        name: "python3",
        status: if python.is_some() { "ok" } else { "missing" },
        required: true,
        detail: match &python {
            Some(version) => format!("{} ({version})", crate::python::interpreter()),
            None => format!("{} not found", crate::python::interpreter()),
        },
        fix: Some("set CPPAN_PYTHON=/path/to/python3 if it is installed under another name"),
    });

    let matrix = Matrix::get();
    checks.push(Check {
        name: "matrix",
        status: if matrix.features.len() > 100 {
            "ok"
        } else {
            "outdated"
        },
        required: true,
        detail: format!(
            "{} features over {} standards, generated {}",
            matrix.features.len(),
            matrix.standards.len(),
            matrix.generated_at
        ),
        fix: Some("cpp-anything matrix sync, then rebuild"),
    });

    let guides = crate::docs::check(Path::new("docs/guides")).unwrap_or_default();
    checks.push(Check {
        name: "guides",
        status: if guides.is_empty() { "ok" } else { "outdated" },
        required: true,
        detail: if guides.is_empty() {
            "docs/guides matches data/matrix.json".into()
        } else {
            format!("{} drifted: {}", guides.len(), guides.join(", "))
        },
        fix: Some("cpp-anything matrix docs"),
    });

    checks.push(mcp_check());

    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(std::path::PathBuf::from);
    let mut any_agent = false;
    if let Some(home) = &home {
        for agent in Agent::ALL {
            if !agent_present(agent, home) {
                continue;
            }
            any_agent = true;
            checks.push(agent_check(agent, home));
        }
    }
    if !any_agent {
        checks.push(Check {
            name: "agents",
            status: "ok",
            required: false,
            detail: "no coding agent found (Claude Code, Codex, Cursor, Gemini CLI, OpenCode); the CLI works without one".into(),
            fix: Some("cpp-anything setup --agent claude"),
        });
    }

    let ok = checks
        .iter()
        .all(|check| !check.required || check.status == "ok");
    if args.flag("json") {
        let doc = Json::obj([
            ("schema", Json::str("cpp-anything/doctor@1")),
            ("ok", Json::Bool(ok)),
            (
                "checks",
                Json::Array(
                    checks
                        .iter()
                        .map(|check| {
                            Json::obj([
                                ("name", Json::str(check.name)),
                                ("status", Json::str(check.status)),
                                ("required", Json::Bool(check.required)),
                                ("detail", Json::str(check.detail.clone())),
                                ("fix", check.fix.map(Json::str).unwrap_or(Json::Null)),
                            ])
                        })
                        .collect(),
                ),
            ),
        ]);
        println!("{}", doc.to_string_pretty());
    } else {
        let width = checks
            .iter()
            .map(|check| check.name.len())
            .max()
            .unwrap_or(10);
        println!("cpp-anything doctor\n");
        for check in &checks {
            println!(
                "  {:<9} {:<width$}  {}{}",
                check.status,
                check.name,
                check.detail,
                if check.required || check.status == "ok" {
                    ""
                } else {
                    " (optional)"
                },
                width = width + 2
            );
            if check.status != "ok" {
                if let Some(fix) = check.fix {
                    println!(
                        "  {:<9} {:<width$}  fix: {}",
                        "",
                        "",
                        fix,
                        width = width + 2
                    );
                }
            }
        }
        println!(
            "\n{}",
            if ok {
                "Everything required is in place. Language runtimes for other source languages are optional: each matters only for the languages you port."
            } else {
                "Something required is missing: cmake, a C++ compiler, python3, the matrix, the guides or the MCP server. Run the fix commands above."
            }
        );
    }
    Ok(if ok { 0 } else { 1 })
}

fn program_check(
    name: &'static str,
    program: &'static str,
    minimum: Option<&str>,
    required: bool,
    version_args: &[&str],
) -> Check {
    let Some(path) = util::which(program) else {
        return Check {
            name,
            status: "missing",
            required,
            detail: format!("{program} not found or did not run"),
            fix: Some(match program {
                "cmake" => "apt install cmake · brew install cmake · winget install Kitware.CMake",
                _ => "install the tool and put it on PATH",
            }),
        };
    };
    let args: Vec<String> = version_args.iter().map(|arg| (*arg).to_string()).collect();
    let run = util::run(&path.display().to_string(), &args, None);
    let banner = run
        .stdout
        .lines()
        .chain(run.stderr.lines())
        .next()
        .unwrap_or("")
        .to_string();
    if let Some(minimum) = minimum {
        let version = first_version(&banner);
        if let Some(version) = version {
            if !util::version_at_least(&version, minimum) {
                return Check {
                    name,
                    status: "outdated",
                    required,
                    detail: format!("{banner}; cpp-anything needs {minimum} or newer"),
                    fix: Some(
                        "apt install cmake · brew upgrade cmake · winget upgrade Kitware.CMake",
                    ),
                };
            }
        }
    }
    Check {
        name,
        status: "ok",
        required,
        detail: if banner.is_empty() {
            format!("{program} on PATH")
        } else {
            banner
        },
        fix: None,
    }
}

fn first_version(banner: &str) -> Option<String> {
    let mut current = String::new();
    for ch in banner.chars() {
        if ch.is_ascii_digit() || (ch == '.' && !current.is_empty()) {
            current.push(ch);
        } else if !current.is_empty() {
            if current.matches('.').count() >= 1 {
                return Some(current.trim_end_matches('.').to_string());
            }
            current.clear();
        }
    }
    (!current.is_empty()).then_some(current.trim_end_matches('.').to_string())
}

/// Spawns this binary's MCP server and asks it the two questions a client asks first.
fn mcp_check() -> Check {
    let Ok(executable) = std::env::current_exe() else {
        return Check {
            name: "mcp server",
            status: "missing",
            required: true,
            detail: "cannot find the running executable".into(),
            fix: Some("reinstall cpp-anything"),
        };
    };
    let request = Json::obj([
        ("jsonrpc", Json::str("2.0")),
        ("id", Json::Int(1)),
        ("method", Json::str("initialize")),
        (
            "params",
            Json::obj([(
                "clientInfo",
                Json::obj([("name", Json::str("cpp-anything-doctor"))]),
            )]),
        ),
    ]);
    let list = Json::obj([
        ("jsonrpc", Json::str("2.0")),
        ("id", Json::Int(2)),
        ("method", Json::str("tools/list")),
    ]);
    let mut child = match std::process::Command::new(&executable)
        .arg("mcp")
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
    {
        Ok(child) => child,
        Err(error) => {
            return Check {
                name: "mcp server",
                status: "missing",
                required: true,
                detail: format!("`{} mcp` could not start: {error}", executable.display()),
                fix: Some("run `cpp-anything mcp` by hand and look at what it prints"),
            };
        }
    };
    {
        use std::io::Write;
        if let Some(stdin) = child.stdin.as_mut() {
            let _ = writeln!(stdin, "{}", request.to_string_compact());
            let _ = writeln!(stdin, "{}", list.to_string_compact());
        }
    }
    let output = child.wait_with_output();
    match output {
        Ok(output) => {
            let text = String::from_utf8_lossy(&output.stdout);
            let tools = text
                .lines()
                .filter_map(|line| Json::parse(line).ok())
                .find_map(|doc| {
                    doc.get("result")
                        .and_then(|result| result.get("tools"))
                        .and_then(Json::as_array)
                        .map(<[Json]>::len)
                });
            match tools {
                Some(count) if count > 0 => Check {
                    name: "mcp server",
                    status: "ok",
                    required: true,
                    detail: format!("`{} mcp` answered with {count} tools", executable.display()),
                    fix: None,
                },
                _ => Check {
                    name: "mcp server",
                    status: "missing",
                    required: true,
                    detail: format!(
                        "`{} mcp` did not list tools: {}",
                        executable.display(),
                        util::truncate_text(text.trim(), 160)
                    ),
                    fix: Some("run `cpp-anything mcp` by hand and look at what it prints"),
                },
            }
        }
        Err(error) => Check {
            name: "mcp server",
            status: "missing",
            required: true,
            detail: format!("`{} mcp`: {error}", executable.display()),
            fix: Some("run `cpp-anything mcp` by hand and look at what it prints"),
        },
    }
}

fn agent_present(agent: Agent, home: &Path) -> bool {
    home.join(agent.marker_dir()).exists() || util::which(agent.command()).is_some()
}

fn agent_check(agent: Agent, home: &Path) -> Check {
    let config = agent.config_file(home);
    let executable = std::env::current_exe()
        .ok()
        .and_then(|path| path.canonicalize().ok())
        .map(|path| path.display().to_string())
        .unwrap_or_default();
    let text = util::read(&config).ok();
    let entry = text.as_deref().and_then(|text| {
        let doc = Json::parse(text).ok()?;
        let path: &[&str] = match agent {
            Agent::OpenCode => {
                if setup::json_get(&doc, &["mcp", "servers", setup::SERVER_KEY]).is_some() {
                    &["mcp", "servers", setup::SERVER_KEY]
                } else {
                    &["mcp", setup::SERVER_KEY]
                }
            }
            _ => &["mcpServers", setup::SERVER_KEY],
        };
        setup::json_get(&doc, path).cloned()
    });
    let (status, detail) = match &entry {
        None => (
            "missing",
            format!("no `{}` server in {}", setup::SERVER_KEY, config.display()),
        ),
        Some(entry) => {
            let command_ok = match agent {
                Agent::OpenCode => entry
                    .get("command")
                    .and_then(Json::as_array)
                    .map(|items| {
                        items.first().and_then(Json::as_str) == Some(executable.as_str())
                            && items.get(1).and_then(Json::as_str) == Some("mcp")
                    })
                    .unwrap_or(false),
                _ => {
                    entry.get("command").and_then(Json::as_str) == Some(executable.as_str())
                        && entry
                            .get("args")
                            .and_then(Json::as_array)
                            .map(|items| items.len() == 1 && items[0].as_str() == Some("mcp"))
                            .unwrap_or(false)
                }
            };
            if command_ok {
                (
                    "ok",
                    format!("{} runs this binary's `mcp`", config.display()),
                )
            } else {
                (
                    "outdated",
                    format!("{} runs a different command", config.display()),
                )
            }
        }
    };
    Check {
        name: "agents",
        status,
        required: true,
        detail: format!("{}: {detail}", agent.title()),
        fix: Some("cpp-anything setup"),
    }
}
