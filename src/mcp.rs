//! `mcp`: the same steps as MCP tools over stdio, so an agent can drive the loop.
//!
//! Hand-written JSON-RPC: the stdio transport needs `initialize`, `tools/list`,
//! `prompts/list`, `prompts/get`, `tools/call` and `ping`, and every tool is the CLI
//! itself in a subprocess, so an MCP call and a terminal call can never drift apart.

use std::io::{BufRead, Write};

use crate::json::Json;
use crate::util;

const PROTOCOL: &str = "2025-06-18";

struct Tool {
    name: &'static str,
    title: &'static str,
    description: &'static str,
    schema: &'static str,
    read_only: bool,
    /// Whether the tool runs the original's code or commands.
    runs_original: bool,
}

const TOOLS: &[Tool] = &[
    Tool {
        name: "analyze",
        title: "Analyze a repository",
        description: "Plan a port: the modules in a repository, their functions, the risks each one carries, and where to start.",
        schema: r#"{"type":"object","properties":{"root":{"type":"string"},"lang":{"type":"string","enum":["python"]},"whole_app":{"type":"boolean"},"json":{"type":"boolean"}},"required":["root"]}"#,
        read_only: true,
        runs_original: false,
    },
    Tool {
        name: "bridge",
        title: "Generate the C++ project for one unit",
        description: "Write the port skeleton, dispatch table, JSON driver, CMake project and profile header for one unit.",
        schema: r#"{"type":"object","properties":{"root":{"type":"string"},"unit":{"type":"string"},"out":{"type":"string"},"toolchain":{"type":"string"},"version":{"type":"string"},"std":{"type":"string"},"force":{"type":"boolean"},"module_root":{"type":"string"}},"required":["root","unit"]}"#,
        read_only: false,
        runs_original: false,
    },
    Tool {
        name: "refresh",
        title: "Regenerate the generated files",
        description: "Re-read the original's signatures and regenerate everything except src/port.cpp.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"}},"required":["project"]}"#,
        read_only: false,
        runs_original: false,
    },
    Tool {
        name: "record",
        title: "Record the original's behaviour",
        description: "Run the original on generated edge cases (and optionally its own test suite) and save what it returns, raises and writes.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"count":{"type":"number"},"seed":{"type":"number"},"inputs":{"type":"string"},"float_tolerance":{"type":"number"},"from_tests":{"type":"boolean"},"test_command":{"type":"array","items":{"type":"string"}}},"required":["project"]}"#,
        read_only: false,
        runs_original: true,
    },
    Tool {
        name: "build",
        title: "Build the C++ project",
        description: "Configure and build the generated project with CMake.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"clean":{"type":"boolean"},"cxx":{"type":"string"},"jobs":{"type":"number"}},"required":["project"]}"#,
        read_only: false,
        runs_original: false,
    },
    Tool {
        name: "diff",
        title: "Compare the port with the recording",
        description: "Replay every recorded case through the C++ port and compare; reports args, expected, got and why.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"json":{"type":"boolean"},"strict_errors":{"type":"boolean"},"tolerance":{"type":"number"},"only":{"type":"string"},"max_report":{"type":"number"}},"required":["project"]}"#,
        read_only: true,
        runs_original: false,
    },
    Tool {
        name: "profile",
        title: "Profile a workload",
        description: "Run a workload under the language's profiler and rank what to move: hot functions, callers, and batch advice.",
        schema: r#"{"type":"object","properties":{"root":{"type":"string"},"command":{"type":"array","items":{"type":"string"}},"json":{"type":"boolean"},"top":{"type":"number"}},"required":["root","command"]}"#,
        read_only: true,
        runs_original: true,
    },
    Tool {
        name: "bench",
        title: "Benchmark the port",
        description: "Time the recorded cases on the original and on the port, and say whether it is worth shipping.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"repeat":{"type":"number"},"json":{"type":"boolean"}},"required":["project"]}"#,
        read_only: true,
        runs_original: true,
    },
    Tool {
        name: "shadow",
        title: "Run the port beside the original",
        description: "Generate the drop-in shadow module: the original returns, the port runs second, disagreements are logged.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"report":{"type":"boolean"},"check":{"type":"boolean"},"json":{"type":"boolean"}},"required":["project"]}"#,
        read_only: false,
        runs_original: false,
    },
    Tool {
        name: "map",
        title: "Map the port",
        description: "Every unit, its project, cases and last diff, what is still only in Python, and which originals changed since they were recorded.",
        schema: r#"{"type":"object","properties":{"repo":{"type":"string"},"crates":{"type":"array","items":{"type":"string"}},"write":{"type":"boolean"},"check":{"type":"boolean"},"json":{"type":"boolean"}},"required":["repo"]}"#,
        read_only: true,
        runs_original: false,
    },
    Tool {
        name: "gate",
        title: "Check the definition of done",
        description: "Matrix, no stubs, error paths, build, dispatch and every recorded case; optional bench.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"json":{"type":"boolean"},"clean":{"type":"boolean"},"bench":{"type":"boolean"}},"required":["project"]}"#,
        read_only: false,
        runs_original: true,
    },
    Tool {
        name: "matrix_features",
        title: "What a compiler supports",
        description: "Supported, partial and missing features of one C++ standard for a compiler release, from cppreference's tables.",
        schema: r#"{"type":"object","properties":{"toolchain":{"type":"string"},"version":{"type":"string"},"std":{"type":"string"},"kind":{"type":"string","enum":["all","core","library"]}},"required":["toolchain","version","std"]}"#,
        read_only: true,
        runs_original: false,
    },
    Tool {
        name: "matrix_explain",
        title: "Explain one feature",
        description: "One feature by name or feature-test macro: first release per compiler, notes, paper.",
        schema: r#"{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}"#,
        read_only: true,
        runs_original: false,
    },
    Tool {
        name: "matrix_profile",
        title: "Generate profile.hpp",
        description: "The CPPAN_HAS_* header for a target toolchain, computed from the matrix.",
        schema: r#"{"type":"object","properties":{"toolchain":{"type":"string"},"version":{"type":"string"},"std":{"type":"string"}},"required":["toolchain","version","std"]}"#,
        read_only: true,
        runs_original: false,
    },
    Tool {
        name: "app_record",
        title: "Record a whole CLI",
        description: "Run the original CLI on its documented examples, your inputs and semantic traps, twice each, and store what it did.",
        schema: r#"{"type":"object","properties":{"repo":{"type":"string"},"run":{"type":"array","items":{"type":"string"}},"inputs":{"type":"string"},"normalize":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"timeout":{"type":"number"},"traps":{"type":"boolean"},"float_tolerance":{"type":"number"},"json":{"type":"boolean"}},"required":["repo"]}"#,
        read_only: false,
        runs_original: true,
    },
    Tool {
        name: "app_replay",
        title: "Replay a build against the app cases",
        description: "Run any build (the original, the front door, the final binary) against the recorded cases and report per-case mismatches.",
        schema: r#"{"type":"object","properties":{"repo":{"type":"string"},"build":{"type":"string"},"timeout":{"type":"number"},"case":{"type":"string"},"json":{"type":"boolean"}},"required":["repo"]}"#,
        read_only: true,
        runs_original: true,
    },
    Tool {
        name: "app_front_door",
        title: "Scaffold the C++ front door",
        description: "Write and build cpp/app: the binary that owns the entry point and falls back to the original for unwired commands.",
        schema: r#"{"type":"object","properties":{"repo":{"type":"string"},"no_check":{"type":"boolean"},"json":{"type":"boolean"}},"required":["repo"]}"#,
        read_only: false,
        runs_original: true,
    },
    Tool {
        name: "app_wire",
        title: "Wire a unit into the front door",
        description: "Route one command to a ported unit: adds the route, the glue stub, the kill switch and the library link.",
        schema: r#"{"type":"object","properties":{"repo":{"type":"string"},"unit":{"type":"string"},"command":{"type":"string"},"project":{"type":"string"}},"required":["repo","unit","command"]}"#,
        read_only: false,
        runs_original: false,
    },
    Tool {
        name: "app_lift",
        title: "Lift the app",
        description: "Refuse while any case still falls back; mark the app standalone when every case is answered by the front door.",
        schema: r#"{"type":"object","properties":{"repo":{"type":"string"},"json":{"type":"boolean"}},"required":["repo"]}"#,
        read_only: false,
        runs_original: true,
    },
    Tool {
        name: "setup",
        title: "Register with the coding agents",
        description: "Write the MCP server entry (and the skill where the agent reads skills) for the agents found on this machine.",
        schema: r#"{"type":"object","properties":{"agent":{"type":"array","items":{"type":"string"}},"remove":{"type":"boolean"},"dry_run":{"type":"boolean"},"json":{"type":"boolean"}}}"#,
        read_only: false,
        runs_original: false,
    },
    Tool {
        name: "doctor",
        title: "Check the tools",
        description: "cmake, a C++ compiler, python3, the matrix, the guides, this MCP server and the agent entries, with the fix for each.",
        schema: r#"{"type":"object","properties":{"json":{"type":"boolean"}}}"#,
        read_only: true,
        runs_original: false,
    },
];

