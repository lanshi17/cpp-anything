//! Go: a unit is a package. The analyzer reads exported functions and their Go
//! types; the recorder builds a harness inside the module; the shadow binding
//! uses cgo over the same C ABI.

use std::fs;
use std::path::{Component, Path, PathBuf};

use crate::json::Json;
use crate::project::Project;
use crate::types::split_top_level;
use crate::util;

use super::{Analysis, Found, FoundParam, RecordOptions};

// ---------------------------------------------------------------------------
// analyzer
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
struct GoParam {
    name: Option<String>,
    ty: String,
}

#[derive(Clone, Debug)]
struct GoResult {
    ty: String,
}

#[derive(Clone, Debug)]
struct GoFunction {
    name: String,
    line: usize,
    params: Vec<GoParam>,
    results: Vec<GoResult>,
    generic: bool,
    receiver: bool,
    malformed: Option<String>,
}

pub fn analyze_package(directory: &Path) -> Result<Analysis, String> {
    let files = go_files(directory)?;
    let mut functions = Vec::new();
    let mut skipped = Vec::new();
    let mut imports = Vec::new();
    let mut lines = 0u32;

    for file in &files {
        let source = util::read(file)?;
        lines += source.lines().count() as u32;
        collect_imports(&source, &mut imports);
        let source_lines: Vec<&str> = source.lines().collect();
        for function in scan_functions(&source) {
            if function.receiver {
                continue;
            }
            let name = function.name.clone();
            if name.is_empty() {
                continue;
            }
            if !is_exported(&name) {
                continue;
            }
            if let Some(reason) = function.malformed.clone() {
                skipped.push((name, reason));
                continue;
            }
            if function.generic {
                skipped.push((name, "generic functions have no wire form".into()));
                continue;
            }
            let mut params = Vec::new();
            let mut reason = None;
            for (index, param) in function.params.iter().enumerate() {
                match map_go_type(&param.ty) {
                    Ok(ty) => params.push(FoundParam {
                        name: param
                            .name
                            .clone()
                            .filter(|name| !name.is_empty() && name != "_")
                            .unwrap_or_else(|| format!("arg{index}")),
                        ty,
                        has_default: false,
                        default: None,
                    }),
                    Err(error) => {
                        let label = param.name.clone().unwrap_or_else(|| param.ty.clone());
                        reason = Some(format!("parameter `{label}`: {error}"));
                        break;
                    }
                }
            }
            if let Some(reason) = reason {
                skipped.push((name, reason));
                continue;
            }
            if function.results.len() > 1 {
                let types = function
                    .results
                    .iter()
                    .map(|result| result.ty.clone())
                    .collect::<Vec<_>>()
                    .join(", ");
                skipped.push((
                    name,
                    format!("multiple results `({types})`: the wire carries one value"),
                ));
                continue;
            }
            let returns = match function.results.first() {
                None => "None".to_string(),
                Some(result) => match map_go_type(&result.ty) {
                    Ok(ty) => ty,
                    Err(error) => {
                        skipped.push((name, format!("result: {error}")));
                        continue;
                    }
                },
            };
            let doc = doc_comment(&source_lines, function.line);
            functions.push(Found {
                name,
                line: function.line as u32 + 1,
                call: String::new(),
                params,
                returns,
                doc,
            });
        }
    }

    for function in &mut functions {
        function.call = function.name.clone();
    }
    imports.sort();
    imports.dedup();

    Ok(Analysis {
        functions,
        skipped,
        imports,
        lines,
    })
}

fn go_files(directory: &Path) -> Result<Vec<PathBuf>, String> {
    let entries =
        fs::read_dir(directory).map_err(|error| format!("{}: {error}", directory.display()))?;
    let mut files = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with('.') || name.starts_with('_') {
            continue;
        }
        if !name.ends_with(".go") || name.ends_with("_test.go") {
            continue;
        }
        files.push(path);
    }
    files.sort();
    Ok(files)
}

fn is_exported(name: &str) -> bool {
    name.chars().next().is_some_and(char::is_uppercase)
}

fn collect_imports(source: &str, imports: &mut Vec<String>) {
    let mut block = false;
    for line in source.lines() {
        let trimmed = line.trim();
        if block {
            if trimmed.starts_with(')') {
                block = false;
                continue;
            }
            if let Some(path) = quoted_path(trimmed) {
                imports.push(path);
            }
            continue;
        }
        let Some(rest) = trimmed.strip_prefix("import") else {
            continue;
        };
        if !rest.starts_with(|ch: char| ch.is_whitespace() || ch == '(') {
            continue;
        }
        let rest = rest.trim_start();
        if rest.starts_with('(') {
            block = true;
        } else if let Some(path) = quoted_path(rest) {
            imports.push(path);
        }
    }
}

fn quoted_path(text: &str) -> Option<String> {
    let start = text.find('"')?;
    let end = text[start + 1..].find('"')? + start + 1;
    Some(text[start + 1..end].to_string())
}

fn doc_comment(lines: &[&str], index: usize) -> String {
    let mut out = Vec::new();
    let mut at = index;
    while at > 0 {
        at -= 1;
        let line = lines[at].trim();
        let Some(text) = line.strip_prefix("//") else {
            break;
        };
        out.push(text.trim().to_string());
    }
    out.reverse();
    out.join(" ")
}

fn strip_line_comment(line: &str) -> &str {
    match line.find("//") {
        Some(position) => &line[..position],
        None => line,
    }
}

fn starts_func(trimmed: &str) -> bool {
    match trimmed.strip_prefix("func") {
        Some(rest) => {
            rest.is_empty() || rest.starts_with(|ch: char| ch.is_whitespace() || ch == '(')
        }
        None => false,
    }
}

/// Function declarations, one per exported or unexported top-level `func`.
/// Signatures that span lines are joined until the body's `{`.
fn scan_functions(source: &str) -> Vec<GoFunction> {
    let lines: Vec<&str> = source.lines().collect();
    let mut out = Vec::new();
    let mut index = 0usize;
    while index < lines.len() {
        let start = index;
        if !starts_func(lines[index].trim_start()) {
            index += 1;
            continue;
        }
        let mut signature = String::new();
        let mut cursor = index;
        let mut parens = 0i32;
        let mut brackets = 0i32;
        let mut seen_params = false;
        let mut finished = false;
        while cursor < lines.len() {
            let content = strip_line_comment(lines[cursor]);
            for ch in content.chars() {
                if ch == '{' && parens == 0 && brackets == 0 && seen_params {
                    finished = true;
                    break;
                }
                signature.push(ch);
                match ch {
                    '(' => parens += 1,
                    ')' => {
                        parens -= 1;
                        if parens == 0 {
                            seen_params = true;
                        }
                    }
                    '[' => brackets += 1,
                    ']' => brackets -= 1,
                    _ => {}
                }
            }
            if finished {
                break;
            }
            signature.push('\n');
            cursor += 1;
        }
        out.push(parse_signature(&signature, start));
        index = cursor.max(start) + 1;
    }
    out
}

