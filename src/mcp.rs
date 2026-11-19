//! `mcp`: the same steps as MCP tools over stdio, so an agent can drive the loop.
//!
//! Hand-written JSON-RPC: the stdio transport needs `initialize`, `tools/list`,
//! `tools/call` and `ping`, and every tool is the CLI itself in a subprocess, so an
//! MCP call and a terminal call can never drift apart.

use std::io::{BufRead, Write};

use crate::json::Json;
use crate::util;

const PROTOCOL: &str = "2024-11-05";

struct Tool {
    name: &'static str,
    description: &'static str,
    schema: &'static str,
}

const TOOLS: &[Tool] = &[
    Tool {
        name: "matrix_features",
        description: "What a compiler release supports of one C++ standard, from cppreference's tables.",
        schema: r#"{"type":"object","properties":{"toolchain":{"type":"string","description":"gcc, clang, msvc, apple-clang"},"version":{"type":"string"},"std":{"type":"string"},"kind":{"type":"string","enum":["all","core","library"]}},"required":["toolchain","version","std"]}"#,
    },
    Tool {
        name: "matrix_explain",
        description: "One feature (name or feature-test macro): which release of each compiler implements it.",
        schema: r#"{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}"#,
    },
    Tool {
        name: "matrix_profile",
        description: "Generate include/cppan/profile.hpp for a target toolchain: CPPAN_HAS_* macros from the matrix.",
        schema: r#"{"type":"object","properties":{"toolchain":{"type":"string"},"version":{"type":"string"},"std":{"type":"string"}},"required":["toolchain","version","std"]}"#,
    },
    Tool {
        name: "analyze",
        description: "Plan a port: the modules in a repository, their functions and the risks each one carries.",
        schema: r#"{"type":"object","properties":{"repo":{"type":"string"},"json":{"type":"boolean"}},"required":["repo"]}"#,
    },
    Tool {
        name: "bridge",
        description: "Generate the C++ project for one unit: port skeleton, dispatch, driver, CMake, profile.",
        schema: r#"{"type":"object","properties":{"repo":{"type":"string"},"unit":{"type":"string"},"out":{"type":"string"},"toolchain":{"type":"string"},"version":{"type":"string"},"std":{"type":"string"},"force":{"type":"boolean"},"module_root":{"type":"string"}},"required":["repo","unit"]}"#,
    },
    Tool {
        name: "record",
        description: "Run the original on generated edge cases and save what it returns as cases.jsonl.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"cases":{"type":"number"},"seed":{"type":"number"},"inputs":{"type":"string"}},"required":["project"]}"#,
    },
    Tool {
        name: "build",
        description: "Configure and build the generated C++ project with CMake.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"clean":{"type":"boolean"},"cxx":{"type":"string"},"jobs":{"type":"number"}},"required":["project"]}"#,
    },
    Tool {
        name: "diff",
        description: "Replay every recorded case through the C++ port and compare with the original.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"json":{"type":"boolean"},"strict_errors":{"type":"boolean"},"tolerance":{"type":"number"},"only":{"type":"string"},"max_report":{"type":"number"}},"required":["project"]}"#,
    },
    Tool {
        name: "bench",
        description: "Time the original against the port on the recorded cases.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"repeat":{"type":"number"},"json":{"type":"boolean"}},"required":["project"]}"#,
    },
    Tool {
        name: "gate",
        description: "The definition of done: matrix, no stubs, build, dispatch and every recorded case.",
        schema: r#"{"type":"object","properties":{"project":{"type":"string"},"json":{"type":"boolean"},"clean":{"type":"boolean"},"bench":{"type":"boolean"}},"required":["project"]}"#,
    },
    Tool {
        name: "doctor",
        description: "Check cmake, a C++ compiler, python3, the matrix and the guides.",
        schema: r#"{"type":"object","properties":{"json":{"type":"boolean"}}}"#,
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
        let params = request.get("params").cloned().unwrap_or(Json::Null);
        if id.is_null() {
            continue; // notifications need no answer
        }
        let response = match method.as_str() {
            "initialize" => result_response(
                id,
                Json::obj([
                    ("protocolVersion", Json::str(PROTOCOL)),
                    ("capabilities", Json::obj([("tools", Json::Object(Vec::new()))])),
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
                            "Port one unit at a time: analyze, bridge, record, write src/port.cpp, diff, gate. \
                             matrix answers what the target toolchain supports.",
                        ),
                    ),
                ]),
            ),
            "ping" => result_response(id, Json::Object(Vec::new())),
            "tools/list" => result_response(id, tools_list()),
            "tools/call" => match call_tool(&params) {
                Ok(text) => result_response(
                    id,
                    Json::obj([
                        ("content", Json::Array(vec![Json::obj([
                            ("type", Json::str("text")),
                            ("text", Json::str(text)),
                        ])])),
                        ("isError", Json::Bool(false)),
                    ]),
                ),
                Err(error) => result_response(
                    id,
                    Json::obj([
                        ("content", Json::Array(vec![Json::obj([
                            ("type", Json::str("text")),
                            ("text", Json::str(error)),
                        ])])),
                        ("isError", Json::Bool(true)),
                    ]),
                ),
            },
            other => error_response(id, -32601, &format!("unknown method {other}")),
        };
        let _ = writeln!(stdout, "{}", response.to_string_compact());
        let _ = stdout.flush();
    }
    Ok(0)
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
                        ("description", Json::str(tool.description)),
                        (
                            "inputSchema",
                            Json::parse(tool.schema).unwrap_or_else(|_| Json::Object(Vec::new())),
                        ),
                    ])
                })
                .collect(),
        ),
    )])
}