pub fn serve() -> Result<i32, String> {
    let stdin = std::io::stdin();
    let mut stdout = std::io::stdout();
    for line in stdin.lock().lines() {
        let line = line.map_err(|error| format!("stdin: {error}"))?;
        if line.trim().is_empty() {
            continue;
        }
        let request = match Json::parse(&line) {
            Ok(request) => request,
            Err(error) => {
                let _ = writeln!(
                    stdout,
                    "{}",
                    error_response(Json::Null, -32700, &format!("parse error: {error}"))
                );
                continue;
            }
        };
        let id = request.get("id").cloned().unwrap_or(Json::Null);
        let method = request
            .get("method")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let params = request
            .get("params")
            .cloned()
            .unwrap_or(Json::Object(Vec::new()));
        if id.is_null() {
            continue; // notifications need no answer
        }
        let progress = params
            .get("_meta")
            .and_then(|meta| meta.get("progressToken"))
            .cloned();
        if let Some(token) = &progress {
            progress_note(&mut stdout, token, 0, &format!("running {method}"));
        }
        let response = match method.as_str() {
            "initialize" => {
                let requested = params
                    .get("protocolVersion")
                    .and_then(Json::as_str)
                    .unwrap_or(PROTOCOL);
                result_response(
                    id,
                    Json::obj([
                        ("protocolVersion", Json::str(requested)),
                        (
                            "capabilities",
                            Json::obj([("tools", Json::Object(Vec::new())), ("prompts", Json::Object(Vec::new()))]),
                        ),
                        (
                            "serverInfo",
                            Json::obj([
                                ("name", Json::str("cpp-anything")),
                                ("version", Json::str(env!("CARGO_PKG_VERSION"))),
                            ]),
                        ),
                        (
                            "instructions",
                            Json::str(
                                "Port one unit at a time: analyze, bridge, record, write src/port.cpp, \
                                 diff, gate. `matrix` answers what the target toolchain supports; \
                                 `app` records a whole CLI and puts a C++ front door in front of it. \
                                 Never mark a port done before gate passes.",
                            ),
                        ),
                    ]),
                )
            }
            "ping" => result_response(id, Json::Object(Vec::new())),
            "tools/list" => result_response(id, tools_list()),
            "prompts/list" => result_response(id, prompts_list()),
            "prompts/get" => match prompts_get(&params) {
                Ok(result) => result_response(id, result),
                Err(error) => error_response(id, -32602, &error),
            },
            "tools/call" => match call_tool(&mut stdout, &params) {
                Ok(text) => result_response(
                    id,
                    Json::obj([
                        (
                            "content",
                            Json::Array(vec![Json::obj([
                                ("type", Json::str("text")),
                                ("text", Json::str(text.clone())),
                            ])]),
                        ),
                        ("isError", Json::Bool(false)),
                    ]),
                ),
                Err((text, structured)) => result_response(
                    id,
                    Json::obj([
                        (
                            "content",
                            Json::Array(vec![Json::obj([
                                ("type", Json::str("text")),
                                ("text", Json::str(text.clone())),
                            ])]),
                        ),
                        ("structuredContent", structured),
                        ("isError", Json::Bool(true)),
                    ]),
                ),
            },
            other => error_response(id, -32601, &format!("unknown method `{other}`")),
        };
        if let Some(token) = &progress {
            progress_note(&mut stdout, token, 1, "finished");
        }
        let _ = writeln!(stdout, "{}", response.to_string_compact());
        let _ = stdout.flush();
    }
    Ok(0)
}