fn split_pair(text: &str, open: char, close: char) -> Option<(String, String)> {
    if !text.starts_with(open) {
        return None;
    }
    let mut depth = 0i32;
    for (position, ch) in text.char_indices() {
        if ch == open {
            depth += 1;
        } else if ch == close {
            depth -= 1;
            if depth == 0 {
                let inner = text[open.len_utf8()..position].to_string();
                let after = text[position + close.len_utf8()..].to_string();
                return Some((inner, after));
            }
        }
    }
    None
}

fn parse_signature(signature: &str, line: usize) -> GoFunction {
    let mut function = GoFunction {
        name: String::new(),
        line,
        params: Vec::new(),
        results: Vec::new(),
        generic: false,
        receiver: false,
        malformed: None,
    };
    let text = signature.trim().to_string();
    let mut rest = text
        .strip_prefix("func")
        .unwrap_or(&text)
        .trim_start()
        .to_string();
    if rest.starts_with('(') {
        function.receiver = true;
        match split_pair(&rest, '(', ')') {
            Some((_, after)) => rest = after.trim_start().to_string(),
            None => {
                function.malformed = Some("an unclosed method receiver".into());
                return function;
            }
        }
    }
    let name_end = rest
        .find(|ch: char| ch.is_whitespace() || ch == '[' || ch == '(')
        .unwrap_or(rest.len());
    let name = rest[..name_end].to_string();
    function.name = name.clone();
    rest = rest[name_end..].trim_start().to_string();
    if name.is_empty() || !name.chars().all(|ch| ch.is_alphanumeric() || ch == '_') {
        function.malformed = Some("the function name is not a plain identifier".into());
        return function;
    }
    if rest.starts_with('[') {
        function.generic = true;
        match split_pair(&rest, '[', ']') {
            Some((_, after)) => rest = after.trim_start().to_string(),
            None => {
                function.malformed = Some("an unclosed type parameter list".into());
                return function;
            }
        }
    }
    if !rest.starts_with('(') {
        function.malformed = Some("a parameter list is missing".into());
        return function;
    }
    let Some((inside, after)) = split_pair(&rest, '(', ')') else {
        function.malformed = Some("an unclosed parameter list".into());
        return function;
    };
    match parse_params(&inside) {
        Ok(params) => function.params = params,
        Err(error) => function.malformed = Some(error),
    }
    let results = after.trim();
    if !results.is_empty() {
        match parse_results(results) {
            Ok(parsed) => function.results = parsed,
            Err(error) => function.malformed = Some(error),
        }
    }
    function
}

fn split_name_type(part: &str) -> Option<(String, String)> {
    let position = part.find(char::is_whitespace)?;
    let (name, ty) = part.split_at(position);
    let ty = ty.trim();
    if ty.is_empty() || name.is_empty() {
        return None;
    }
    if !name
        .chars()
        .all(|ch| ch.is_ascii_alphanumeric() || ch == '_')
    {
        return None;
    }
    if !name
        .chars()
        .next()
        .is_some_and(|ch| ch.is_ascii_alphabetic() || ch == '_')
    {
        return None;
    }
    Some((name.to_string(), ty.to_string()))
}

fn parse_params(text: &str) -> Result<Vec<GoParam>, String> {
    let mut out = Vec::new();
    let mut pending: Vec<String> = Vec::new();
    for part in split_top_level(text, ',') {
        let part = part.trim();
        if part.is_empty() {
            continue;
        }
        if part.starts_with("...") {
            return Err("variadic parameters have no wire form".into());
        }
        match split_name_type(part) {
            Some((name, ty)) => {
                if ty.starts_with("...") {
                    return Err(format!("variadic parameter `{name}` has no wire form"));
                }
                pending.push(name);
                for name in pending.drain(..) {
                    out.push(GoParam {
                        name: Some(name),
                        ty: ty.clone(),
                    });
                }
            }
            None => pending.push(part.to_string()),
        }
    }
    if !pending.is_empty() {
        if out.is_empty() {
            for ty in pending {
                out.push(GoParam { name: None, ty });
            }
        } else {
            return Err(format!("parameter `{}` has no type", pending.join(", ")));
        }
    }
    Ok(out)
}

fn parse_results(text: &str) -> Result<Vec<GoResult>, String> {
    let text = text.trim();
    if text.is_empty() {
        return Ok(Vec::new());
    }
    if text.starts_with('(') {
        let Some((inside, after)) = split_pair(text, '(', ')') else {
            return Err("an unclosed result list".into());
        };
        if !after.trim().is_empty() {
            return Err(format!("`{}` after the result list", after.trim()));
        }
        let mut out = Vec::new();
        let mut pending: Vec<String> = Vec::new();
        for part in split_top_level(&inside, ',') {
            let part = part.trim();
            if part.is_empty() {
                continue;
            }
            match split_name_type(part) {
                Some((name, ty)) => {
                    pending.push(name);
                    for _ in pending.drain(..) {
                        out.push(GoResult { ty: ty.clone() });
                    }
                }
                None => pending.push(part.to_string()),
            }
        }
        if !pending.is_empty() {
            if out.is_empty() {
                for ty in pending {
                    out.push(GoResult { ty });
                }
            } else {
                return Err(format!("result `{}` has no type", pending.join(", ")));
            }
        }
        return Ok(out);
    }
    Ok(vec![GoResult {
        ty: text.to_string(),
    }])
}

