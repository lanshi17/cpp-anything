//! `profile`: run a real workload under the language's profiler and say what is
//! worth moving, with the advice an agent would otherwise guess.
//!
//! Python keeps using the bundled probe (cProfile), which already speaks this
//! module's JSON. Node, Ruby, Go and Java each get a collector here: Node's
//! `--cpu-prof`, a `TracePoint` tracer for Ruby, `go test -cpuprofile` read with
//! `go tool pprof -raw`, and Flight Recorder for Java. Every collector ends in
//! the same ranked `Report`, so the rest of the tool does not care where the
//! numbers came from.

use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::sync::LazyLock;

use crate::json::Json;
use crate::util;

pub struct Entry {
    pub harness: bool,
    pub function: String,
    pub file: String,
    pub line: i64,
    pub calls: i64,
    pub self_seconds: f64,
    pub cum_seconds: f64,
    pub per_call_ns: f64,
    pub share: f64,
    pub callers: Vec<String>,
    pub advice: String,
    pub external: bool,
}

pub struct Report {
    pub workload: Vec<String>,
    pub total_seconds: f64,
    pub entries: Vec<Entry>,
}

/// Runs the workload through the profiler that fits its runtime and turns the
/// raw stats into a ranked report. `python3 bench.py` and `bench.py` both work;
/// the probe strips the interpreter so cProfile runs the script itself. Node,
/// Ruby, Go and Java are profiled in-process and read back here.
pub fn run(root: &Path, command: &[String], top: usize) -> Result<Report, String> {
    if command.is_empty() {
        return Err(
            "profile needs the workload after `--`, e.g. `cpp-anything profile . -- python3 bench.py`"
                .into(),
        );
    }
    let program = Path::new(&command[0])
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    let runtime = runtime_of(&program);
    match runtime {
        Runtime::Node | Runtime::Ruby | Runtime::Java => {
            let root = canonical_root(root)?;
            let work = work_dir()?;
            let result = match runtime {
                Runtime::Node => node(&root, command, &work),
                Runtime::Ruby => ruby(&root, command, &work),
                Runtime::Java => java(&root, command, &work),
                _ => unreachable!(),
            };
            let _ = std::fs::remove_dir_all(&work);
            let (functions, total_seconds) = result?;
            Ok(assemble(command, functions, total_seconds, top))
        }
        Runtime::Go => {
            let root = canonical_root(root)?;
            let work = work_dir()?;
            let result = go(&root, command, &work);
            let _ = std::fs::remove_dir_all(&work);
            let (functions, total_seconds) = result?;
            Ok(assemble(command, functions, total_seconds, top))
        }
        Runtime::Python => {
            let mut args = vec!["profile".to_string()];
            args.extend(command.iter().cloned());
            // The workload's paths are relative to the caller's cwd, like every
            // other command the user would type; `root` is only for detection and
            // the report.
            let output = crate::python::probe_command(&args)?;
            let doc = Json::parse(output.trim())?;
            if let Some(error) = doc.get("error").and_then(Json::as_str) {
                let stderr = doc.get("stderr").and_then(Json::as_str).unwrap_or("");
                return Err(format!("{error}\n{}", util::indent(stderr, 2)));
            }
            Ok(rank(&doc, top))
        }
    }
}

enum Runtime {
    Node,
    Ruby,
    Go,
    Java,
    Python,
}

fn runtime_of(program: &str) -> Runtime {
    let program = program.strip_suffix(".exe").unwrap_or(program);
    match program {
        "node" | "nodejs" => Runtime::Node,
        "go" => Runtime::Go,
        "java" => Runtime::Java,
        _ if program == "ruby" || program.starts_with("ruby3") || program.starts_with("ruby2") => {
            Runtime::Ruby
        }
        _ => Runtime::Python,
    }
}

fn canonical_root(root: &Path) -> Result<std::path::PathBuf, String> {
    root.canonicalize()
        .map_err(|error| format!("{}: {error}", root.display()))
}

/// A private scratch directory under the system temp dir; profilers write their
/// raw recordings there and nothing leaks into the repository.
fn work_dir() -> Result<std::path::PathBuf, String> {
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|elapsed| elapsed.subsec_nanos())
        .unwrap_or(0);
    let work = std::env::temp_dir().join(format!(
        "cpp-anything-profile-{}-{stamp}",
        std::process::id()
    ));
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).map_err(|error| format!("{}: {error}", work.display()))?;
    Ok(work)
}

// ---- shared model ----------------------------------------------------------------------------

/// A frame in user terms: file (repository-relative, or absolute when the file
/// is outside the repository), function, line and whether it is library code.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct FrameKey {
    file: String,
    function: String,
    line: i64,
    external: bool,
}

#[derive(Default)]
struct Acc {
    self_seconds: f64,
    cum_seconds: f64,
    calls: i64,
    /// Caller -> (inclusive seconds observed under it, calls it made).
    callers: HashMap<FrameKey, (f64, i64)>,
}

fn assemble(
    workload: &[String],
    functions: HashMap<FrameKey, Acc>,
    total_seconds: f64,
    top: usize,
) -> Report {
    let mut entries: Vec<Entry> = functions
        .into_iter()
        .filter(|(_, acc)| acc.self_seconds > 0.0 || acc.cum_seconds > 0.0)
        .map(|(key, acc)| {
            let mut callers: Vec<(String, f64)> = acc
                .callers
                .iter()
                .map(|(caller, (seconds, _))| (caller.function.clone(), *seconds))
                .collect();
            callers.sort_by(|a, b| b.1.total_cmp(&a.1));
            callers.truncate(5);
            Entry {
                harness: false,
                function: key.function,
                file: key.file,
                line: key.line,
                calls: acc.calls,
                self_seconds: acc.self_seconds,
                cum_seconds: acc.cum_seconds,
                per_call_ns: if acc.calls > 0 {
                    acc.cum_seconds * 1e9 / acc.calls as f64
                } else {
                    0.0
                },
                share: if total_seconds > 0.0 {
                    acc.self_seconds / total_seconds
                } else {
                    0.0
                },
                callers: callers.into_iter().map(|(name, _)| name).collect(),
                advice: String::new(),
                external: key.external,
            }
        })
        .collect();
    for entry in &mut entries {
        entry.advice = advice(entry, total_seconds);
    }
    // Cost order, but never let a library frame hide a repository one: the point
    // of the report is what the user can port.
    entries.sort_by(|a, b| {
        a.external
            .cmp(&b.external)
            .then_with(|| b.cum_seconds.total_cmp(&a.cum_seconds))
            .then_with(|| b.self_seconds.total_cmp(&a.self_seconds))
    });
    entries.truncate(top.max(1));
    Report {
        workload: workload.to_vec(),
        total_seconds,
        entries,
    }
}