fn call_tool(params: &Json) -> Result<String, String> {
    let name = params
        .get("name")
        .and_then(Json::as_str)
        .ok_or("tools/call needs a name")?;
    let arguments = params
        .get("arguments")
        .cloned()
        .unwrap_or(Json::Object(Vec::new()));
    let mut argv: Vec<String> = Vec::new();
    let push = |argv: &mut Vec<String>, flag: &str, key: &str| {
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
                    argv.push(number.to_string());
                }
                _ => {}
            }
        }
    };
    let positional = |key: &str| {
        arguments
            .get(key)
            .and_then(Json::as_str)
            .map(str::to_string)
            .ok_or_else(|| format!("tools/call {name}: missing {key}"))
    };

    match name {
        "matrix_features" => {
            argv.push("matrix".into());
            argv.push("features".into());
            for (flag, key) in [
                ("toolchain", "toolchain"),
                ("version", "version"),
                ("std", "std"),
                ("kind", "kind"),
            ] {
                push(&mut argv, flag, key);
            }
            argv.push("--json".into());
        }
        "matrix_explain" => {
            argv.push("matrix".into());
            argv.push("explain".into());
            argv.push(positional("query")?);
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
                push(&mut argv, flag, key);
            }
        }
        "analyze" => {
            argv.push("analyze".into());
            argv.push(positional("repo")?);
            argv.push("--json".into());
        }
        "bridge" => {
            argv.push("bridge".into());
            argv.push(positional("repo")?);
            argv.push(positional("unit")?);
            for (flag, key) in [
                ("out", "out"),
                ("toolchain", "toolchain"),
                ("version", "version"),
                ("std", "std"),
                ("force", "force"),
                ("module-root", "module_root"),
            ] {
                push(&mut argv, flag, key);
            }
        }
        "record" => {
            argv.push("record".into());
            argv.push(positional("project")?);
            for (flag, key) in [("cases", "cases"), ("seed", "seed"), ("inputs", "inputs")] {
                push(&mut argv, flag, key);
            }
        }
        "build" => {
            argv.push("build".into());
            argv.push(positional("project")?);
            for (flag, key) in [("clean", "clean"), ("cxx", "cxx"), ("jobs", "jobs")] {
                push(&mut argv, flag, key);
            }
        }
        "diff" => {
            argv.push("diff".into());
            argv.push(positional("project")?);
            for (flag, key) in [
                ("json", "json"),
                ("strict-errors", "strict_errors"),
                ("tolerance", "tolerance"),
                ("only", "only"),
                ("max-report", "max_report"),
            ] {
                push(&mut argv, flag, key);
            }
        }
        "bench" => {
            argv.push("bench".into());
            argv.push(positional("project")?);
            for (flag, key) in [("repeat", "repeat"), ("json", "json")] {
                push(&mut argv, flag, key);
            }
        }
        "gate" => {
            argv.push("gate".into());
            argv.push(positional("project")?);
            for (flag, key) in [("json", "json"), ("clean", "clean"), ("bench", "bench")] {
                push(&mut argv, flag, key);
            }
        }
        "doctor" => {
            argv.push("doctor".into());
            argv.push("--json".into());
        }
        other => return Err(format!("unknown tool {other}")),
    }

    let exe = std::env::current_exe().map_err(|error| format!("current_exe: {error}"))?;
    let run = util::run(&exe.display().to_string(), &argv, None);
    if run.ok {
        Ok(run.stdout)
    } else {
        Err(format!(
            "cpp-anything {} failed: {}\n{}",
            argv.join(" "),
            run.failure(),
            run.stderr
        ))
    }
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