fn map_go_type(text: &str) -> Result<String, String> {
    let text = text.trim();
    if text.is_empty() {
        return Err("an empty type".into());
    }
    match text {
        "int" | "int8" | "int16" | "int32" | "int64" | "uint" | "uint8" | "uint16" | "uint32"
        | "uint64" | "uintptr" | "byte" | "rune" => return Ok("int".into()),
        "float32" | "float64" => return Ok("float".into()),
        "string" => return Ok("str".into()),
        "bool" => return Ok("bool".into()),
        "error" => {
            return Err("the `error` type has no wire form (a nil error is not a value)".into())
        }
        _ => {}
    }
    if let Some(inner) = text.strip_prefix("[]") {
        return Ok(format!("list[{}]", map_go_type(inner)?));
    }
    if let Some(inner) = text.strip_prefix("map[") {
        let mut depth = 1i32;
        let mut end = None;
        for (position, ch) in inner.char_indices() {
            match ch {
                '[' => depth += 1,
                ']' => {
                    depth -= 1;
                    if depth == 0 {
                        end = Some(position);
                        break;
                    }
                }
                _ => {}
            }
        }
        let Some(end) = end else {
            return Err("an unclosed map type".into());
        };
        let key = inner[..end].trim();
        let value = inner[end + 1..].trim();
        if key != "string" {
            return Err(format!("map key `{key}` is not string"));
        }
        return Ok(format!("dict[{}]", map_go_type(value)?));
    }
    if text.starts_with("...") {
        return Err("variadic parameters have no wire form".into());
    }
    if text.starts_with('*') {
        return Err("pointer types do not cross the wire".into());
    }
    if text.starts_with('[') {
        return Err("fixed-size arrays do not cross the wire".into());
    }
    if text.starts_with("func") {
        return Err("function values do not cross the wire".into());
    }
    if text.starts_with("chan") || text.starts_with("<-chan") {
        return Err("channels do not cross the wire".into());
    }
    if text == "any" || text.starts_with("interface") {
        return Err("interface types do not cross the wire".into());
    }
    if text.starts_with("struct") {
        return Err("struct types do not cross the wire".into());
    }
    if text.contains('|') || text.contains('~') {
        return Err("type constraints have no wire form".into());
    }
    if text.starts_with("complex") {
        return Err("complex numbers do not cross the wire".into());
    }
    Err(format!("`{text}` is not a type the wire carries"))
}

// ---------------------------------------------------------------------------
// module resolution and the out-of-process harness
// ---------------------------------------------------------------------------

fn find_module_root(package_dir: &Path) -> Option<PathBuf> {
    let mut current = Some(package_dir);
    while let Some(directory) = current {
        if directory.join("go.mod").is_file() {
            return Some(directory.to_path_buf());
        }
        current = directory.parent();
    }
    None
}

fn module_path(root: &Path) -> Result<String, String> {
    let text = util::read(&root.join("go.mod"))?;
    for line in text.lines() {
        let trimmed = line.trim();
        let Some(rest) = trimmed.strip_prefix("module ") else {
            continue;
        };
        let module = rest
            .split_whitespace()
            .next()
            .unwrap_or("")
            .trim_matches('"');
        if !module.is_empty() {
            return Ok(module.to_string());
        }
    }
    Err(format!("{}: no module line", root.join("go.mod").display()))
}

fn relative_import(module: &str, root: &Path, package_dir: &Path) -> String {
    let relative = package_dir.strip_prefix(root).unwrap_or(Path::new(""));
    let parts = relative
        .components()
        .filter_map(|component| match component {
            Component::Normal(part) => Some(part.to_string_lossy().into_owned()),
            _ => None,
        })
        .collect::<Vec<_>>();
    if parts.is_empty() {
        module.to_string()
    } else {
        format!("{module}/{}", parts.join("/"))
    }
}

fn import_path(package_dir: &Path) -> Result<String, String> {
    let root = find_module_root(package_dir).ok_or_else(|| {
        format!(
            "{}: no go.mod above this package; the Go binding needs the module path",
            package_dir.display()
        )
    })?;
    let module = module_path(&root)?;
    Ok(relative_import(&module, &root, package_dir))
}

struct Harness {
    import: String,
    directory: PathBuf,
    run: String,
    cwd: PathBuf,
    temporary: Vec<PathBuf>,
}

/// Picks where the harness lives: inside the package's module when it has one,
/// else a throwaway module whose `replace` points at the repository.
fn locate_harness(project: &Project, package_dir: &Path, ns: &str) -> Result<Harness, String> {
    if let Some(root) = find_module_root(package_dir) {
        let module = module_path(&root)?;
        let directory = root.join(format!("cppan-record-{ns}"));
        if directory.exists() {
            fs::remove_dir_all(&directory)
                .map_err(|error| format!("{}: {error}", directory.display()))?;
        }
        return Ok(Harness {
            import: relative_import(&module, &root, package_dir),
            run: format!("./cppan-record-{ns}"),
            cwd: root,
            directory,
            temporary: Vec::new(),
        });
    }

    let repo = project
        .package_root
        .clone()
        .filter(|root| package_dir.starts_with(root))
        .unwrap_or_else(|| package_dir.to_path_buf());
    let module_file = repo.join("go.mod");
    let created_module = !module_file.exists();
    if created_module {
        util::write(&module_file, "module cppanoriginal\n\ngo 1.21\n")?;
    }
    let directory = project.dir.join("build").join(format!("gorecord-{ns}"));
    if directory.exists() {
        fs::remove_dir_all(&directory)
            .map_err(|error| format!("{}: {error}", directory.display()))?;
    }
    let relative = package_dir.strip_prefix(&repo).unwrap_or(Path::new(""));
    let import = relative_import("cppanoriginal", Path::new(""), relative);
    util::write(
        &directory.join("go.mod"),
        &format!(
            "module cppanrecord\n\ngo 1.21\n\nrequire cppanoriginal v0.0.0\n\nreplace cppanoriginal => {}\n",
            repo.display()
        ),
    )?;
    Ok(Harness {
        import,
        run: ".".to_string(),
        cwd: directory.clone(),
        directory,
        temporary: if created_module {
            vec![module_file]
        } else {
            Vec::new()
        },
    })
}