// ---- frame classification --------------------------------------------------------------------

/// Turns a profiler's file reference into `(file, external)`: repository files
/// stay relative, files elsewhere keep their absolute path and are marked
/// external, and runtime pseudo-frames (`node:internal/…`, `<frozen …>`) are
/// dropped because there is nothing to port there.
fn classify(root: &Path, raw: &str) -> Option<(String, bool)> {
    let decoded = decode_file_url(raw);
    let path = decoded.as_str();
    if path.is_empty() || path.starts_with('<') || path.starts_with('~') {
        return None;
    }
    let absolute = path.starts_with('/')
        || (path.len() > 2
            && path.as_bytes()[1] == b':'
            && matches!(path.as_bytes()[2], b'\\' | b'/'));
    if !absolute {
        if path.contains(':') {
            return None; // node:internal/..., a URL scheme, not a file
        }
        let rel = path.trim_start_matches("./");
        return root
            .join(rel)
            .is_file()
            .then(|| (rel.replace('\\', "/"), false));
    }
    // Installed dependencies stay external even when they sit inside the repo:
    // they are not code the user wrote.
    let library = path.split(['/', '\\']).any(|part| {
        matches!(
            part,
            "node_modules"
                | "site-packages"
                | "dist-packages"
                | ".venv"
                | "venv"
                | "gems"
                | "vendor"
        )
    }) || ["/lib/python3", "/lib/ruby/", "/pkg/mod/"]
        .iter()
        .any(|directory| path.contains(directory));
    if library {
        return Some((path.to_string(), true));
    }
    match Path::new(path).strip_prefix(root) {
        Ok(rel) => Some((rel.to_string_lossy().replace('\\', "/"), false)),
        Err(_) => Some((path.to_string(), true)),
    }
}

fn decode_file_url(raw: &str) -> String {
    let path = raw.strip_prefix("file://").unwrap_or(raw);
    if !path.contains('%') {
        return path.to_string();
    }
    let bytes = path.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'%' && index + 2 < bytes.len() {
            let hex = std::str::from_utf8(&bytes[index + 1..index + 3]).ok();
            if let Some(byte) = hex.and_then(|hex| u8::from_str_radix(hex, 16).ok()) {
                out.push(byte);
                index += 3;
                continue;
            }
        }
        out.push(bytes[index]);
        index += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Go's own sources; asking `go` once is cheaper than embedding a GOROOT guess.
static GOROOT: LazyLock<Option<String>> = LazyLock::new(|| {
    let run = util::run("go", &["env".to_string(), "GOROOT".to_string()], None);
    let root = run.stdout.trim();
    if !run.ok || root.is_empty() {
        return None;
    }
    let mut prefix = root.to_string();
    if !prefix.ends_with('/') {
        prefix.push('/');
    }
    Some(prefix)
});

fn goroot() -> Option<&'static String> {
    GOROOT.as_ref()
}

// ---- node --cpu-prof -------------------------------------------------------------------------

fn node(
    root: &Path,
    command: &[String],
    work: &Path,
) -> Result<(HashMap<FrameKey, Acc>, f64), String> {
    let dir = work.join("node");
    std::fs::create_dir_all(&dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    // NODE_OPTIONS splits on whitespace but honours double quotes; escape the
    // path so a temp directory with spaces still works.
    let dir_option = dir
        .to_string_lossy()
        .replace('\\', "\\\\")
        .replace('"', "\\\"");
    let options = format!(
        "{} --cpu-prof --cpu-prof-dir=\"{dir_option}\"",
        std::env::var("NODE_OPTIONS").unwrap_or_default().trim()
    );
    let run = util::run_with_env(
        &command[0],
        &command[1..],
        Some(root),
        &[("NODE_OPTIONS".to_string(), options.trim().to_string())],
    );
    if !run.ok {
        return Err(format!("the workload failed ({})", run.failure()));
    }
    let mut profiles: Vec<std::path::PathBuf> = std::fs::read_dir(&dir)
        .map_err(|error| format!("{}: {error}", dir.display()))?
        .flatten()
        .map(|entry| entry.path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == "cpuprofile")
        })
        .collect();
    if profiles.is_empty() {
        return Err(
            "no CPU profile was written: is the workload a node process, and did it run?".into(),
        );
    }
    profiles.sort();
    let mut functions: HashMap<FrameKey, Acc> = HashMap::new();
    let mut total_seconds = 0.0;
    for path in &profiles {
        let text = util::read(path)?;
        let profile =
            Json::parse(text.trim()).map_err(|error| format!("{}: {error}", path.display()))?;
        total_seconds += merge_cpuprofile(root, &profile, &mut functions);
    }
    Ok((functions, total_seconds))
}

struct NodeFrame {
    key: Option<FrameKey>,
    self_seconds: f64,
    children: Vec<i64>,
}

