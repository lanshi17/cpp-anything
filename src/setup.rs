//! `setup`: register cpp-anything with the coding agents on this machine.
//!
//! Writes only its own entries, keeps every other setting byte-for-byte where it can,
//! backs up a file before changing it, and is safe to run again. `--remove` takes the
//! entries out again.

use std::path::{Path, PathBuf};

use crate::cli::Parsed;
use crate::json::Json;
use crate::util;

pub const SERVER_KEY: &str = "cpp-anything";

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Agent {
    Claude,
    Codex,
    Cursor,
    Gemini,
    OpenCode,
}

impl Agent {
    pub const ALL: [Agent; 5] = [
        Agent::Claude,
        Agent::Codex,
        Agent::Cursor,
        Agent::Gemini,
        Agent::OpenCode,
    ];

    pub fn slug(self) -> &'static str {
        match self {
            Agent::Claude => "claude",
            Agent::Codex => "codex",
            Agent::Cursor => "cursor",
            Agent::Gemini => "gemini",
            Agent::OpenCode => "opencode",
        }
    }

    pub fn title(self) -> &'static str {
        match self {
            Agent::Claude => "Claude Code",
            Agent::Codex => "Codex",
            Agent::Cursor => "Cursor",
            Agent::Gemini => "Gemini CLI",
            Agent::OpenCode => "OpenCode",
        }
    }

    pub fn marker_dir(self) -> &'static str {
        match self {
            Agent::Claude => ".claude",
            Agent::Codex => ".codex",
            Agent::Cursor => ".cursor",
            Agent::Gemini => ".gemini",
            Agent::OpenCode => ".config/opencode",
        }
    }

    pub fn command(self) -> &'static str {
        match self {
            Agent::Claude => "claude",
            Agent::Codex => "codex",
            Agent::Cursor => "cursor",
            Agent::Gemini => "gemini",
            Agent::OpenCode => "opencode",
        }
    }

    pub fn config_file(self, home: &Path) -> PathBuf {
        match self {
            Agent::Claude => home.join(".claude.json"),
            Agent::Codex => home.join(".codex").join("config.toml"),
            Agent::Cursor => home.join(".cursor").join("mcp.json"),
            Agent::Gemini => home.join(".gemini").join("settings.json"),
            Agent::OpenCode => home.join(".config").join("opencode").join("opencode.json"),
        }
    }

    pub fn skill_dir(self, home: &Path) -> Option<PathBuf> {
        match self {
            Agent::Claude => Some(home.join(".claude").join("skills").join(SERVER_KEY)),
            Agent::Codex => Some(home.join(".codex").join("skills").join(SERVER_KEY)),
            Agent::OpenCode => Some(
                home.join(".config")
                    .join("opencode")
                    .join("skills")
                    .join(SERVER_KEY),
            ),
            Agent::Cursor | Agent::Gemini => None,
        }
    }

    fn present(self, home: &Path) -> bool {
        home.join(self.marker_dir()).exists() || util::which(self.command()).is_some()
    }
}

pub struct Change {
    pub agent: &'static str,
    pub file: String,
    pub action: &'static str,
    pub backup: Option<String>,
}