/// Writes the harness, runs it, and removes the temporary module either way.
fn execute(project: &Project, harness: Harness, body: &str) -> Result<String, String> {
    let write = util::write(&harness.directory.join("main.go"), body);
    let result = match write {
        Ok(()) => super::run_harness(
            project,
            "go",
            &["run".to_string(), harness.run.clone()],
            Some(&harness.cwd),
        ),
        Err(error) => Err(error),
    };
    let _ = fs::remove_dir_all(&harness.directory);
    for path in &harness.temporary {
        let _ = fs::remove_file(path);
    }
    result
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

pub fn record(
    project: &Project,
    unit: &crate::inventory::Unit,
    calls: &[Json],
    _options: &RecordOptions,
) -> Result<usize, String> {
    let calls_path = project.dir.join("build").join("record-calls.jsonl");
    util::write(&calls_path, &super::cases_json(calls))?;
    if unit.functions.is_empty() {
        return Err("the package has no functions with a supported signature".into());
    }
    let ns = crate::codegen::namespace_of(project);
    let harness = locate_harness(project, &unit.file, &ns)?;
    let body = render_record_harness(
        &harness.import,
        &unit
            .functions
            .iter()
            .map(|function| function.name.clone())
            .collect::<Vec<_>>(),
        calls,
    );
    let output = execute(project, harness, &body)?;
    super::write_cases(project, calls, &output)
}

fn render_record_harness(import: &str, functions: &[String], calls: &[Json]) -> String {
    let mut source = String::new();
    source.push_str("// Generated by cpp-anything record. Runs the original once and prints one JSON line per call.\n");
    source.push_str("package main\n\n");
    source.push_str("import (\n\t\"encoding/json\"\n\t\"fmt\"\n\t\"math\"\n\t\"os\"\n\t\"reflect\"\n\t\"strconv\"\n\t\"strings\"\n\n\tcppanoriginal ");
    source.push_str(&go_quote(import));
    source.push_str("\n)\n\n");
    source.push_str("var cppanCalls = []string{\n");
    for call in calls {
        source.push_str("\t");
        source.push_str(&go_quote(&call.to_string_compact()));
        source.push_str(",\n");
    }
    source.push_str("}\n\n");
    source.push_str("var cppanFunctions = map[string]interface{}{\n");
    for name in functions {
        source.push_str(&format!("\t{}: cppanoriginal.{},\n", go_quote(name), name));
    }
    source.push_str("}\n\n");
    source.push_str(GO_HARNESS_HELPERS);
    source.push_str(
        r#"

func main() {
	for _, line := range cppanCalls {
		var call struct {
			ID   string            `json:"id"`
			Fn   string            `json:"fn"`
			Args []json.RawMessage `json:"args"`
		}
		if err := cppanDecode(line, &call); err != nil {
			fmt.Fprintln(os.Stderr, "cppan record: bad call line:", err)
			continue
		}
		args := make([]interface{}, 0, len(call.Args))
		failed := false
		for _, raw := range call.Args {
			var value interface{}
			if err := cppanDecode(string(raw), &value); err != nil {
				cppanEmitError(call.ID, &cppanError{Type: "DecodeError", Msg: err.Error()})
				failed = true
				break
			}
			args = append(args, value)
		}
		if failed {
			continue
		}
		function, ok := cppanFunctions[call.Fn]
		if !ok {
			cppanEmitError(call.ID, &cppanError{Type: "NameError", Msg: "no function named " + call.Fn})
			continue
		}
		value, failure := cppanInvoke(function, args)
		if failure != nil {
			cppanEmitError(call.ID, failure)
		} else {
			cppanEmit(call.ID, value)
		}
	}
}
"#,
    );
    source
}

// ---------------------------------------------------------------------------
// benchmarking
// ---------------------------------------------------------------------------

pub fn bench(project: &Project, repeat: usize) -> Result<f64, String> {
    let cases_path = project.dir.join("cases.jsonl");
    let text = util::read(&cases_path)?;
    let mut first: std::collections::BTreeMap<String, Json> = std::collections::BTreeMap::new();
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let doc = Json::parse(line)?;
        let function = doc
            .get("fn")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_string();
        first.entry(function).or_insert(doc);
    }
    let mut calls = Vec::new();
    for doc in first.values() {
        let function = doc
            .get("fn")
            .and_then(Json::as_str)
            .ok_or("a case without fn")?
            .to_string();
        let args = doc.get("args").cloned().unwrap_or(Json::Array(Vec::new()));
        calls.push(Json::obj([("fn", Json::str(function)), ("args", args)]).to_string_compact());
    }
    if calls.is_empty() {
        return Err("no recorded cases to benchmark".into());
    }
    let ns = crate::codegen::namespace_of(project);
    let harness = locate_harness(project, &project.source_file, &ns)?;
    let mut source = String::new();
    source.push_str("// Generated by cpp-anything bench. Times the original on its first recorded case per function.\n");
    source.push_str("package main\n\n");
    source.push_str("import (\n\t\"encoding/json\"\n\t\"fmt\"\n\t\"math\"\n\t\"os\"\n\t\"reflect\"\n\t\"strconv\"\n\t\"strings\"\n\t\"time\"\n\n\tcppanoriginal ");
    source.push_str(&go_quote(&harness.import));
    source.push_str("\n)\n\n");
    source.push_str("var cppanFunctions = map[string]interface{}{\n");
    for function in &project.functions {
        source.push_str(&format!(
            "\t{}: cppanoriginal.{},\n",
            go_quote(&function.name),
            function.name
        ));
    }
    source.push_str("}\n\n");
    source.push_str("var cppanBench = []string{\n");
    for call in &calls {
        source.push_str("\t");
        source.push_str(&go_quote(call));
        source.push_str(",\n");
    }
    source.push_str("}\n\n");
    source.push_str(&format!("const cppanRepeat = {repeat}\n"));
    source.push_str(GO_HARNESS_HELPERS);
    source.push_str(
        r#"

func main() {
	for _, line := range cppanBench {
		cppanBenchCall(line)
	}
	started := time.Now()
	for round := 0; round < cppanRepeat; round++ {
		for _, line := range cppanBench {
			cppanBenchCall(line)
		}
	}
	elapsed := time.Since(started).Nanoseconds()
	fmt.Printf("{\"ns\":%d,\"calls\":%d}\n", elapsed, len(cppanBench)*cppanRepeat)
}

func cppanBenchCall(line string) {
	var call struct {
		Fn   string            `json:"fn"`
		Args []json.RawMessage `json:"args"`
	}
	if err := cppanDecode(line, &call); err != nil {
		return
	}
	args := make([]interface{}, 0, len(call.Args))
	for _, raw := range call.Args {
		var value interface{}
		if err := cppanDecode(string(raw), &value); err != nil {
			return
		}
		args = append(args, value)
	}
	function, ok := cppanFunctions[call.Fn]
	if !ok {
		return
	}
	_, _ = cppanInvoke(function, args)
}
"#,
    );
    let output = execute(project, harness, &source)?;
    let doc = Json::parse(output.trim())?;
    let total = doc.get("ns").and_then(Json::as_f64).unwrap_or(0.0);
    let count = doc.get("calls").and_then(Json::as_f64).unwrap_or(1.0);
    if count <= 0.0 {
        return Err("the Go benchmark ran no calls".into());
    }
    Ok(total / count)
}

// ---------------------------------------------------------------------------
// the harness helpers, shared by `record` and `bench`
// ---------------------------------------------------------------------------

const GO_HARNESS_HELPERS: &str = r#"
var cppanErrorInterface = reflect.TypeOf((*error)(nil)).Elem()

type cppanError struct {
	Type string `json:"type"`
	Msg  string `json:"msg"`
}

func cppanDecode(text string, target interface{}) error {
	decoder := json.NewDecoder(strings.NewReader(text))
	decoder.UseNumber()
	return decoder.Decode(target)
}