fn progress_note(stdout: &mut std::io::Stdout, token: &Json, progress: i64, message: &str) {
    let note = Json::obj([
        ("jsonrpc", Json::str("2.0")),
        ("method", Json::str("notifications/progress")),
        (
            "params",
            Json::obj([
                ("progressToken", token.clone()),
                ("progress", Json::Int(progress)),
                ("total", Json::Int(1)),
                ("message", Json::str(message.to_string())),
            ]),
        ),
    ]);
    let _ = writeln!(stdout, "{}", note.to_string_compact());
    let _ = stdout.flush();
}

fn tools_list() -> Json {
    Json::obj([(
        "tools",
        Json::Array(
            TOOLS
                .iter()
                .map(|tool| {
                    Json::obj([
                        ("name", Json::str(tool.name)),
                        ("title", Json::str(tool.title)),
                        ("description", Json::str(tool.description)),
                        (
                            "inputSchema",
                            Json::parse(tool.schema).unwrap_or_else(|_| Json::Object(Vec::new())),
                        ),
                        (
                            "annotations",
                            Json::obj([
                                ("title", Json::str(tool.title)),
                                ("readOnlyHint", Json::Bool(tool.read_only)),
                                ("destructiveHint", Json::Bool(false)),
                                ("idempotentHint", Json::Bool(!tool.runs_original)),
                                ("openWorldHint", Json::Bool(tool.runs_original)),
                            ]),
                        ),
                    ])
                })
                .collect(),
        ),
    )])
}