/// Adds one `.cpuprofile` to `functions`; returns its duration in seconds.
fn merge_cpuprofile(root: &Path, profile: &Json, functions: &mut HashMap<FrameKey, Acc>) -> f64 {
    let Some(nodes) = profile.get("nodes").and_then(Json::as_array) else {
        return 0.0;
    };
    let sample_count = profile
        .get("samples")
        .and_then(Json::as_array)
        .map(|samples| samples.len())
        .unwrap_or(0)
        .max(1);
    let start = profile
        .get("startTime")
        .and_then(Json::as_f64)
        .unwrap_or(0.0);
    let end = profile.get("endTime").and_then(Json::as_f64).unwrap_or(0.0);
    let duration_seconds = ((end - start) / 1e6).max(0.0);
    let per_sample = duration_seconds / sample_count as f64;
    // `hitCount` is the sample count per node; fall back to counting `samples`
    // when a producer omits it.
    let mut hits: HashMap<i64, f64> = HashMap::new();
    if let Some(samples) = profile.get("samples").and_then(Json::as_array) {
        for id in samples.iter().filter_map(Json::as_i64) {
            *hits.entry(id).or_default() += 1.0;
        }
    }
    let mut by_id: HashMap<i64, NodeFrame> = HashMap::new();
    let mut is_child: HashSet<i64> = HashSet::new();
    for node in nodes {
        let id = node.get("id").and_then(Json::as_i64).unwrap_or(0);
        let frame = node.get("callFrame");
        let name = frame
            .and_then(|frame| frame.get("functionName"))
            .and_then(Json::as_str)
            .unwrap_or("");
        let url = frame
            .and_then(|frame| frame.get("url"))
            .and_then(Json::as_str)
            .unwrap_or("");
        let line = frame
            .and_then(|frame| frame.get("lineNumber"))
            .and_then(Json::as_i64)
            .unwrap_or(-1)
            + 1;
        let line = line.max(0);
        let key = classify(root, url).map(|(file, external)| FrameKey {
            file,
            // An empty name is a module wrapper or callback; keep a stable,
            // recognisable name and let caller lookup skip it.
            function: if name.is_empty() {
                "(anonymous)".to_string()
            } else {
                name.to_string()
            },
            line,
            external,
        });
        let children: Vec<i64> = node
            .get("children")
            .and_then(Json::as_array)
            .map(|children| children.iter().filter_map(Json::as_i64).collect())
            .unwrap_or_default();
        is_child.extend(children.iter().copied());
        let self_seconds = match node.get("hitCount").and_then(Json::as_f64) {
            Some(count) => count * per_sample,
            None => hits.get(&id).copied().unwrap_or(0.0) * per_sample,
        };
        by_id.insert(
            id,
            NodeFrame {
                key,
                self_seconds,
                children,
            },
        );
    }
    // Inclusive time per node, depth first.
    fn inclusive(id: i64, by_id: &HashMap<i64, NodeFrame>, memo: &mut HashMap<i64, f64>) -> f64 {
        if let Some(total) = memo.get(&id) {
            return *total;
        }
        let Some(frame) = by_id.get(&id) else {
            return 0.0;
        };
        let total = frame.self_seconds
            + frame
                .children
                .iter()
                .map(|child| inclusive(*child, by_id, memo))
                .sum::<f64>();
        memo.insert(id, total);
        total
    }
    let mut memo: HashMap<i64, f64> = HashMap::new();
    // Walk from the roots with the repository frames on the stack: recursion
    // counts once, and a callee's caller is the nearest *named* frame above it,
    // so anonymous callbacks are transparent.
    let roots: Vec<i64> = by_id
        .keys()
        .filter(|id| !is_child.contains(id))
        .copied()
        .collect();
    let mut stack: Vec<(i64, Vec<FrameKey>)> = roots
        .into_iter()
        .map(|root_id| (root_id, Vec::new()))
        .collect();
    while let Some((id, active)) = stack.pop() {
        let Some(frame) = by_id.get(&id) else {
            continue;
        };
        let key = frame.key.clone();
        let mut next = active.clone();
        if let Some(key) = &key {
            let total = inclusive(id, &by_id, &mut memo);
            let entry = functions.entry(key.clone()).or_default();
            entry.self_seconds += frame.self_seconds;
            if !active.contains(key) {
                entry.cum_seconds += total;
                if let Some(caller) = active
                    .iter()
                    .rev()
                    .find(|caller| !caller.function.starts_with("(anonymous)"))
                {
                    if caller != key {
                        entry.callers.entry(caller.clone()).or_default().0 += total;
                    }
                }
            }
            next.push(key.clone());
        }
        for child in &frame.children {
            stack.push((*child, next.clone()));
        }
    }
    duration_seconds
}

// ---- python cProfile (the bundled probe) -----------------------------------------------------