func cppanInvoke(function interface{}, args []interface{}) (result interface{}, failure *cppanError) {
	defer func() {
		if recovered := recover(); recovered != nil {
			result = nil
			failure = &cppanError{Type: "panic", Msg: cppanTrim(fmt.Sprintf("%T: %v", recovered, recovered))}
		}
	}()
	value := reflect.ValueOf(function)
	signature := value.Type()
	if signature.IsVariadic() {
		return nil, &cppanError{Type: "ArgumentError", Msg: "variadic functions have no wire form"}
	}
	if len(args) != signature.NumIn() {
		return nil, &cppanError{Type: "ArgumentError", Msg: fmt.Sprintf("expected %d arguments, got %d", signature.NumIn(), len(args))}
	}
	values := make([]reflect.Value, len(args))
	for index, arg := range args {
		converted, err := cppanConvert(arg, signature.In(index))
		if err != nil {
			return nil, &cppanError{Type: "ArgumentError", Msg: err.Error()}
		}
		values[index] = converted
	}
	out := value.Call(values)
	if len(out) > 0 {
		last := out[len(out)-1]
		if last.Type().Implements(cppanErrorInterface) {
			if cppanNilable(last.Kind()) && last.IsNil() {
				out = out[:len(out)-1]
			} else {
				original := last.Interface().(error)
				return nil, &cppanError{Type: cppanErrorName(original), Msg: cppanTrim(original.Error())}
			}
		}
	}
	switch len(out) {
	case 0:
		return nil, nil
	case 1:
		return out[0].Interface(), nil
	default:
		values := make([]interface{}, len(out))
		for index := range out {
			values[index] = out[index].Interface()
		}
		return values, nil
	}
}

func cppanNilable(kind reflect.Kind) bool {
	switch kind {
	case reflect.Chan, reflect.Func, reflect.Interface, reflect.Map, reflect.Ptr, reflect.Slice:
		return true
	default:
		return false
	}
}

func cppanConvert(value interface{}, target reflect.Type) (reflect.Value, error) {
	if value == nil {
		switch target.Kind() {
		case reflect.Interface, reflect.Ptr, reflect.Slice, reflect.Map:
			return reflect.Zero(target), nil
		default:
			return reflect.Value{}, fmt.Errorf("null is not a %s", target)
		}
	}
	switch target.Kind() {
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		number, err := cppanInt(value)
		if err != nil {
			return reflect.Value{}, err
		}
		if target.OverflowInt(number) {
			return reflect.Value{}, fmt.Errorf("%d overflows %s", number, target)
		}
		out := reflect.New(target).Elem()
		out.SetInt(number)
		return out, nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		number, err := cppanUint(value)
		if err != nil {
			return reflect.Value{}, err
		}
		if target.OverflowUint(number) {
			return reflect.Value{}, fmt.Errorf("%d overflows %s", number, target)
		}
		out := reflect.New(target).Elem()
		out.SetUint(number)
		return out, nil
	case reflect.Float32, reflect.Float64:
		number, err := cppanFloat(value)
		if err != nil {
			return reflect.Value{}, err
		}
		out := reflect.New(target).Elem()
		out.SetFloat(number)
		return out, nil
	case reflect.String:
		text, ok := value.(string)
		if !ok {
			return reflect.Value{}, fmt.Errorf("%v is not a string", value)
		}
		out := reflect.New(target).Elem()
		out.SetString(text)
		return out, nil
	case reflect.Bool:
		flag, ok := value.(bool)
		if !ok {
			return reflect.Value{}, fmt.Errorf("%v is not a bool", value)
		}
		out := reflect.New(target).Elem()
		out.SetBool(flag)
		return out, nil
	case reflect.Slice:
		items, ok := value.([]interface{})
		if !ok {
			return reflect.Value{}, fmt.Errorf("%v is not an array", value)
		}
		out := reflect.MakeSlice(target, len(items), len(items))
		for index, item := range items {
			converted, err := cppanConvert(item, target.Elem())
			if err != nil {
				return reflect.Value{}, err
			}
			out.Index(index).Set(converted)
		}
		return out, nil
	case reflect.Map:
		if target.Key().Kind() != reflect.String {
			return reflect.Value{}, fmt.Errorf("map keys must be strings, not %s", target.Key())
		}
		object, ok := value.(map[string]interface{})
		if !ok {
			return reflect.Value{}, fmt.Errorf("%v is not an object", value)
		}
		out := reflect.MakeMapWithSize(target, len(object))
		for key, item := range object {
			converted, err := cppanConvert(item, target.Elem())
			if err != nil {
				return reflect.Value{}, err
			}
			out.SetMapIndex(reflect.ValueOf(key).Convert(target.Key()), converted)
		}
		return out, nil
	case reflect.Interface:
		dynamic, err := cppanDynamic(value)
		if err != nil {
			return reflect.Value{}, err
		}
		if !dynamic.Type().AssignableTo(target) {
			return reflect.Value{}, fmt.Errorf("%s is not assignable to %s", dynamic.Type(), target)
		}
		return dynamic, nil
	default:
		return reflect.Value{}, fmt.Errorf("%s has no JSON form", target)
	}
}

func cppanDynamic(value interface{}) (reflect.Value, error) {
	if value == nil {
		return reflect.Zero(reflect.TypeOf((*interface{})(nil)).Elem()), nil
	}
	if number, ok := value.(json.Number); ok {
		if integer, err := number.Int64(); err == nil {
			return reflect.ValueOf(integer), nil
		}
		if real, err := number.Float64(); err == nil {
			return reflect.ValueOf(real), nil
		}
		return reflect.Value{}, fmt.Errorf("%s is not a number", number)
	}
	if items, ok := value.([]interface{}); ok {
		out := make([]interface{}, len(items))
		for index, item := range items {
			converted, err := cppanDynamic(item)
			if err != nil {
				return reflect.Value{}, err
			}
			out[index] = converted.Interface()
		}
		return reflect.ValueOf(out), nil
	}
	if object, ok := value.(map[string]interface{}); ok {
		out := make(map[string]interface{}, len(object))
		for key, item := range object {
			converted, err := cppanDynamic(item)
			if err != nil {
				return reflect.Value{}, err
			}
			out[key] = converted.Interface()
		}
		return reflect.ValueOf(out), nil
	}
	return reflect.ValueOf(value), nil
}