pub fn command(args: &Parsed) -> Result<i32, String> {
    let home = home_dir()?;
    let executable = std::env::current_exe()
        .map_err(|error| format!("current_exe: {error}"))?
        .canonicalize()
        .unwrap_or_else(|_| PathBuf::from(SERVER_KEY));
    let requested: Vec<Agent> = if args.many("agent").is_empty() {
        Agent::ALL
            .into_iter()
            .filter(|agent| agent.present(&home))
            .collect()
    } else {
        let mut agents = Vec::new();
        for name in args.many("agent") {
            let agent = Agent::ALL
                .into_iter()
                .find(|agent| agent.slug() == name)
                .ok_or_else(|| {
                    format!("unknown agent {name:?}: claude|codex|cursor|gemini|opencode")
                })?;
            agents.push(agent);
        }
        agents
    };
    if requested.is_empty() {
        return Err(
            "no coding agent found (looked for ~/.claude, ~/.codex, ~/.cursor, ~/.gemini, \
             ~/.config/opencode and their commands); name one with --agent claude|codex|cursor|gemini|opencode"
                .into(),
        );
    }
    let remove = args.flag("remove");
    let dry_run = args.flag("dry-run");
    let mut changes = Vec::new();
    for agent in &requested {
        changes.extend(wire_agent(*agent, &home, &executable, remove, dry_run)?);
    }

    if args.flag("json") {
        let doc = Json::obj([
            ("command", Json::str(executable.display().to_string())),
            (
                "agents",
                Json::Array(
                    requested
                        .iter()
                        .map(|agent| Json::str(agent.slug()))
                        .collect(),
                ),
            ),
            (
                "changes",
                Json::Array(
                    changes
                        .iter()
                        .map(|change| {
                            let mut fields = vec![
                                ("agent", Json::str(change.agent)),
                                ("file", Json::str(change.file.clone())),
                                ("action", Json::str(change.action)),
                            ];
                            if let Some(backup) = &change.backup {
                                fields.push(("backup", Json::str(backup.clone())));
                            }
                            Json::obj(fields)
                        })
                        .collect(),
                ),
            ),
            ("dry_run", Json::Bool(dry_run)),
        ]);
        println!("{}", doc.to_string_pretty());
    } else {
        let names = requested
            .iter()
            .map(|agent| agent.title())
            .collect::<Vec<_>>()
            .join(", ");
        println!(
            "{} {SERVER_KEY} for {names}",
            if dry_run { "Would set up" } else { "Set up" }
        );
        for change in &changes {
            println!(
                "  {:<10} {:<9} {}",
                change.agent, change.action, change.file
            );
            if let Some(backup) = &change.backup {
                println!("  {:<10} {:<9} {backup}", "", "backup");
            }
        }
        let touched = changes
            .iter()
            .any(|change| !matches!(change.action, "removed" | "absent"));
        if !dry_run && touched && !remove {
            println!(
                "\nThe agents run `{} mcp`. Restart them, then ask, for example:\n  \"Port this repo's \
                 hottest unit to C++ with cpp-anything and stop when gate passes.\"",
                executable.display()
            );
        }
    }
    Ok(0)
}

fn home_dir() -> Result<PathBuf, String> {
    std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .filter(|path| !path.as_os_str().is_empty())
        .ok_or_else(|| "no home directory (HOME is not set)".to_string())
}

fn wire_agent(
    agent: Agent,
    home: &Path,
    executable: &Path,
    remove: bool,
    dry_run: bool,
) -> Result<Vec<Change>, String> {
    let config = agent.config_file(home);
    let file = config.display().to_string();
    let (action, changed_text) = match agent {
        Agent::Codex => edit_toml(&config, executable, remove)?,
        Agent::OpenCode => edit_json(&config, executable, remove, Some("opencode"))?,
        Agent::Cursor | Agent::Gemini => edit_json(&config, executable, remove, None)?,
        Agent::Claude => edit_json(&config, executable, remove, Some("claude"))?,
    };
    let mut backup = None;
    if !dry_run && action != "unchanged" && action != "absent" {
        if let Some(text) = changed_text {
            if config.exists() {
                let backup_path = backup_path_for(&config);
                std::fs::copy(&config, &backup_path)
                    .map_err(|error| format!("{}: {error}", backup_path.display()))?;
                backup = Some(backup_path.display().to_string());
            }
            write_atomic(&config, &text)?;
        } else if remove && config.exists() {
            // Nothing of ours left: leave the file alone.
        }
    }
    let mut changes = vec![Change {
        agent: agent.slug(),
        file,
        action,
        backup,
    }];
    if let Some(dir) = agent.skill_dir(home) {
        let (skill_action, skill_file) = install_skill(&dir, remove, dry_run)?;
        if skill_action != "unchanged" {
            changes.push(Change {
                agent: agent.slug(),
                file: skill_file,
                action: skill_action,
                backup: None,
            });
        }
    }
    Ok(changes)
}