fn rank(doc: &Json, top: usize) -> Report {
    let workload: Vec<String> = doc
        .get("workload")
        .and_then(Json::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default();
    let mut entries = Vec::new();
    let mut total_seconds = 0.0;
    let script = script_of(&workload);
    for item in doc
        .get("functions")
        .and_then(Json::as_array)
        .unwrap_or_default()
    {
        let file = item
            .get("file")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let function = item
            .get("function")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        let self_seconds = item
            .get("self_seconds")
            .and_then(Json::as_f64)
            .unwrap_or(0.0);
        if self_seconds <= 0.0 || function.is_empty() {
            continue;
        }
        // Frozen modules and C builtins have no file to port, and the string pstats
        // builds for them is unreadable: drop them.
        if file.starts_with('<') {
            continue;
        }
        total_seconds += self_seconds;
        let calls = item.get("calls").and_then(Json::as_i64).unwrap_or(0);
        let external = file.contains("site-packages")
            || file.contains("/lib/python")
            || file.starts_with("<frozen");
        let harness = file == script;
        entries.push(Entry {
            harness,
            function,
            file: file.clone(),
            line: item.get("line").and_then(Json::as_i64).unwrap_or(0),
            calls,
            self_seconds,
            cum_seconds: item
                .get("cum_seconds")
                .and_then(Json::as_f64)
                .unwrap_or(0.0),
            per_call_ns: item
                .get("per_call_seconds")
                .and_then(Json::as_f64)
                .unwrap_or(0.0)
                * 1e9,
            share: 0.0,
            callers: item
                .get("callers")
                .and_then(Json::as_array)
                .map(|callers| {
                    callers
                        .iter()
                        .filter_map(|caller| caller.get("function").and_then(Json::as_str))
                        .filter(|name| *name != "<module>")
                        .map(str::to_string)
                        .collect()
                })
                .unwrap_or_default(),
            advice: String::new(),
            external,
        });
    }
    for entry in &mut entries {
        entry.share = if total_seconds > 0.0 {
            entry.self_seconds / total_seconds
        } else {
            0.0
        };
        entry.advice = advice(entry, total_seconds);
    }
    entries.truncate(top.max(1));
    Report {
        workload,
        total_seconds,
        entries,
    }
}

// ---- Ruby TracePoint tracer ------------------------------------------------------------------

/// Loaded with `RUBYOPT=-r`: times every Ruby method call on the main thread
/// (inclusive, self, callers) and writes cProfile-like entries at exit.
/// Tracing slows the program down; the shares stay comparable.
const RUBY_TRACER: &str = r##"
require "json"

module CppAnythingProfile
  STATS = Hash.new { |h, k| h[k] = { calls: 0, self: 0.0, total: 0.0, callers: Hash.new { |c, ck| c[ck] = [0, 0.0] } } }
  STACK = []
  ACTIVE = Hash.new(0)
  MAIN = Thread.current
  TRACE = TracePoint.new(:call, :return) do |tp|
    next unless Thread.current == MAIN
    now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    if tp.event == :call
      path = tp.path
      path = File.expand_path(path) unless path.start_with?("<")
      key = [path, tp.lineno, tp.method_id.to_s]
      ACTIVE[key] += 1
      STACK << [key, now, 0.0]
    elsif (frame = STACK.pop)
      key, start, child = frame
      elapsed = now - start
      ACTIVE[key] -= 1
      outer = ACTIVE[key].zero?
      s = STATS[key]
      s[:calls] += 1
      s[:self] += elapsed - child
      s[:total] += elapsed if outer
      if (parent = STACK.last)
        parent[2] += elapsed
        c = s[:callers][parent[0]]
        c[0] += 1
        c[1] += elapsed if outer
      end
    end
  end
  TRACE.enable

  at_exit do
    TRACE.disable
    out = STATS.map do |(file, line, name), s|
      { file: file, line: line, name: name, calls: s[:calls], self: s[:self] * 1000, total: s[:total] * 1000,
        callers: s[:callers].map { |(f, l, n), (calls, total)| { file: f, line: l, name: n, calls: calls, total: total * 1000 } } }
    end
    File.write(ENV.fetch("CPP_ANYTHING_PROFILE_OUT"), JSON.generate(out))
  end
end
"##;

/// Ruby: the tracer above, loaded into any Ruby command through `RUBYOPT`.
fn ruby(
    root: &Path,
    command: &[String],
    work: &Path,
) -> Result<(HashMap<FrameKey, Acc>, f64), String> {
    let tracer = work.join("cpp_anything_profile.rb");
    util::write(&tracer, RUBY_TRACER)?;
    let out = work.join("profile.json");
    let rubyopt = format!(
        "{} -r{}",
        std::env::var("RUBYOPT").unwrap_or_default().trim(),
        tracer.display()
    );
    let start = std::time::Instant::now();
    let run = util::run_with_env(
        &command[0],
        &command[1..],
        Some(root),
        &[
            ("RUBYOPT".to_string(), rubyopt.trim().to_string()),
            (
                "CPP_ANYTHING_PROFILE_OUT".to_string(),
                out.display().to_string(),
            ),
        ],
    );
    let total_seconds = start.elapsed().as_secs_f64();
    if !run.ok {
        return Err(format!("the workload failed ({})", run.failure()));
    }
    let text = util::read(&out).map_err(|_| {
        "the Ruby tracer wrote nothing: is the workload a Ruby process, and did it reach exit?"
            .to_string()
    })?;
    let entries = Json::parse(text.trim()).map_err(|error| format!("ruby tracer: {error}"))?;
    let mut functions: HashMap<FrameKey, Acc> = HashMap::new();
    ingest_entries(root, entries.as_array().unwrap_or_default(), &mut functions);
    Ok((functions, total_seconds))
}

/// Function entries from a tracing profiler (the Ruby tracer): file, line,
/// name, calls, self and total milliseconds, and callers.
fn ingest_entries(root: &Path, entries: &[Json], functions: &mut HashMap<FrameKey, Acc>) {
    for entry in entries {
        let raw_file = entry.get("file").and_then(Json::as_str).unwrap_or("");
        let Some((file, external)) = classify(root, raw_file) else {
            continue;
        };
        let function = entry
            .get("name")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        if function.is_empty() {
            continue;
        }
        let key = FrameKey {
            file,
            function,
            line: entry.get("line").and_then(Json::as_i64).unwrap_or(0),
            external,
        };
        let acc = functions.entry(key).or_default();
        acc.self_seconds += entry.get("self").and_then(Json::as_f64).unwrap_or(0.0) / 1000.0;
        acc.cum_seconds += entry.get("total").and_then(Json::as_f64).unwrap_or(0.0) / 1000.0;
        acc.calls += entry.get("calls").and_then(Json::as_i64).unwrap_or(0);
        for caller in entry
            .get("callers")
            .and_then(Json::as_array)
            .unwrap_or_default()
        {
            let raw_caller = caller.get("file").and_then(Json::as_str).unwrap_or("");
            let Some((caller_file, caller_external)) = classify(root, raw_caller) else {
                continue;
            };
            let caller_key = FrameKey {
                file: caller_file,
                function: caller
                    .get("name")
                    .and_then(Json::as_str)
                    .unwrap_or("")
                    .to_string(),
                line: caller.get("line").and_then(Json::as_i64).unwrap_or(0),
                external: caller_external,
            };
            if caller_key.function.is_empty() {
                continue;
            }
            let slot = acc.callers.entry(caller_key).or_default();
            slot.0 += caller.get("total").and_then(Json::as_f64).unwrap_or(0.0) / 1000.0;
            slot.1 += caller.get("calls").and_then(Json::as_i64).unwrap_or(0);
        }
    }
}

// ---- sampled stacks (Go pprof, Java JFR) -----------------------------------------------------

/// Adds sampled stacks, each root first with `None` for frames outside user
/// code, and the time the sample stands for. A function's inclusive time counts
/// once per sample (recursion); its caller is the nearest named user frame above
/// it (lambdas and closures are skipped).
fn merge_samples(samples: &[(Vec<Option<FrameKey>>, f64)], functions: &mut HashMap<FrameKey, Acc>) {
    for (stack, seconds) in samples {
        let mut seen: Vec<&FrameKey> = Vec::new();
        let mut named: Option<&FrameKey> = None;
        for frame in stack {
            let Some(key) = frame else {
                continue;
            };
            if !seen.contains(&key) {
                seen.push(key);
                let acc = functions.entry(key.clone()).or_default();
                acc.cum_seconds += seconds;
                if let Some(caller) = named {
                    if caller != key {
                        acc.callers.entry(caller.clone()).or_default().0 += seconds;
                    }
                }
            }
            if !key.function.starts_with("(anonymous)") {
                named = Some(key);
            }
        }
        if let Some(Some(leaf)) = stack.last() {
            functions.entry(leaf.clone()).or_default().self_seconds += seconds;
        }
    }
}

// ---- go test -cpuprofile ---------------------------------------------------------------------

/// A Go function's own name: `example.com/x/pkg.Slugify` is `Slugify`,
/// `pkg.(*T).M` is `M`; closures (`Outer.func1`) are anonymous.
fn go_name(full: &str) -> String {
    let tail = full.rsplit('/').next().unwrap_or(full);
    let rest = tail.split_once('.').map_or(tail, |(_, rest)| rest);
    let parts: Vec<&str> = rest.split('.').collect();
    match parts.as_slice() {
        [.., outer, function]
            if function.starts_with("func")
                && function[4..].chars().all(|c| c.is_ascii_digit()) =>
        {
            format!("(anonymous) in {outer}")
        }
        [.., last] => last.to_string(),
        [] => rest.to_string(),
    }
}

/// Strips the `:line:column` suffix `go tool pprof` puts after a file.
fn pprof_file(place: &str) -> &str {
    let mut path = place;
    for _ in 0..2 {
        match path.rsplit_once(':') {
            Some((head, tail)) if !tail.is_empty() && tail.chars().all(|c| c.is_ascii_digit()) => {
                path = head;
            }
            _ => break,
        }
    }
    path
}

/// Seconds per unit in the sample type line, e.g. `cpu/nanoseconds`.
fn unit_seconds(unit: &str) -> Option<f64> {
    match unit {
        "nanoseconds" => Some(1e-9),
        "microseconds" | "us" => Some(1e-6),
        "milliseconds" | "ms" => Some(1e-3),
        "seconds" | "s" => Some(1.0),
        _ => None,
    }
}

/// `go tool pprof -raw` output: samples (leaf first, by location id) and
/// locations (each one or more frames, innermost first: `func file:line:col s=start`).
fn parse_pprof_raw(root: &Path, raw: &str) -> Vec<(Vec<Option<FrameKey>>, f64)> {
    let mut locations: HashMap<i64, Vec<Option<FrameKey>>> = HashMap::new();
    let mut samples: Vec<(Vec<i64>, f64)> = Vec::new();
    let mut section = "";
    let mut current: Option<i64> = None;
    // The CPU value is the second column of `samples/count cpu/nanoseconds`;
    // when no unit header is present Go CPU profiles store nanoseconds.
    let mut value_index = 1usize;
    let mut scale = 1e-9f64;
    for line in raw.lines() {
        let text = line.trim_end();
        match text {
            "Samples:" => {
                section = "samples";
                continue;
            }
            "Locations" | "Locations:" => {
                section = "locations";
                continue;
            }
            _ if text.starts_with("Mappings") => {
                section = "";
                continue;
            }
            _ => {}
        }
        match section {
            "samples" if text.contains(':') => {
                let (values, ids) = text.split_once(':').unwrap_or_default();
                let values: Vec<f64> = values
                    .split_whitespace()
                    .filter_map(|value| value.parse().ok())
                    .collect();
                let ids: Vec<i64> = ids
                    .split_whitespace()
                    .filter_map(|id| id.parse().ok())
                    .collect();
                if let Some(value) = values.get(value_index).or(values.last()) {
                    samples.push((ids, value * scale));
                }
            }
            "samples" if text.contains('/') => {
                let kinds: Vec<&str> = text.split_whitespace().collect();
                if let Some(index) = kinds
                    .iter()
                    .position(|kind| kind.ends_with("/nanoseconds"))
                    .or_else(|| {
                        kinds.iter().position(|kind| {
                            unit_seconds(kind.rsplit('/').next().unwrap_or(kind)).is_some()
                        })
                    })
                {
                    value_index = index;
                    let unit = kinds[index].rsplit('/').next().unwrap_or(kinds[index]);
                    scale = unit_seconds(unit).unwrap_or(1e-9);
                }
            }
            "locations" => {
                let trimmed = text.trim_start();
                let frame_text = match trimmed.split_once(": ") {
                    Some((id, rest)) if id.chars().all(|c| c.is_ascii_digit()) => {
                        current = id.parse().ok();
                        // `0x4c2ded M=1 func file:line:col s=start`
                        rest.split_whitespace()
                            .skip(2)
                            .collect::<Vec<_>>()
                            .join(" ")
                    }
                    _ => trimmed.to_string(),
                };
                let Some(id) = current else {
                    continue;
                };
                if let Some(key) = pprof_frame(root, &frame_text) {
                    locations.entry(id).or_default().push(key);
                }
            }
            _ => {}
        }
    }
    samples
        .into_iter()
        .map(|(ids, seconds)| {
            // Leaf first, and inlined frames innermost first: reversed, the
            // stack is root first.
            let mut stack: Vec<Option<FrameKey>> = ids
                .iter()
                .flat_map(|id| locations.get(id).cloned().unwrap_or_default())
                .collect();
            stack.reverse();
            (stack, seconds)
        })
        .collect()
}

/// One frame line inside a pprof location; `None` in the outer option means the
/// frame is runtime/library code that is not reported.
fn pprof_frame(root: &Path, text: &str) -> Option<Option<FrameKey>> {
    let mut parts = text.split_whitespace();
    let function = parts.next()?;
    let place = parts.next()?;
    let start = parts
        .find_map(|part| part.strip_prefix("s="))
        .and_then(|start| start.parse::<i64>().ok())
        .unwrap_or(0);
    let file = pprof_file(place);
    if let Some(goroot) = goroot() {
        if file.starts_with(goroot.as_str()) {
            return Some(None); // Go's runtime and standard library
        }
    }
    Some(classify(root, file).map(|(file, external)| FrameKey {
        file,
        function: go_name(function),
        line: start,
        external,
    }))
}

/// Go: `go test` with `-cpuprofile` (Go has no profiler to attach from
/// outside), read with `go tool pprof -raw`.
fn go(
    root: &Path,
    command: &[String],
    work: &Path,
) -> Result<(HashMap<FrameKey, Acc>, f64), String> {
    if command.get(1).map(String::as_str) != Some("test") {
        return Err(
            "for Go, profile a test or benchmark: `cpp-anything profile . -- go test -run '^$' \
             -bench . ./pkg` (Go profiles only from inside the process; `go test` adds \
             -cpuprofile)"
                .into(),
        );
    }
    let profile = work.join("cpu.pprof");
    let mut args = vec![
        "test".to_string(),
        format!("-cpuprofile={}", profile.display()),
    ];
    args.extend(command.iter().skip(2).cloned());
    let run = util::run(&command[0], &args, Some(root));
    if !run.ok {
        return Err(format!("the workload failed ({})", run.failure()));
    }
    if !profile.is_file() {
        return Err(format!(
            "no CPU profile was written to {}: does the package have tests to run?",
            profile.display()
        ));
    }
    let raw = util::run(
        &command[0],
        &[
            "tool".to_string(),
            "pprof".to_string(),
            "-raw".to_string(),
            profile.display().to_string(),
        ],
        Some(root),
    );
    if !raw.ok {
        return Err(format!("go tool pprof failed: {}", raw.stderr.trim()));
    }
    let samples = parse_pprof_raw(root, &raw.stdout);
    let total_seconds: f64 = samples.iter().map(|(_, seconds)| seconds).sum();
    let mut functions: HashMap<FrameKey, Acc> = HashMap::new();
    merge_samples(&samples, &mut functions);
    Ok((functions, total_seconds))
}

// ---- Java Flight Recorder --------------------------------------------------------------------

/// The line declaring method `name` in Java source: the nearest declaration at
/// or above `near`.
fn java_declaration(text: &str, name: &str, near: i64) -> Option<i64> {
    let needle = format!("{name}(");
    let mut best = None;
    for (index, line) in text.lines().enumerate() {
        let number = index as i64 + 1;
        if number > near.max(1) {
            break;
        }
        let Some(at) = line.find(&needle) else {
            continue;
        };
        let before = line[..at].trim_end();
        // A declaration names a type or modifier before the method: `static int pages(`.
        let declares = before
            .ends_with(|c: char| c.is_alphanumeric() || matches!(c, '>' | ']' | '_'))
            && !before.contains('=')
            && !before.contains("return")
            && !before.contains("new ")
            && !before.ends_with('.')
            && !line.trim_end().ends_with(';');
        if declares && at > 0 && line.as_bytes()[at - 1].is_ascii_whitespace() {
            best = Some(number);
        }
    }
    best
}

fn skip_java_dir(name: &str) -> bool {
    matches!(
        name,
        "target"
            | "build"
            | "out"
            | "bin"
            | ".git"
            | "node_modules"
            | ".gradle"
            | ".idea"
            | ".mvn"
            | ".venv"
            | "venv"
            | "__pycache__"
    )
}

fn collect_java(dir: &Path, root: &Path, out: &mut HashMap<String, String>, depth: usize) {
    if depth > 16 {
        return;
    }
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        if kind.is_dir() {
            if entry
                .file_name()
                .to_str()
                .is_some_and(|name| skip_java_dir(name))
            {
                continue;
            }
            collect_java(&path, root, out, depth + 1);
            continue;
        }
        if path.extension().is_none_or(|extension| extension != "java") {
            continue;
        }
        let Ok(relative) = path.strip_prefix(root) else {
            continue;
        };
        let relative = relative.to_string_lossy().replace('\\', "/");
        let text = std::fs::read_to_string(&path).unwrap_or_default();
        let package = text
            .lines()
            .find_map(|line| {
                line.trim()
                    .strip_prefix("package ")
                    .map(|rest| rest.trim_end_matches(';').trim().to_string())
            })
            .unwrap_or_default();
        let prefix = if package.is_empty() {
            String::new()
        } else {
            format!("{}/", package.replace('.', "/"))
        };
        let stem = path
            .file_stem()
            .map(|stem| stem.to_string_lossy().into_owned())
            .unwrap_or_default();
        if !stem.is_empty() {
            out.insert(format!("{prefix}{stem}"), relative.clone());
        }
        // Other (often non-public) top-level types in the same file still get
        // their own class name in samples.
        for line in text.lines() {
            for keyword in ["class ", "interface ", "record ", "enum "] {
                let Some(at) = line.find(keyword) else {
                    continue;
                };
                let before = &line[..at];
                if before
                    .chars()
                    .last()
                    .is_some_and(|c| c.is_alphanumeric() || c == '_' || c == '.' || c == '$')
                {
                    continue; // `Foo.class`, `a.classify(`, not a declaration
                }
                let name: String = line[at + keyword.len()..]
                    .trim_start()
                    .chars()
                    .take_while(|c| c.is_alphanumeric() || *c == '_' || *c == '$')
                    .collect();
                if !name.is_empty() {
                    out.insert(format!("{prefix}{name}"), relative.clone());
                }
            }
        }
    }
}