fn prompts_list() -> Json {
    Json::obj([(
        "prompts",
        Json::Array(vec![
            Json::obj([
                ("name", Json::str("port_unit")),
                ("title", Json::str("Port one unit to C++")),
                ("description", Json::str("Port one unit of a repository to C++ with cpp-anything, and prove it.")),
                (
                    "arguments",
                    Json::Array(vec![
                        Json::obj([
                            ("name", Json::str("path")),
                            ("description", Json::str("Repository root.")),
                            ("required", Json::Bool(true)),
                        ]),
                        Json::obj([
                            ("name", Json::str("unit")),
                            ("description", Json::str("Unit id from analyze; the best first port when omitted.")),
                            ("required", Json::Bool(false)),
                        ]),
                    ]),
                ),
            ]),
            Json::obj([
                ("name", Json::str("port_whole_app")),
                ("title", Json::str("Port a whole app to C++")),
                ("description", Json::str("Record the app, put a C++ front door in front of it, and walk it to standalone.")),
                (
                    "arguments",
                    Json::Array(vec![Json::obj([
                        ("name", Json::str("path")),
                        ("description", Json::str("Repository root.")),
                        ("required", Json::Bool(true)),
                    ])]),
                ),
            ]),
        ]),
    )])
}

fn prompts_get(params: &Json) -> Result<Json, String> {
    let name = params.get("name").and_then(Json::as_str).unwrap_or("");
    let arguments = params
        .get("arguments")
        .cloned()
        .unwrap_or(Json::Object(Vec::new()));
    let path = arguments
        .get("path")
        .and_then(Json::as_str)
        .ok_or_else(|| format!("prompt `{name}` needs the `path` argument"))?;
    let skill = skill_body();
    let (title, text) = match name {
        "port_unit" => {
            let unit = arguments.get("unit").and_then(Json::as_str);
            let lead = match unit {
                Some(unit) => format!(
                    "Port the unit `{unit}` of the repository at {path} to C++ with cpp-anything. Call \
                     the port done only when gate passes."
                ),
                None => format!(
                    "Port one unit of the repository at {path} to C++ with cpp-anything: run \
                     `analyze` on it first and take the best first port from `start_here`. Call the \
                     port done only when gate passes."
                ),
            };
            ("Port one unit to C++", format!("{lead}\n\nFollow this guide:\n\n{skill}"))
        }
        "port_whole_app" => (
            "Port a whole app to C++",
            format!(
                "Rewrite the app at {path} as a C++ front door that answers every recorded case, \
                 keeping the original only as reference.\n\nWork in this order: `app record`, `app \
                 replay` (the original against its own cases), `app front-door`, then one unit at a \
                 time through the per-unit loop, `app wire` for each command it answers, and `app \
                 lift` when nothing falls back.\n\nFollow this guide:\n\n{skill}"
            ),
        ),
        other => return Err(format!("unknown prompt `{other}`")),
    };
    Ok(Json::obj([
        ("description", Json::str(title)),
        (
            "messages",
            Json::Array(vec![Json::obj([
                ("role", Json::str("user")),
                (
                    "content",
                    Json::obj([("type", Json::str("text")), ("text", Json::str(text))]),
                ),
            ])]),
        ),
    ]))
}