fn backup_path_for(path: &Path) -> PathBuf {
    let name = path
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    path.with_file_name(format!("{name}.{SERVER_KEY}.bak"))
}

fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|error| format!("{}: {error}", parent.display()))?;
    }
    let temp = path.with_extension("cpp-anything.tmp");
    util::write(&temp, text)?;
    if let Ok(metadata) = std::fs::metadata(path) {
        let _ = std::fs::set_permissions(&temp, metadata.permissions());
    }
    std::fs::rename(&temp, path).map_err(|error| format!("{}: {error}", path.display()))
}

/// JSON agents: `mcpServers.cpp-anything` by default; OpenCode keeps its shape.
fn edit_json(
    path: &Path,
    executable: &Path,
    remove: bool,
    flavor: Option<&str>,
) -> Result<(&'static str, Option<String>), String> {
    let existing = if path.exists() {
        Some(util::read(path)?)
    } else {
        None
    };
    let mut root = match &existing {
        Some(text) if !text.trim().is_empty() => Json::parse(text).map_err(|_| {
            format!(
                "{} is not valid JSON; fix it or add the server by hand",
                path.display()
            )
        })?,
        _ => Json::Object(Vec::new()),
    };
    if !matches!(root, Json::Object(_)) {
        return Err(format!(
            "{} is not a JSON object; add the server by hand",
            path.display()
        ));
    }

    let had = match flavor {
        Some("opencode") => {
            crate::setup::json_get(&root, &["mcp", "servers", SERVER_KEY]).is_some()
                || crate::setup::json_get(&root, &["mcp", SERVER_KEY]).is_some()
        }
        _ => crate::setup::json_get(&root, &["mcpServers", SERVER_KEY]).is_some(),
    };

    if remove {
        let action = if had { "removed" } else { "absent" };
        if !had {
            return Ok((action, None));
        }
        match flavor {
            Some("opencode") => {
                crate::setup::json_remove(&mut root, &["mcp", "servers", SERVER_KEY]);
                crate::setup::json_remove(&mut root, &["mcp", SERVER_KEY]);
                let servers_empty = crate::setup::json_get(&root, &["mcp", "servers"])
                    .and_then(Json::as_object)
                    .is_some_and(|pairs| pairs.is_empty());
                if servers_empty {
                    crate::setup::json_remove(&mut root, &["mcp", "servers"]);
                }
                let mcp_empty = crate::setup::json_get(&root, &["mcp"])
                    .and_then(Json::as_object)
                    .is_some_and(|pairs| pairs.is_empty());
                if mcp_empty {
                    crate::setup::json_remove(&mut root, &["mcp"]);
                }
            }
            _ => crate::setup::json_remove(&mut root, &["mcpServers", SERVER_KEY]),
        }
        return Ok(("removed", Some(root.to_string_pretty() + "\n")));
    }

    let executable_text = executable.display().to_string();
    let unchanged = match flavor {
        Some("opencode") => crate::setup::json_get(&root, &["mcp", "servers", SERVER_KEY])
            .or_else(|| crate::setup::json_get(&root, &["mcp", SERVER_KEY]))
            .is_some_and(|entry| {
                entry.get("url").is_none()
                    && entry.get("disabled").is_none()
                    && entry
                        .get("command")
                        .and_then(Json::as_array)
                        .is_some_and(|command| {
                            command.len() == 2
                                && command[0].as_str() == Some(executable_text.as_str())
                                && command[1].as_str() == Some("mcp")
                        })
            }),
        _ => crate::setup::json_get(&root, &["mcpServers", SERVER_KEY]).is_some_and(|entry| {
            entry.get("command").and_then(Json::as_str) == Some(executable_text.as_str())
                && entry.get("url").is_none()
                && entry
                    .get("args")
                    .and_then(Json::as_array)
                    .map(|args| args.len() == 1 && args[0].as_str() == Some("mcp"))
                    .unwrap_or(false)
        }),
    };
    if unchanged {
        return Ok(("unchanged", None));
    }

    let action = if had { "updated" } else { "added" };
    let mut entry: Vec<(String, Json)> = Vec::new();
    match flavor {
        Some("opencode") => {
            entry.push(("type".into(), Json::str("local")));
            entry.push((
                "command".into(),
                Json::Array(vec![Json::str(executable_text.clone()), Json::str("mcp")]),
            ));
        }
        _ => {
            entry.push(("command".into(), Json::str(executable_text.clone())));
            entry.push(("args".into(), Json::Array(vec![Json::str("mcp")])));
            entry.push(("type".into(), Json::str("stdio")));
            if flavor == Some("claude") {
                entry.push(("env".into(), Json::Object(Vec::new())));
            }
        }
    }
    // Keep everything a user had on our entry except the parts that would break it.
    if let Some(Json::Object(pairs)) = match flavor {
        Some("opencode") => crate::setup::json_get(&root, &["mcp", "servers", SERVER_KEY])
            .or_else(|| crate::setup::json_get(&root, &["mcp", SERVER_KEY])),
        _ => crate::setup::json_get(&root, &["mcpServers", SERVER_KEY]),
    } {
        {
            for (key, value) in pairs {
                if matches!(
                    key.as_str(),
                    "command" | "args" | "url" | "disabled" | "enabled" | "type" | "env"
                ) {
                    continue;
                }
                entry.push((key.clone(), value.clone()));
            }
        }
    }
    let object = || Json::Object(entry.clone());
    match flavor {
        Some("opencode") => {
            let servers_object = match crate::setup::json_get_mut(&mut root, &["mcp", "servers"]) {
                Some(Json::Object(pairs)) => {
                    pairs.retain(|(key, _)| key != SERVER_KEY);
                    pairs.push((SERVER_KEY.to_string(), object()));
                    true
                }
                _ => false,
            };
            if !servers_object {
                crate::setup::json_set(
                    &mut root,
                    &["mcp"],
                    Json::obj([("servers", Json::obj([(SERVER_KEY, object())]))]),
                );
            }
        }
        _ => crate::setup::json_set(&mut root, &["mcpServers", SERVER_KEY], object()),
    }
    Ok((action, Some(root.to_string_pretty() + "\n")))
}