func cppanInt(value interface{}) (int64, error) {
	switch typed := value.(type) {
	case json.Number:
		if integer, err := typed.Int64(); err == nil {
			return integer, nil
		}
		real, err := typed.Float64()
		if err != nil {
			return 0, fmt.Errorf("%s is not an integer", typed)
		}
		return cppanFloatToInt(real)
	case float64:
		return cppanFloatToInt(typed)
	case int64:
		return typed, nil
	case int:
		return int64(typed), nil
	}
	return 0, fmt.Errorf("%v is not an integer", value)
}

func cppanFloatToInt(number float64) (int64, error) {
	if math.IsNaN(number) || math.IsInf(number, 0) || number != math.Trunc(number) || number < math.MinInt64 || number > math.MaxInt64 {
		return 0, fmt.Errorf("%v is not an int64", number)
	}
	return int64(number), nil
}

func cppanUint(value interface{}) (uint64, error) {
	switch typed := value.(type) {
	case json.Number:
		if integer, err := typed.Int64(); err == nil {
			if integer < 0 {
				return 0, fmt.Errorf("%s is negative", typed)
			}
			return uint64(integer), nil
		}
		if unsigned, err := strconv.ParseUint(string(typed), 10, 64); err == nil {
			return unsigned, nil
		}
		real, err := typed.Float64()
		if err != nil {
			return 0, fmt.Errorf("%s is not an integer", typed)
		}
		if real < 0 || math.IsNaN(real) || math.IsInf(real, 0) {
			return 0, fmt.Errorf("%s is not a uint64", typed)
		}
		return uint64(real), nil
	case float64:
		if typed < 0 || math.IsNaN(typed) || math.IsInf(typed, 0) {
			return 0, fmt.Errorf("%v is not a uint64", typed)
		}
		return uint64(typed), nil
	case int:
		if typed < 0 {
			return 0, fmt.Errorf("%d is negative", typed)
		}
		return uint64(typed), nil
	case int64:
		if typed < 0 {
			return 0, fmt.Errorf("%d is negative", typed)
		}
		return uint64(typed), nil
	}
	return 0, fmt.Errorf("%v is not an integer", value)
}

func cppanFloat(value interface{}) (float64, error) {
	switch typed := value.(type) {
	case json.Number:
		return typed.Float64()
	case float64:
		return typed, nil
	case int64:
		return float64(typed), nil
	case int:
		return float64(typed), nil
	}
	return 0, fmt.Errorf("%v is not a number", value)
}

func cppanEncode(value interface{}) (interface{}, *cppanError) {
	return cppanEncodeValue(reflect.ValueOf(value))
}

func cppanEncodeValue(value reflect.Value) (interface{}, *cppanError) {
	if !value.IsValid() {
		return nil, nil
	}
	for value.Kind() == reflect.Interface || value.Kind() == reflect.Ptr {
		if value.IsNil() {
			return nil, nil
		}
		value = value.Elem()
	}
	switch value.Kind() {
	case reflect.Bool:
		return value.Bool(), nil
	case reflect.String:
		return value.String(), nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return value.Int(), nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		number := value.Uint()
		if number > math.MaxInt64 {
			return nil, &cppanError{Type: "RangeError", Msg: fmt.Sprintf("%d does not fit int64", number)}
		}
		return int64(number), nil
	case reflect.Float32, reflect.Float64:
		number := value.Float()
		if math.IsNaN(number) || math.IsInf(number, 0) {
			return nil, &cppanError{Type: "RangeError", Msg: "non-finite float"}
		}
		return number, nil
	case reflect.Slice, reflect.Array:
		items := make([]interface{}, value.Len())
		for index := 0; index < value.Len(); index++ {
			encoded, failure := cppanEncodeValue(value.Index(index))
			if failure != nil {
				return nil, failure
			}
			items[index] = encoded
		}
		return items, nil
	case reflect.Map:
		if value.Type().Key().Kind() != reflect.String {
			return nil, &cppanError{Type: "TypeError", Msg: "map keys must be strings"}
		}
		object := make(map[string]interface{}, value.Len())
		iterator := value.MapRange()
		for iterator.Next() {
			encoded, failure := cppanEncodeValue(iterator.Value())
			if failure != nil {
				return nil, failure
			}
			object[iterator.Key().String()] = encoded
		}
		return object, nil
	default:
		return map[string]interface{}{"$opaque": value.Type().String()}, nil
	}
}

func cppanErrorName(err error) string {
	name := reflect.TypeOf(err).String()
	if position := strings.LastIndex(name, "."); position >= 0 {
		name = name[position+1:]
	}
	name = strings.TrimPrefix(name, "*")
	if name == "" {
		return "error"
	}
	return name
}

func cppanTrim(text string) string {
	if len(text) > 200 {
		return text[:200]
	}
	return text
}

func cppanEmit(id string, value interface{}) {
	encoded, failure := cppanEncode(value)
	if failure != nil {
		cppanEmitError(id, failure)
		return
	}
	cppanPrint(map[string]interface{}{"id": id, "ok": encoded})
}

func cppanEmitError(id string, failure *cppanError) {
	if id == "" {
		fmt.Fprintln(os.Stderr, "cppan record:", failure.Type, failure.Msg)
		return
	}
	cppanPrint(map[string]interface{}{"id": id, "err": failure})
}

func cppanPrint(value interface{}) {
	text, err := json.Marshal(value)
	if err != nil {
		fmt.Fprintln(os.Stderr, "cppan record:", err)
		return
	}
	fmt.Println(string(text))
}
"#;

// ---------------------------------------------------------------------------
// the shadow binding (cgo over the same C ABI)
// ---------------------------------------------------------------------------