/// The skill without its YAML front matter: what a prompt should send.
fn skill_body() -> String {
    let text = include_str!("../skills/cpp-anything/SKILL.md");
    let body = match text.strip_prefix("---") {
        Some(rest) => match rest.split_once("\n---\n") {
            Some((_, body)) => body,
            None => rest,
        },
        None => text,
    };
    body.trim().to_string()
}

fn call_tool(stdout: &mut std::io::Stdout, params: &Json) -> Result<String, (String, Json)> {
    let name = params.get("name").and_then(Json::as_str).ok_or_else(|| {
        (
            "tools/call needs a name".to_string(),
            failure("invalid_arguments", "tools/call needs a name", None),
        )
    })?;
    let arguments = params
        .get("arguments")
        .cloned()
        .unwrap_or(Json::Object(Vec::new()));
    let spec = TOOLS.iter().find(|tool| tool.name == name);
    let Some(_spec) = spec else {
        let text = format!("unknown tool `{name}`");
        return Err((text.clone(), failure("unknown_tool", &text, None)));
    };
    let executable = std::env::current_exe().map_err(|error| {
        (
            "current_exe failed".to_string(),
            failure("failed", &error.to_string(), None),
        )
    })?;
    let argv = match build_argv(name, &arguments, &executable) {
        Ok(argv) => argv,
        Err(text) => return Err((text.clone(), failure("invalid_arguments", &text, None))),
    };
    let _ = stdout.flush();
    let run = util::run(&executable.display().to_string(), &argv, None);
    if run.ok {
        return Ok(run.stdout);
    }
    let text = format!(
        "cpp-anything {} failed: {}\n{}",
        argv.join(" "),
        run.failure(),
        run.stderr
    );
    let code = classify(&text, &arguments);
    Err((
        text.clone(),
        failure(code, &text, next_hint(code, &arguments)),
    ))
}

fn classify(text: &str, _arguments: &Json) -> &'static str {
    if text.contains("no recorded cases")
        || text.contains("app record") && text.contains("no cases")
    {
        "no_recording"
    } else if text.contains("changed since it was recorded") {
        "stale_recording"
    } else if text.contains("is not built yet") || text.contains("build failed") {
        "build_failed"
    } else {
        "failed"
    }
}

fn next_hint(code: &str, arguments: &Json) -> Option<String> {
    let path = ["project", "repo", "root"]
        .iter()
        .find_map(|key| arguments.get(key).and_then(Json::as_str))
        .unwrap_or("<project>");
    match code {
        "no_recording" => Some(format!("cpp-anything record {path}")),
        "stale_recording" => Some(format!("cpp-anything record {path}")),
        "build_failed" => Some(format!("cpp-anything build {path}")),
        _ => None,
    }
}

fn failure(code: &str, message: &str, next: Option<String>) -> Json {
    Json::obj([
        ("code", Json::str(code)),
        ("message", Json::str(message)),
        ("retryable", Json::Bool(false)),
        ("next", next.map(Json::str).unwrap_or(Json::Null)),
    ])
}

fn arg_str(arguments: &Json, key: &str) -> Option<String> {
    arguments
        .get(key)
        .and_then(Json::as_str)
        .map(str::to_string)
}