/// Codex uses TOML: rewrite just the `[mcp_servers.cpp-anything]` table, keep the
/// rest of the file byte-for-byte.
fn edit_toml(
    path: &Path,
    executable: &Path,
    remove: bool,
) -> Result<(&'static str, Option<String>), String> {
    let existing = if path.exists() {
        Some(util::read(path)?)
    } else {
        None
    };
    let text = existing.clone().unwrap_or_default();
    let header = format!("[mcp_servers.{SERVER_KEY}]");
    let lines: Vec<&str> = text.lines().collect();
    let mut start = None;
    for (index, line) in lines.iter().enumerate() {
        if line.trim() == header {
            start = Some(index);
            break;
        }
    }
    let mut end = lines.len();
    if let Some(begin) = start {
        for (index, line) in lines.iter().enumerate().skip(begin + 1) {
            if line.trim_start().starts_with('[') {
                end = index;
                break;
            }
        }
    }
    let had = start.is_some()
        || text
            .lines()
            .any(|line| line.contains(&format!("mcp_servers.{SERVER_KEY} =")));
    if remove {
        if !had {
            return Ok(("absent", None));
        }
        let mut out: Vec<String> = Vec::new();
        for (index, line) in lines.iter().enumerate() {
            if let Some(begin) = start {
                if index >= begin && index < end {
                    continue;
                }
            }
            out.push((*line).to_string());
        }
        let mut result = out.join("\n");
        if !result.is_empty() && !result.ends_with('\n') {
            result.push('\n');
        }
        return Ok(("removed", Some(result)));
    }

    let executable_text = executable.display().to_string();
    let unchanged = start.is_some_and(|begin| {
        let block = &lines[begin + 1..end];
        let command = block
            .iter()
            .find(|line| line.trim_start().starts_with("command"));
        let has_url = block
            .iter()
            .any(|line| line.trim_start().starts_with("url"));
        let args_ok = block
            .iter()
            .any(|line| line.trim_start().starts_with("args") && line.contains("\"mcp\""));
        command.is_some_and(|line| line.contains(&executable_text)) && !has_url && args_ok
    });
    if unchanged {
        return Ok(("unchanged", None));
    }
    let action = if had { "updated" } else { "added" };
    let block = format!(
        "{header}\ncommand = {command:?}\nargs = [\"mcp\"]\n",
        header = header,
        command = executable_text
    );
    let mut out: Vec<String> = Vec::new();
    if let Some(begin) = start {
        out.extend(lines[..begin].iter().map(|line| line.to_string()));
        out.push(block.trim_end().to_string());
        out.extend(lines[end..].iter().map(|line| line.to_string()));
    } else {
        out.extend(lines.iter().map(|line| line.to_string()));
        if !out.is_empty() && !out.last().is_some_and(|line| line.is_empty()) {
            out.push(String::new());
        }
        out.push(block.trim_end().to_string());
    }
    let mut result = out.join("\n");
    if !result.ends_with('\n') {
        result.push('\n');
    }
    Ok((action, Some(result)))
}