/// Java: Flight Recorder's execution samples, read with `jfr print --json`.
fn java(
    root: &Path,
    command: &[String],
    work: &Path,
) -> Result<(HashMap<FrameKey, Acc>, f64), String> {
    let recording = work.join("profile.jfr");
    let options = format!(
        "{} -XX:StartFlightRecording=filename={},settings=profile \
         -XX:FlightRecorderOptions=stackdepth=256",
        std::env::var("JAVA_TOOL_OPTIONS")
            .unwrap_or_default()
            .trim(),
        recording.display()
    );
    let run = util::run_with_env(
        &command[0],
        &command[1..],
        Some(root),
        &[("JAVA_TOOL_OPTIONS".to_string(), options.trim().to_string())],
    );
    if !run.ok {
        return Err(format!("the workload failed ({})", run.failure()));
    }
    if !recording.is_file() {
        return Err(
            "no Flight Recorder file was written: is the workload a Java process launched by `java`?"
                .into(),
        );
    }
    let printed = util::run(
        "jfr",
        &[
            "print".to_string(),
            "--json".to_string(),
            "--stack-depth".to_string(),
            "256".to_string(),
            "--events".to_string(),
            "jdk.ExecutionSample".to_string(),
            recording.display().to_string(),
        ],
        Some(root),
    );
    if !printed.ok {
        return Err(format!(
            "jfr print failed (it comes with the JDK): {}",
            printed.stderr.trim()
        ));
    }
    let doc = Json::parse(printed.stdout.trim())
        .map_err(|error| format!("reading the Flight Recorder output: {error}"))?;
    let events: &[Json] = doc
        .get("recording")
        .and_then(|recording| recording.get("events"))
        .and_then(Json::as_array)
        .unwrap_or_default();
    let mut sources: HashMap<String, String> = HashMap::new();
    collect_java(root, root, &mut sources, 0);
    // The `profile` settings sample Java threads every 10 ms.
    const SAMPLE_SECONDS: f64 = 0.010;
    let mut stacks: Vec<Vec<Option<FrameKey>>> = Vec::new();
    let mut sample_events = 0usize;
    for event in events {
        if event
            .get("type")
            .and_then(Json::as_str)
            .is_some_and(|kind| kind != "jdk.ExecutionSample")
        {
            continue;
        }
        sample_events += 1;
        let frames = event
            .get("values")
            .and_then(|values| values.get("stackTrace"))
            .and_then(|trace| trace.get("frames"))
            .and_then(Json::as_array)
            .unwrap_or_default();
        let mut stack: Vec<Option<FrameKey>> = Vec::new();
        for frame in frames {
            let method = frame.get("method");
            let class = method
                .and_then(|method| method.get("type"))
                .and_then(|kind| kind.get("name"))
                .and_then(Json::as_str)
                .unwrap_or("");
            let outer = class.split('$').next().unwrap_or(class);
            let Some(file) = sources.get(outer) else {
                continue; // JDK and third-party classes: nothing to port
            };
            let name = method
                .and_then(|method| method.get("name"))
                .and_then(Json::as_str)
                .unwrap_or("");
            if name.is_empty() {
                continue;
            }
            let line = frame.get("lineNumber").and_then(Json::as_i64).unwrap_or(0);
            let function = if name.starts_with("lambda$") || name.starts_with('$') {
                format!("(anonymous) {name}")
            } else {
                name.to_string()
            };
            stack.push(Some(FrameKey {
                file: file.clone(),
                function,
                line,
                external: false,
            }));
        }
        stack.reverse();
        stacks.push(stack);
    }
    // Key each method by its declaration line, found from the lowest sampled line.
    let mut lowest: HashMap<(String, String), i64> = HashMap::new();
    for (file, function, line) in stacks
        .iter()
        .flatten()
        .flatten()
        .map(|key| (key.file.clone(), key.function.clone(), key.line))
    {
        let slot = lowest.entry((file, function)).or_insert(line);
        *slot = (*slot).min(line);
    }
    let mut texts: HashMap<String, String> = HashMap::new();
    let resolved: Vec<(Vec<Option<FrameKey>>, f64)> = stacks
        .into_iter()
        .map(|stack| {
            let stack = stack
                .into_iter()
                .map(|frame| {
                    let mut key = frame?;
                    if key.function.starts_with("(anonymous)") {
                        return Some(key);
                    }
                    let near = lowest
                        .get(&(key.file.clone(), key.function.clone()))
                        .copied()
                        .unwrap_or(key.line);
                    let text = texts.entry(key.file.clone()).or_insert_with(|| {
                        std::fs::read_to_string(root.join(&key.file)).unwrap_or_default()
                    });
                    key.line = if key.function.starts_with('<') {
                        near
                    } else {
                        java_declaration(text, &key.function, near).unwrap_or(near)
                    };
                    Some(key)
                })
                .collect();
            (stack, SAMPLE_SECONDS)
        })
        .collect();
    let total_seconds = sample_events as f64 * SAMPLE_SECONDS;
    let mut functions: HashMap<FrameKey, Acc> = HashMap::new();
    merge_samples(&resolved, &mut functions);
    Ok((functions, total_seconds))
}