fn required(arguments: &Json, key: &str) -> Result<String, String> {
    arg_str(arguments, key).ok_or_else(|| format!("missing `{key}`"))
}

fn push_flag(argv: &mut Vec<String>, arguments: &Json, flag: &str, key: &str) {
    if let Some(value) = arguments.get(key) {
        match value {
            Json::Bool(true) => argv.push(format!("--{flag}")),
            Json::Str(text) => {
                argv.push(format!("--{flag}"));
                argv.push(text.clone());
            }
            Json::Int(number) => {
                argv.push(format!("--{flag}"));
                argv.push(number.to_string());
            }
            Json::Real(number) => {
                argv.push(format!("--{flag}"));
                argv.push(crate::json::format_real(*number));
            }
            _ => {}
        }
    }
}

fn push_repeated(argv: &mut Vec<String>, arguments: &Json, flag: &str, key: &str) {
    for item in arguments
        .get(key)
        .and_then(Json::as_array)
        .unwrap_or_default()
    {
        if let Some(text) = item.as_str() {
            argv.push(format!("--{flag}"));
            argv.push(text.to_string());
        }
    }
}

fn build_argv(
    name: &str,
    arguments: &Json,
    executable: &std::path::Path,
) -> Result<Vec<String>, String> {
    let _ = executable;
    let mut argv: Vec<String> = Vec::new();
    match name {
        "analyze" => {
            argv.push("analyze".into());
            argv.push(required(arguments, "root")?);
            push_flag(&mut argv, arguments, "lang", "lang");
            push_flag(&mut argv, arguments, "whole-app", "whole_app");
            argv.push("--json".into());
        }
        "bridge" => {
            argv.push("bridge".into());
            argv.push(required(arguments, "root")?);
            argv.push(required(arguments, "unit")?);
            for (flag, key) in [
                ("out", "out"),
                ("toolchain", "toolchain"),
                ("version", "version"),
                ("std", "std"),
                ("force", "force"),
                ("module-root", "module_root"),
            ] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "refresh" => {
            argv.push("refresh".into());
            argv.push(required(arguments, "project")?);
        }
        "record" => {
            argv.push("record".into());
            argv.push(required(arguments, "project")?);
            for (flag, key) in [
                ("count", "count"),
                ("seed", "seed"),
                ("inputs", "inputs"),
                ("float-tolerance", "float_tolerance"),
                ("from-tests", "from_tests"),
            ] {
                push_flag(&mut argv, arguments, flag, key);
            }
            let tests = arguments
                .get("test_command")
                .and_then(Json::as_array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(|item| item.as_str().map(str::to_string))
                        .collect::<Vec<_>>()
                })
                .unwrap_or_default();
            if !tests.is_empty() {
                argv.push("--".into());
                argv.extend(tests);
            }
        }
        "build" => {
            argv.push("build".into());
            argv.push(required(arguments, "project")?);
            for (flag, key) in [("clean", "clean"), ("cxx", "cxx"), ("jobs", "jobs")] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "diff" => {
            argv.push("diff".into());
            argv.push(required(arguments, "project")?);
            for (flag, key) in [
                ("json", "json"),
                ("strict-errors", "strict_errors"),
                ("tolerance", "tolerance"),
                ("only", "only"),
                ("max-report", "max_report"),
            ] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "profile" => {
            argv.push("profile".into());
            argv.push(required(arguments, "root")?);
            push_flag(&mut argv, arguments, "json", "json");
            push_flag(&mut argv, arguments, "top", "top");
            let command = arguments
                .get("command")
                .and_then(Json::as_array)
                .ok_or("missing `command`")?
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect::<Vec<_>>();
            if command.is_empty() {
                return Err("missing `command`".into());
            }
            argv.push("--".into());
            argv.extend(command);
        }
        "bench" => {
            argv.push("bench".into());
            argv.push(required(arguments, "project")?);
            for (flag, key) in [("repeat", "repeat"), ("json", "json")] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "shadow" => {
            argv.push("shadow".into());
            argv.push(required(arguments, "project")?);
            for (flag, key) in [("report", "report"), ("check", "check"), ("json", "json")] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "map" => {
            argv.push("map".into());
            argv.push(required(arguments, "repo")?);
            push_repeated(&mut argv, arguments, "crates", "crates");
            for (flag, key) in [("write", "write"), ("check", "check"), ("json", "json")] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "gate" => {
            argv.push("gate".into());
            argv.push(required(arguments, "project")?);
            for (flag, key) in [("json", "json"), ("clean", "clean"), ("bench", "bench")] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "matrix_features" => {
            argv.push("matrix".into());
            argv.push("features".into());
            for (flag, key) in [
                ("toolchain", "toolchain"),
                ("version", "version"),
                ("std", "std"),
                ("kind", "kind"),
            ] {
                push_flag(&mut argv, arguments, flag, key);
            }
            argv.push("--json".into());
        }
        "matrix_explain" => {
            argv.push("matrix".into());
            argv.push("explain".into());
            argv.push(required(arguments, "query")?);
            argv.push("--json".into());
        }
        "matrix_profile" => {
            argv.push("matrix".into());
            argv.push("profile".into());
            for (flag, key) in [
                ("toolchain", "toolchain"),
                ("version", "version"),
                ("std", "std"),
            ] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "app_record" => {
            argv.push("app".into());
            argv.push("record".into());
            argv.push(required(arguments, "repo")?);
            for (flag, key) in [
                ("inputs", "inputs"),
                ("name", "name"),
                ("timeout", "timeout"),
                ("float-tolerance", "float_tolerance"),
            ] {
                push_flag(&mut argv, arguments, flag, key);
            }
            push_repeated(&mut argv, arguments, "normalize", "normalize");
            push_repeated(&mut argv, arguments, "fresh", "fresh");
            push_repeated(&mut argv, arguments, "arg", "args");
            if arguments.get("traps").and_then(Json::as_bool) == Some(false) {
                argv.push("--no-traps".into());
            }
            if let Some(run) = arguments.get("run").and_then(Json::as_array) {
                let run: Vec<String> = run
                    .iter()
                    .filter_map(|item| item.as_str().map(str::to_string))
                    .collect();
                if !run.is_empty() {
                    argv.push("--".into());
                    argv.extend(run);
                }
            }
        }
        "app_replay" => {
            argv.push("app".into());
            argv.push("replay".into());
            argv.push(required(arguments, "repo")?);
            for (flag, key) in [
                ("build", "build"),
                ("timeout", "timeout"),
                ("case", "case"),
                ("json", "json"),
            ] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "app_front_door" => {
            argv.push("app".into());
            argv.push("front-door".into());
            argv.push(required(arguments, "repo")?);
            for (flag, key) in [("no-check", "no_check"), ("json", "json")] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "app_wire" => {
            argv.push("app".into());
            argv.push("wire".into());
            argv.push(required(arguments, "repo")?);
            argv.push(required(arguments, "unit")?);
            push_flag(&mut argv, arguments, "command", "command");
            push_flag(&mut argv, arguments, "project", "project");
        }
        "app_lift" => {
            argv.push("app".into());
            argv.push("lift".into());
            argv.push(required(arguments, "repo")?);
            push_flag(&mut argv, arguments, "json", "json");
        }
        "setup" => {
            argv.push("setup".into());
            push_repeated(&mut argv, arguments, "agent", "agent");
            for (flag, key) in [
                ("remove", "remove"),
                ("dry-run", "dry_run"),
                ("json", "json"),
            ] {
                push_flag(&mut argv, arguments, flag, key);
            }
        }
        "doctor" => {
            argv.push("doctor".into());
            argv.push("--json".into());
        }
        other => return Err(format!("unknown tool `{other}`")),
    }
    Ok(argv)
}

fn result_response(id: Json, result: Json) -> Json {
    Json::obj([
        ("jsonrpc", Json::str("2.0")),
        ("id", id),
        ("result", result),
    ])
}

fn error_response(id: Json, code: i64, message: &str) -> Json {
    Json::obj([
        ("jsonrpc", Json::str("2.0")),
        ("id", id),
        (
            "error",
            Json::obj([("code", Json::Int(code)), ("message", Json::str(message))]),
        ),
    ])
}