fn install_skill(
    dir: &Path,
    remove: bool,
    dry_run: bool,
) -> Result<(&'static str, String), String> {
    let skill = dir.join("SKILL.md");
    let body = include_str!("../skills/cpp-anything/SKILL.md");
    let existing = util::read(&skill).ok();
    if remove {
        if existing.is_none() {
            return Ok(("absent", skill.display().to_string()));
        }
        if !dry_run {
            let _ = std::fs::remove_file(&skill);
            let _ = std::fs::remove_dir(dir);
        }
        return Ok(("removed", skill.display().to_string()));
    }
    let action = match existing {
        None => "added",
        Some(text) if text == body => "unchanged",
        Some(_) => "updated",
    };
    if action != "unchanged" && !dry_run {
        util::write(&skill, body)?;
    }
    Ok((action, skill.display().to_string()))
}

// ---------------------------------------------------------------------------
// tiny JSON tree helpers (objects only, order preserved)
// ---------------------------------------------------------------------------

pub fn json_get<'a>(value: &'a Json, path: &[&str]) -> Option<&'a Json> {
    let mut current = value;
    for key in path {
        current = current.get(key)?;
    }
    Some(current)
}

pub fn json_get_mut<'a>(value: &'a mut Json, path: &[&str]) -> Option<&'a mut Json> {
    if path.is_empty() {
        return Some(value);
    }
    let Json::Object(pairs) = value else {
        return None;
    };
    let key = path[0];
    for (name, item) in pairs.iter_mut() {
        if name == key {
            return json_get_mut(item, &path[1..]);
        }
    }
    None
}

pub fn json_set(value: &mut Json, path: &[&str], new: Json) {
    if path.is_empty() {
        *value = new;
        return;
    }
    if !matches!(value, Json::Object(_)) {
        *value = Json::Object(Vec::new());
    }
    let Json::Object(pairs) = value else { return };
    let key = path[0];
    if let Some((_, item)) = pairs.iter_mut().find(|(name, _)| name == key) {
        json_set(item, &path[1..], new);
    } else {
        let mut nested = Json::Object(Vec::new());
        json_set(&mut nested, &path[1..], new);
        pairs.push((key.to_string(), nested));
    }
}

pub fn json_remove(value: &mut Json, path: &[&str]) {
    if path.is_empty() {
        return;
    }
    let Json::Object(pairs) = value else { return };
    if path.len() == 1 {
        pairs.retain(|(name, _)| name != path[0]);
        return;
    }
    if let Some((_, item)) = pairs.iter_mut().find(|(name, _)| name == path[0]) {
        json_remove(item, &path[1..]);
    }
}