pub fn shadow_files(project: &Project, ns: &str) -> Vec<(String, String)> {
    let import = import_path(&project.source_file).unwrap_or_else(|_| "cppanoriginal".into());
    let signatures = if project.source_file.is_dir() {
        read_signatures(&project.source_file)
    } else {
        std::collections::BTreeMap::new()
    };
    let log = crate::shadow::log_path(project);
    let mut wrappers = String::new();
    for function in &project.functions {
        if matches!(function.name.as_str(), "Call" | "Error") {
            wrappers.push_str(&format!(
                "// {}: the generated binding already uses that name; no wrapper was generated.\n",
                function.name
            ));
            continue;
        }
        let Some(signature) = signatures.get(&function.name) else {
            wrappers.push_str(&format!(
                "// {}: not found in the package source; no wrapper was generated.\n",
                function.name
            ));
            continue;
        };
        if signature.params.len() != function.params.len() || signature.results.len() > 1 {
            wrappers.push_str(&format!(
                "// {}: the source signature changed shape; run `cpp-anything refresh`.\n",
                function.name
            ));
            continue;
        }
        wrappers.push_str(&render_wrapper(function, signature));
    }
    let anchor = project
        .functions
        .first()
        .map(|function| format!("var _ = cppanoriginal.{}", function.name))
        .unwrap_or_default();
    let import_line = if anchor.is_empty() {
        format!("_ {}", go_quote(&import))
    } else {
        format!("cppanoriginal {}", go_quote(&import))
    };
    let text = SHADOW_TEMPLATE
        .replace("@@NS@@", ns)
        .replace("@@IMPORT_LINE@@", &import_line)
        .replace("@@LOG@@", &log.display().to_string())
        .replace("@@WRAPPERS@@", &wrappers)
        .replace("@@ANCHOR_LINE@@", &anchor);
    let build = format!(
        "#!/bin/sh\n# cpp-anything shadow: cgo links the port at `go build` time.\nset -eu\nhere=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\nlib=\"$here/../build/libcppan_shadow.so\"\nif [ ! -f \"$lib\" ]; then\n  echo \"shadow: $lib is missing; run: cpp-anything build <project>\" >&2\n  exit 1\nfi\necho \"shadow: ready ($lib)\"\n"
    );
    vec![
        (format!("shadow/{ns}_shadow.go"), text),
        ("shadow/build.sh".to_string(), build),
    ]
}

fn read_signatures(package_dir: &Path) -> std::collections::BTreeMap<String, GoFunction> {
    let mut signatures = std::collections::BTreeMap::new();
    let Ok(files) = go_files(package_dir) else {
        return signatures;
    };
    for file in files {
        let Ok(source) = util::read(&file) else {
            continue;
        };
        for function in scan_functions(&source) {
            if function.receiver || !is_exported(&function.name) {
                continue;
            }
            signatures.insert(function.name.clone(), function);
        }
    }
    signatures
}

fn render_wrapper(function: &crate::inventory::Function, signature: &GoFunction) -> String {
    let mut args = String::new();
    for (index, param) in signature.params.iter().enumerate() {
        if index > 0 {
            args.push_str(", ");
        }
        let name = param
            .name
            .clone()
            .filter(|name| !name.is_empty() && name != "_")
            .unwrap_or_else(|| format!("arg{index}"));
        args.push_str(&format!("{} {}", name, param.ty));
    }
    let names = signature
        .params
        .iter()
        .enumerate()
        .map(|(index, param)| {
            param
                .name
                .clone()
                .filter(|name| !name.is_empty() && name != "_")
                .unwrap_or_else(|| format!("arg{index}"))
        })
        .collect::<Vec<_>>();
    let values = names
        .iter()
        .map(|name| name.clone())
        .collect::<Vec<_>>()
        .join(", ");
    let call = format!("cppanoriginal.{}({values})", function.name);
    let mut out = String::new();
    out.push_str(&format!(
        "\n// {} runs the original package's function first; the port observes it.\n",
        function.name
    ));
    if signature.results.is_empty() {
        out.push_str(&format!("func {}({args}) {{\n", function.name));
        out.push_str(&format!(
            "\t_cppanArgs := []interface{{}}{{{values}}}\n\tdefer cppanPanic({}, _cppanArgs)\n\t{}\n\tcppanObserve({}, _cppanArgs, nil)\n}}\n",
            go_quote(&function.name),
            call,
            go_quote(&function.name)
        ));
    } else {
        let result = &signature.results[0].ty;
        out.push_str(&format!("func {}({args}) {result} {{\n", function.name));
        out.push_str(&format!(
            "\t_cppanArgs := []interface{{}}{{{values}}}\n\tdefer cppanPanic({}, _cppanArgs)\n\t_cppanResult := {call}\n\tcppanObserve({}, _cppanArgs, _cppanResult)\n\treturn _cppanResult\n}}\n",
            go_quote(&function.name),
            go_quote(&function.name)
        ));
    }
    out
}

const SHADOW_TEMPLATE: &str = r#"// Generated by cpp-anything. DO NOT EDIT.
//
// The original Go function runs first; the C++ port runs second through the C
// ABI in build/libcppan_shadow.so. Callers see exactly the original's value or
// panic; disagreements are appended to shadow/@@NS@@.shadow.jsonl.
package cppanshadow_@@NS@@