// ---- the report ------------------------------------------------------------------------------

/// The rules an experienced porter applies, written down:
/// a cheap leaf called in a loop is not worth one call per item; batching is.
fn advice(entry: &Entry, total_seconds: f64) -> String {
    if entry.harness {
        return "the workload harness: not part of the port; profile what it calls".into();
    }
    if entry.external {
        return "third-party or stdlib code: not yours to port; port your callers instead".into();
    }
    if entry.calls >= 500 && entry.per_call_ns < 2_000.0 {
        return format!(
            "called {} times at {:.0} ns each: one call per item costs more than the work, port it \
             with its callers or batch it (`bridge <repo> <unit> --batch {}:<param>`)",
            entry.calls, entry.per_call_ns, entry.function
        );
    }
    if entry.calls >= 500 {
        return format!(
            "hot leaf ({} calls, {:.0} ns each): batch it (`--batch {}:<param>`) so the loop stays on \
             the C++ side",
            entry.calls, entry.per_call_ns, entry.function
        );
    }
    if entry.share >= 0.10 {
        return format!(
            "top cost ({:.0}% of the workload): port this first, then `diff` and `bench`",
            entry.share * 100.0
        );
    }
    // Sampling profilers see inlined frames only as callers: their own time is
    // attributed to the leaf, but their inclusive time is still the best signal.
    if entry.self_seconds <= 0.0 && total_seconds > 0.0 && entry.cum_seconds / total_seconds >= 0.10
    {
        return format!(
            "top cost inclusive ({:.0}% of the time sampled here, mostly in what it calls): port \
             this first, then `diff` and `bench`",
            entry.cum_seconds / total_seconds * 100.0
        );
    }
    "port with its callers; alone it is unlikely to pay for the crossing".into()
}