/*
#cgo LDFLAGS: -L${SRCDIR}/../build -lcppan_shadow -Wl,-rpath,${SRCDIR}/../build
#include <stdlib.h>

const char* cppan_shadow_call(const char* fn, const char* args_json);
void cppan_shadow_free(const char* ptr);
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"time"
	"unsafe"

	@@IMPORT_LINE@@
)

const cppanLogPath = "@@LOG@@"

// Error is what Call returns when the port fails.
type Error struct {
	Type string `json:"type"`
	Msg  string `json:"msg"`
}

func (e *Error) Error() string {
	if e == nil {
		return "shadow error"
	}
	return e.Type + ": " + e.Msg
}

// Call runs the C++ port through the C ABI. It returns the port's value, or an
// *Error carrying the port's error type (messages are not compared).
func Call(fn string, args ...interface{}) (interface{}, error) {
	payload, err := json.Marshal(args)
	if err != nil {
		return nil, &Error{Type: "EncodingError", Msg: err.Error()}
	}
	cName := C.CString(fn)
	defer C.free(unsafe.Pointer(cName))
	cArgs := C.CString(string(payload))
	defer C.free(unsafe.Pointer(cArgs))
	pointer := C.cppan_shadow_call(cName, cArgs)
	if pointer == nil {
		return nil, &Error{Type: "ShadowError", Msg: "the port returned nothing"}
	}
	text := C.GoString(pointer)
	C.cppan_shadow_free(pointer)
	var envelope struct {
		Ok  json.RawMessage `json:"ok"`
		Err *Error          `json:"err"`
	}
	if err := json.Unmarshal([]byte(text), &envelope); err != nil {
		return nil, &Error{Type: "ShadowError", Msg: "the port returned invalid JSON: " + err.Error()}
	}
	if envelope.Err != nil {
		return nil, envelope.Err
	}
	if envelope.Ok == nil {
		return nil, nil
	}
	var value interface{}
	decoder := json.NewDecoder(strings.NewReader(string(envelope.Ok)))
	decoder.UseNumber()
	if err := decoder.Decode(&value); err != nil {
		return nil, &Error{Type: "ShadowError", Msg: err.Error()}
	}
	return value, nil
}

func cppanPanic(fn string, args []interface{}) {
	if recovered := recover(); recovered != nil {
		cppanObservePanic(fn, args, recovered)
		panic(recovered)
	}
}

func cppanObserve(fn string, args []interface{}, value interface{}) {
	if os.Getenv("CPPAN_SHADOW") == "off" {
		return
	}
	expected := cppanExpected(value, nil, false)
	got := cppanCall(fn, args)
	if reason := cppanCompare(expected, got); reason != "" {
		cppanLog(fn, args, expected, got, reason)
	}
}

func cppanObservePanic(fn string, args []interface{}, recovered interface{}) {
	if os.Getenv("CPPAN_SHADOW") == "off" {
		return
	}
	expected := cppanExpected(nil, recovered, true)
	got := cppanCall(fn, args)
	if reason := cppanCompare(expected, got); reason != "" {
		cppanLog(fn, args, expected, got, reason)
	}
}

func cppanExpected(value interface{}, panicValue interface{}, panicked bool) map[string]interface{} {
	if panicked {
		return map[string]interface{}{
			"err": map[string]interface{}{
				"type": "panic",
				"msg":  cppanTrim(fmt.Sprintf("%T: %v", panicValue, panicValue)),
			},
		}
	}
	encoded, failure := cppanEncode(value)
	if failure != nil {
		return map[string]interface{}{"err": map[string]interface{}{"type": failure.Type, "msg": failure.Msg}}
	}
	text, err := json.Marshal(encoded)
	if err != nil {
		return map[string]interface{}{"err": map[string]interface{}{"type": "EncodingError", "msg": err.Error()}}
	}
	var normalized interface{}
	decoder := json.NewDecoder(strings.NewReader(string(text)))
	decoder.UseNumber()
	if err := decoder.Decode(&normalized); err != nil {
		return map[string]interface{}{"err": map[string]interface{}{"type": "EncodingError", "msg": err.Error()}}
	}
	return map[string]interface{}{"ok": normalized}
}

func cppanCall(fn string, args []interface{}) map[string]interface{} {
	value, err := Call(fn, args...)
	if err == nil {
		return map[string]interface{}{"ok": value}
	}
	if failure, ok := err.(*Error); ok {
		return map[string]interface{}{"err": map[string]interface{}{"type": failure.Type, "msg": failure.Msg}}
	}
	return map[string]interface{}{"err": map[string]interface{}{"type": "ShadowError", "msg": err.Error()}}
}

func cppanCompare(expected, got map[string]interface{}) string {
	expectedError, expectedFailed := expected["err"]
	gotError, gotFailed := got["err"]
	if expectedFailed || gotFailed {
		if !expectedFailed {
			return "the original returns a value, the port raises " + cppanErrorType(gotError)
		}
		if !gotFailed {
			return "the original raises, the port returns a value"
		}
		left := cppanErrorType(expectedError)
		right := cppanErrorType(gotError)
		if left != right {
			return "error type: expected " + left + ", got " + right
		}
		return ""
	}
	if !reflect.DeepEqual(expected["ok"], got["ok"]) {
		return fmt.Sprintf("values differ: expected %v, got %v", expected["ok"], got["ok"])
	}
	return ""
}

func cppanErrorType(value interface{}) string {
	object, ok := value.(map[string]interface{})
	if !ok {
		return "?"
	}
	text, _ := object["type"].(string)
	if text == "" {
		return "?"
	}
	return text
}

func cppanLog(fn string, args []interface{}, expected, got map[string]interface{}, reason string) {
	encoded := make([]interface{}, 0, len(args))
	for _, arg := range args {
		value, failure := cppanEncode(arg)
		if failure != nil {
			value = map[string]interface{}{"$opaque": fmt.Sprintf("%T", arg)}
		}
		encoded = append(encoded, value)
	}
	path := os.Getenv("CPPAN_SHADOW_LOG")
	if path == "" {
		path = cppanLogPath
	}
	if directory := filepath.Dir(path); directory != "" {
		_ = os.MkdirAll(directory, 0o755)
	}
	entry := map[string]interface{}{
		"at":       time.Now().UTC().Format("2006-01-02T15:04:05Z"),
		"fn":       fn,
		"args":     encoded,
		"expected": expected,
		"got":      got,
		"reason":   reason,
	}
	text, err := json.Marshal(entry)
	if err != nil {
		return
	}
	file, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	defer file.Close()
	_, _ = file.Write(append(text, '\n'))
}

func cppanEncode(value interface{}) (interface{}, *Error) {
	return cppanEncodeValue(reflect.ValueOf(value))
}

func cppanEncodeValue(value reflect.Value) (interface{}, *Error) {
	if !value.IsValid() {
		return nil, nil
	}
	for value.Kind() == reflect.Interface || value.Kind() == reflect.Ptr {
		if value.IsNil() {
			return nil, nil
		}
		value = value.Elem()
	}
	switch value.Kind() {
	case reflect.Bool:
		return value.Bool(), nil
	case reflect.String:
		return value.String(), nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return value.Int(), nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		number := value.Uint()
		if number > math.MaxInt64 {
			return nil, &Error{Type: "RangeError", Msg: fmt.Sprintf("%d does not fit int64", number)}
		}
		return int64(number), nil
	case reflect.Float32, reflect.Float64:
		number := value.Float()
		if math.IsNaN(number) || math.IsInf(number, 0) {
			return nil, &Error{Type: "RangeError", Msg: "non-finite float"}
		}
		return number, nil
	case reflect.Slice, reflect.Array:
		items := make([]interface{}, value.Len())
		for index := 0; index < value.Len(); index++ {
			encoded, failure := cppanEncodeValue(value.Index(index))
			if failure != nil {
				return nil, failure
			}
			items[index] = encoded
		}
		return items, nil
	case reflect.Map:
		if value.Type().Key().Kind() != reflect.String {
			return nil, &Error{Type: "TypeError", Msg: "map keys must be strings"}
		}
		object := make(map[string]interface{}, value.Len())
		iterator := value.MapRange()
		for iterator.Next() {
			encoded, failure := cppanEncodeValue(iterator.Value())
			if failure != nil {
				return nil, failure
			}
			object[iterator.Key().String()] = encoded
		}
		return object, nil
	default:
		return map[string]interface{}{"$opaque": value.Type().String()}, nil
	}
}

func cppanTrim(text string) string {
	if len(text) > 200 {
		return text[:200]
	}
	return text
}

@@WRAPPERS@@

@@ANCHOR_LINE@@
"#;

// ---------------------------------------------------------------------------
// text helpers
// ---------------------------------------------------------------------------

fn go_quote(text: &str) -> String {
    let mut out = String::from("\"");
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            other if (other as u32) < 0x20 || other as u32 == 0x7f => {
                out.push_str(&format!("\\u{:04x}", other as u32));
            }
            other => out.push(other),
        }
    }
    out.push('"');
    out
}