pub fn render(report: &Report) -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "cpp-anything profile: {} — {:.2} s of self time across {} function(s)\n",
        if report.workload.is_empty() {
            "workload".to_string()
        } else {
            report.workload.join(" ")
        },
        report.total_seconds,
        report.entries.len()
    ));
    out.push_str("\n   self%     calls     per call   function (file:line)\n");
    if report.entries.is_empty() {
        out.push_str(
            "\nNo functions were found in the profile: run the workload longer, or all of its time \
             is outside the repository.\n",
        );
        out.push_str(
            "\nNext: `cpp-anything analyze .`, then `cpp-anything bridge . <unit>` for the unit the \
             hot function lives in.\n",
        );
        return out;
    }
    for entry in &report.entries {
        out.push_str(&format!(
            "  {:>5.1}%  {:>9}  {:>10}   {}.{}  {}:{}\n",
            entry.share * 100.0,
            entry.calls,
            format_ns(entry.per_call_ns),
            module_of(&entry.file),
            entry.function,
            util::short_path(Path::new(&entry.file)),
            entry.line
        ));
        if entry.external || entry.harness {
            out.push_str(&format!("            {}\n", entry.advice));
            continue;
        }
        if !entry.callers.is_empty() {
            out.push_str(&format!(
                "            called from: {}\n",
                entry
                    .callers
                    .iter()
                    .take(3)
                    .cloned()
                    .collect::<Vec<_>>()
                    .join(", ")
            ));
        }
        out.push_str(&format!("            {}\n", entry.advice));
    }
    if report.entries.iter().all(|entry| entry.external) {
        out.push_str(
            "\nEverything hot is in a dependency: profile the code that calls it, or the work is \
             not in the functions you own.\n",
        );
    }
    out.push_str(
        "\nNext: `cpp-anything analyze .`, then `cpp-anything bridge . <unit>` for the unit the hot \
         function lives in.\n",
    );
    out
}

fn script_of(workload: &[String]) -> String {
    workload
        .iter()
        .find(|part| part.ends_with(".py"))
        .cloned()
        .unwrap_or_default()
}

fn module_of(file: &str) -> String {
    Path::new(file)
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn format_ns(nanos: f64) -> String {
    if !nanos.is_finite() || nanos <= 0.0 {
        return "0 ns".to_string();
    }
    if nanos >= 1_000_000.0 {
        format!("{:.2} ms", nanos / 1_000_000.0)
    } else if nanos >= 1_000.0 {
        format!("{:.2} µs", nanos / 1_000.0)
    } else {
        format!("{nanos:.0} ns")
    }
}

pub fn to_json(report: &Report, root: &Path) -> Json {
    Json::obj([
        ("schema", Json::str("cpp-anything/profile@1")),
        ("root", Json::str(root.display().to_string())),
        (
            "workload",
            Json::Array(
                report
                    .workload
                    .iter()
                    .map(|part| Json::str(part.clone()))
                    .collect(),
            ),
        ),
        ("self_seconds", Json::Real(report.total_seconds)),
        (
            "functions",
            Json::Array(
                report
                    .entries
                    .iter()
                    .map(|entry| {
                        Json::obj([
                            ("function", Json::str(entry.function.clone())),
                            ("file", Json::str(entry.file.clone())),
                            ("line", Json::Int(entry.line)),
                            ("calls", Json::Int(entry.calls)),
                            ("self_seconds", Json::Real(entry.self_seconds)),
                            ("cum_seconds", Json::Real(entry.cum_seconds)),
                            ("per_call_ns", Json::Real(entry.per_call_ns)),
                            ("share", Json::Real(entry.share)),
                            (
                                "callers",
                                Json::Array(
                                    entry
                                        .callers
                                        .iter()
                                        .map(|name| Json::str(name.clone()))
                                        .collect(),
                                ),
                            ),
                            ("external", Json::Bool(entry.external)),
                            ("advice", Json::str(entry.advice.clone())),
                        ])
                    })
                    .collect(),
            ),
        ),
    ])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_root(tag: &str) -> std::path::PathBuf {
        let root =
            std::env::temp_dir().join(format!("cpp-anything-profile-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        root
    }

    #[test]
    fn runtime_dispatch_is_by_program_name() {
        assert!(matches!(runtime_of("node"), Runtime::Node));
        assert!(matches!(runtime_of("nodejs"), Runtime::Node));
        assert!(matches!(runtime_of("ruby"), Runtime::Ruby));
        assert!(matches!(runtime_of("ruby3.3"), Runtime::Ruby));
        assert!(matches!(runtime_of("go"), Runtime::Go));
        assert!(matches!(runtime_of("java"), Runtime::Java));
        assert!(matches!(runtime_of("python3"), Runtime::Python));
        assert!(matches!(runtime_of("bash"), Runtime::Python));
    }

    #[test]
    fn node_frames_are_relative_and_library_frames_external() {
        let root = temp_root("rel");
        std::fs::write(root.join("bench.js"), "").unwrap();
        let root = root.canonicalize().unwrap();
        assert_eq!(
            classify(&root, "file:///tmp/bench.js")
                .map(|(file, external)| (file.ends_with("bench.js"), external)),
            Some((true, true))
        );
        let url = format!("file://{}", root.join("bench.js").display());
        assert_eq!(classify(&root, &url), Some(("bench.js".to_string(), false)));
        assert_eq!(classify(&root, "node:internal/main"), None);
        assert_eq!(
            classify(&root, "file:///tmp/x/node_modules/y.js").map(|(_, e)| e),
            Some(true)
        );
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn a_cpuprofile_yields_self_inclusive_time_and_callers() {
        let root = temp_root("cpuprofile");
        std::fs::write(root.join("bench.js"), "").unwrap();
        let root = root.canonicalize().unwrap();
        let url = format!("file://{}", root.join("bench.js").display());
        let profile = Json::parse(&format!(
            r#"{{"startTime":0,"endTime":40000,
                 "nodes":[
                   {{"id":1,"callFrame":{{"functionName":"(root)","url":"","lineNumber":-1}},
                     "children":[2]}},
                   {{"id":2,"callFrame":{{"functionName":"","url":"{url}","lineNumber":1}},
                     "hitCount":1,"children":[3]}},
                   {{"id":3,"callFrame":{{"functionName":"hot","url":"{url}","lineNumber":4}},
                     "hitCount":3}}
                 ],
                 "samples":[2,3,3,3]}}"#
        ))
        .unwrap();
        let mut functions = HashMap::new();
        let duration = merge_cpuprofile(&root, &profile, &mut functions);
        assert!((duration - 0.04).abs() < 1e-9, "{duration}");
        let hot = functions
            .iter()
            .find(|(key, _)| key.function == "hot")
            .map(|(_, acc)| acc)
            .expect("hot frame");
        assert!(
            (hot.self_seconds - 0.03).abs() < 1e-9,
            "{}",
            hot.self_seconds
        );
        assert!((hot.cum_seconds - 0.03).abs() < 1e-9, "{}", hot.cum_seconds);
        let report = assemble(&["node".into(), "bench.js".into()], functions, duration, 10);
        let hot = report
            .entries
            .iter()
            .find(|entry| entry.function == "hot")
            .expect("hot entry");
        assert!(hot.share > 0.7, "{}", hot.share);
        assert_eq!(hot.callers.first().map(String::as_str), None);
        assert_eq!(hot.file, "bench.js");
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn pprof_raw_stacks_are_read_root_first_with_nanoseconds() {
        let root = temp_root("pprof");
        let raw = "\
PeriodType: cpu nanoseconds
SampleType: samples/count cpu/nanoseconds
Samples:
samples/count cpu/nanoseconds
          1   20000000: 1 2
Locations
     1: 0x1 M=1 example.com/x.Hot /repo/hot.go:11:0 s=8
     2: 0x2 M=1 example.com/x.Bench /repo/bench_test.go:18:0 s=16
";
        let samples = parse_pprof_raw(&root, raw);
        assert_eq!(samples.len(), 1);
        assert!((samples[0].1 - 0.02).abs() < 1e-9, "{}", samples[0].1);
        let names: Vec<&str> = samples[0]
            .0
            .iter()
            .flatten()
            .map(|key| key.function.as_str())
            .collect();
        assert_eq!(names, vec!["Bench", "Hot"]);
        let mut functions = HashMap::new();
        merge_samples(&samples, &mut functions);
        let hot = functions
            .iter()
            .find(|(key, _)| key.function == "Hot")
            .map(|(_, acc)| acc)
            .expect("Hot");
        assert!((hot.cum_seconds - 0.02).abs() < 1e-9);
        assert!((hot.self_seconds - 0.02).abs() < 1e-9);
        let bench = functions
            .iter()
            .find(|(key, _)| key.function == "Bench")
            .map(|(_, acc)| acc)
            .expect("Bench");
        assert!((bench.cum_seconds - 0.02).abs() < 1e-9);
        assert_eq!(bench.self_seconds, 0.0);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn go_closure_names_are_anonymous() {
        assert_eq!(go_name("example.com/x/pkg.Slugify"), "Slugify");
        assert_eq!(go_name("pkg.(*T).M"), "M");
        assert_eq!(go_name("main.main"), "main");
        assert_eq!(go_name("pkg.Outer.func1"), "(anonymous) in Outer");
    }

    #[test]
    fn java_declarations_are_found_above_the_sample() {
        let text =
            "class Hot {\n    static double hot(int n) {\n        double x = hot(2);\n    }\n}\n";
        assert_eq!(java_declaration(text, "hot", 3), Some(2));
        assert_eq!(java_declaration(text, "hot", 1), None);
    }

    #[test]
    fn empty_reports_do_not_panic() {
        let report = assemble(&["go".into(), "test".into()], HashMap::new(), 0.0, 5);
        assert!(report.entries.is_empty());
        let text = render(&report);
        assert!(text.contains("No functions were found"), "{text}");
    }
}
