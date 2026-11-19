#!/usr/bin/env python3
"""cpp-anything probe: the Python side of the loop.

Modes:
    analyze   --root REPO
    inventory --path FILE --module NAME --package-root DIR
    record    --path FILE --module NAME --package-root DIR --out cases.jsonl
              [--cases N] [--seed N] [--inputs extra.jsonl]
    bench     --path FILE --module NAME --package-root DIR --cases cases.jsonl
              --repeat N

Everything is emitted as JSON on stdout (or straight to a file for `record`), so
the Rust side never has to parse Python source. Stdlib only: the probe must run in
the user's own interpreter, whatever their project already depends on.
"""

from __future__ import annotations

import argparse
import ast
import importlib
import importlib.util
import json
import math
import random
import sys
import time
from pathlib import Path

SKIP_DIRS = {
    ".git", ".hg", ".svn", "node_modules", "dist", "build", "out", "target",
    "coverage", "__pycache__", ".venv", "venv", "env", ".tox", ".mypy_cache",
    ".pytest_cache", "site-packages", "fixtures", "__fixtures__", "__snapshots__",
    "examples", "docs", "migrations", "vendor",
}
SKIP_FILES = {"setup.py", "conftest.py", "noxfile.py"}

# ---------------------------------------------------------------------------
# wire encoding: values -> JSON that the C++ codec reads back exactly
# ---------------------------------------------------------------------------


def encode(value):
    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value if -(2**63) <= value < 2**63 else {"$int": str(value)}
    if isinstance(value, float):
        return value
    if isinstance(value, str):
        return value
    if isinstance(value, (list, tuple)):
        items = [encode(item) for item in value]
        return items if isinstance(value, list) else {"$tuple": items}
    if isinstance(value, (set, frozenset)):
        items = [encode(item) for item in value]
        return {"$set": sorted(items, key=lambda item: json.dumps(item, sort_keys=True))}
    if isinstance(value, dict):
        if all(isinstance(key, str) for key in value):
            return {key: encode(item) for key, item in value.items()}
        return {"$map": [[encode(key), encode(item)] for key, item in value.items()]}
    if isinstance(value, (bytes, bytearray)):
        import base64
        return {"$bytes": base64.b64encode(bytes(value)).decode("ascii")}
    return {"$opaque": repr(value)}


def decode(value):
    """Inverse of encode, for replaying recorded arguments in Python."""
    if isinstance(value, dict):
        if "$int" in value:
            return int(value["$int"])
        if "$tuple" in value:
            return tuple(decode(item) for item in value["$tuple"])
        if "$set" in value:
            return {decode(item) for item in value["$set"]}
        if "$map" in value:
            return {decode(k): decode(v) for k, v in value["$map"]}
        if "$bytes" in value:
            import base64
            return base64.b64decode(value["$bytes"])
        if "$opaque" in value:
            return value["$opaque"]
        return {key: decode(item) for key, item in value.items()}
    if isinstance(value, list):
        return [decode(item) for item in value]
    return value


# ---------------------------------------------------------------------------
# corpora: deterministic, seeded, and biased towards edges
# ---------------------------------------------------------------------------

STRINGS = [
    "", " ", "Hello World", "  Mixed CASE  ", "café — naïve", "a" * 300,
    "one\ntwo\tthree", "123", "Ünïcødé ☃", "trailing---", " leading",
    "line1\nline2", "Москва", "e\u0301", "0",
]
# Deliberately inside int64: a value outside it cannot cross the wire as an
# integer, so the analyzer reports that as a risk instead of recording a case the
# generated C++ signature can never satisfy.
INTS = [0, 1, -1, 7, 42, 12, 100, -100, 10**6, 3, 19, 255, -7, 2**31, 0, 11]
FLOATS = [
    0.0, -0.0, 1.5, -2.25, 1e308, 5e-324, float("nan"), float("inf"),
    0.1, 1 / 3, 2.0, -0.5, 1e-9, 123456.789, -1.0, 7.0,
]
BOOLS = [True, False]
LISTS = [
    [], ["a"], ["a", "b", "c"], ["Hello World", "x"], ["", " "], ["café"],
    ["a", "b"], ["one two"], ["trailing---", "ok"], ["x"] * 3, ["A", "B"],
    ["a", "b", "c", "d", "e"], ["zen"], ["", ""], ["микс"], ["0", "1"],
]
DICTS = [
    {}, {"a": 1}, {"slug": "x"}, {"a": "1", "b": "2"}, {"n": -1},
    {"": ""}, {"café": "naïve"}, {"k": 0}, {"x": 1, "y": 2},
]
ANY = [None, "x", 0, 1.5, [1], {"a": 1}, True, "text"]


def corpus_for(annotation: str, element_hint: str = "str"):
    text = (annotation or "").replace(" ", "")
    if text in ("str", "Text"):
        return STRINGS
    if text in ("int", "Integral"):
        return INTS
    if text in ("float", "Real"):
        return FLOATS
    if text in ("bool",):
        return BOOLS
    if text in ("None", "NoneType"):
        return [None]
    if text.startswith(("list[", "List[", "Sequence[", "Iterable[", "set[")):
        inner = text[text.index("[") + 1:-1]
        if inner in ("str", "Text"):
            return LISTS
        if inner in ("int",):
            return [[], [0], [1, 2, 3], [-1, 0, 1], [2**63 - 1]]
        return LISTS
    if text.startswith(("dict[str,str]", "Dict[str,str]", "dict[str,int]")):
        return DICTS
    if text.startswith(("Optional[", "Union[")) and "None" in text:
        inner = text[text.index("[") + 1:text.index("]")]
        inner = inner.replace("None", "").replace(",", "").split("|")[0].strip()
        return [None] + [item for item in corpus_for(inner) if item is not None][:7]
    return ANY


def corpus_value(items, index):
    return items[index % len(items)]


# ---------------------------------------------------------------------------
# AST helpers
# ---------------------------------------------------------------------------


def annotation_text(node) -> str:
    if node is None:
        return ""
    try:
        return ast.unparse(node)
    except Exception:  # noqa: BLE001 - unparse is best effort
        return ""


def literal(node):
    try:
        return True, ast.literal_eval(node)
    except Exception:  # noqa: BLE001 - not a literal
        return False, None


RISK_PATTERNS = [
    ("len(", "len() on str counts code points; std::string::size() counts bytes"),
    ("[:", "slice offsets: Python indexes code points, C++ indexes bytes"),
    ("int(", "int() accepts underscores, unicode digits and surrounding space; std::from_chars does not"),
    ("float(", "float() accepts 'inf'/'nan'/underscores; std::from_chars is stricter"),
    (".split()", "str.split() with no argument splits on unicode whitespace"),
    (".strip()", "str.strip() strips unicode whitespace, not just ASCII"),
    ("re.", "Python's re engine differs from std::regex (lookaround, \\w, backtracking)"),
    ("datetime", "datetime/time semantics (time zones, DST) rarely match across languages"),
    ("json.", "json round-trips NaNs, big ints and key order differently"),
    ("sorted(", "sorted() is stable and Python-ordered; std::sort is not stable"),
    (".sort(", "list.sort() is stable; std::sort is not"),
    (".format(", "format() behaviour depends on Python's float formatting"),
    ("f\"", "f-strings format floats with Python's shortest repr"),
    (".lower()", "str.lower() is unicode-aware; std::tolower is ASCII-only"),
    (".upper()", "str.upper() is unicode-aware"),
    ("isinstance(", "isinstance() dispatch has no direct C++ equivalent"),
    ("getattr(", "getattr() is dynamic dispatch"),
    ("**", "integer ** can produce numbers wider than 64 bits"),
    ("int(", "Python ints are unbounded: values beyond 2**63-1 do not fit std::int64_t"),
    ("sum(", "Python ints are unbounded: sums can exceed std::int64_t"),
    ("/ ", "Python's / is always float division"),
    ("//", "Python's // floors towards negative infinity; C++ / truncates"),
    ("round(", "Python's round() is banker's rounding, half to even"),
]


def function_risks(source: str, node) -> list[str]:
    segment = ast.get_source_segment(source, node) or ""
    risks = []
    for needle, explanation in RISK_PATTERNS:
        if needle in segment and explanation not in risks:
            risks.append(explanation)
    for argument in list(node.args.args) + list(node.args.kwonlyargs):
        if argument.annotation is None:
            risks.append("unannotated parameter: the port takes " + argument.arg + " as cppan::Json")
    if node.returns is None:
        risks.append("no return annotation: the port returns cppan::Json until typed")
    if isinstance(node, ast.AsyncFunctionDef):
        risks.append("async function: the port would need a scheduler, not a plain call")
    return risks


def describe_function(source: str, node) -> dict | None:
    if isinstance(node, ast.AsyncFunctionDef):
        return None
    if not isinstance(node, (ast.FunctionDef,)):
        return None
    if node.decorator_list:
        return None
    if node.args.vararg or node.args.kwarg:
        return None
    positional = list(node.args.posonlyargs) + list(node.args.args)
    defaults = [None] * (len(positional) - len(node.args.defaults)) + list(node.args.defaults)
    params = []
    for argument, default in zip(positional, defaults):
        ok, value = literal(default) if default is not None else (False, None)
        params.append({
            "name": argument.arg,
            "annotation": annotation_text(argument.annotation),
            "default": encode(value) if ok else None,
            "has_default": default is not None,
            "literal_default": ok,
        })
    for argument, default in zip(node.args.kwonlyargs, node.args.kw_defaults):
        ok, value = literal(default) if default is not None else (False, None)
        params.append({
            "name": argument.arg,
            "annotation": annotation_text(argument.annotation),
            "default": encode(value) if ok else None,
            "has_default": default is not None,
            "literal_default": ok,
            "keyword_only": True,
        })
    raises = any(isinstance(child, ast.Raise) for child in ast.walk(node))
    return {
        "name": node.name,
        "line": node.lineno,
        "params": params,
        "raises": raises,
        "returns": annotation_text(node.returns),
        "doc": (ast.get_docstring(node) or "").strip().splitlines()[0:1],
        "risks": function_risks(source, node),
    }


def module_functions(path: Path) -> tuple[list[dict], str]:
    source = path.read_text(encoding="utf-8", errors="replace")
    tree = ast.parse(source, filename=str(path))
    functions = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and not node.name.startswith("_"):
            described = describe_function(source, node)
            if described is not None:
                functions.append(described)
    return functions, source


def module_imports(path: Path) -> list[str]:
    try:
        tree = ast.parse(path.read_text(encoding="utf-8", errors="replace"))
    except SyntaxError:
        return []
    names = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.extend(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            names.append(node.module.split(".")[0])
    return sorted(set(names))


def module_risks(source: str) -> list[str]:
    risks = []
    for needle, explanation in RISK_PATTERNS:
        if needle in source and explanation not in risks:
            risks.append(explanation)
    if "class " in source:
        risks.append("classes stay in Python until the unit is ported to structs by hand")
    if "import random" in source or "random." in source:
        risks.append("randomness must be seeded on both sides or recorded away")
    if "open(" in source:
        risks.append("file I/O needs paths the C++ port can reach")
    return risks


# ---------------------------------------------------------------------------
# module loading
# ---------------------------------------------------------------------------


def load_module(path: Path, module: str, package_root: Path | None):
    if package_root is not None:
        sys.path.insert(0, str(package_root))
        try:
            return importlib.import_module(module)
        finally:
            sys.path.pop(0)
    spec = importlib.util.spec_from_file_location(module.replace(".", "_"), path)
    loaded = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = loaded
    spec.loader.exec_module(loaded)
    return loaded


def guarded(module, name: str):
    value = getattr(module, name, None)
    if value is None or not callable(value):
        raise RuntimeError(f"{name} is not callable in the loaded module")
    return value


# ---------------------------------------------------------------------------
# modes
# ---------------------------------------------------------------------------


def mode_analyze(args) -> int:
    root = Path(args.root).resolve()
    modules = []
    for path in sorted(root.rglob("*.py")):
        try:
            relative_parts = path.relative_to(root).parts
        except ValueError:
            continue
        # Only the repository's own layout matters: an absolute path can contain
        # `build/` or `fixtures/` for reasons that say nothing about the code.
        if any(part in SKIP_DIRS for part in relative_parts[:-1]):
            continue
        if path.name in SKIP_FILES or not path.is_file():
            continue
        try:
            functions, source = module_functions(path)
        except SyntaxError:
            continue
        if not functions:
            continue
        line_count = source.count("\n") + 1
        modules.append({
            "path": str(path.relative_to(root)),
            "module": ".".join(path.relative_to(root).with_suffix("").parts),
            "lines": line_count,
            "functions": [
                {
                    "name": function["name"],
                    "params": len(function["params"]),
                    "typed": sum(1 for param in function["params"] if param["annotation"]),
                    "returns": function["returns"],
                }
                for function in functions
            ],
            "risks": module_risks(source),
            "imports": module_imports(path),
        })
    print(json.dumps({"root": str(root), "modules": modules}, ensure_ascii=False))
    return 0


def mode_inventory(args) -> int:
    path = Path(args.path).resolve()
    functions, source = module_functions(path)
    skipped = []
    tree = ast.parse(source, filename=str(path))
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and not node.name.startswith("_"):
            if describe_function(source, node) is None:
                reason = "decorated" if node.decorator_list else (
                    "async" if isinstance(node, ast.AsyncFunctionDef) else "varargs")
                skipped.append({"name": node.name, "reason": reason})
    unit = args.unit or path.stem
    print(json.dumps({
        "unit": unit,
        "module": args.module,
        "file": str(path),
        "package_root": str(Path(args.package_root).resolve()) if args.package_root else None,
        "lines": source.count("\n") + 1,
        "doc": (ast.get_docstring(tree) or "").strip().splitlines()[0:1],
        "functions": functions,
        "skipped": skipped,
        "risks": module_risks(source),
        "imports": module_imports(path),
    }, ensure_ascii=False))
    return 0


def build_cases(functions: list[dict], count: int, seed: int) -> list[dict]:
    rng = random.Random(seed)
    cases = []
    for function in functions:
        if function.get("keyword_only"):
            continue
        params = [param for param in function["params"] if not param.get("keyword_only")]
        corpora = [corpus_for(param["annotation"]) for param in params]
        if not corpora:
            total = 1
        else:
            total = min(count, max(len(items) for items in corpora))
        for index in range(total):
            args = [encode(corpus_value(items, index)) for items in corpora]
            cases.append({
                "fn": function["name"],
                "args": args,
                "params": [param["name"] for param in params],
            })
        # A few cases carry arguments the edge corpora never produce: a random
        # mix, so the port is not tuned to the table above.
        if corpora and count > 0:
            for _ in range(max(1, count // 8)):
                args = []
                for annotation, items in zip((p["annotation"] for p in params), corpora):
                    if annotation.startswith(("int", "float")) and rng.random() < 0.5:
                        args.append(encode(rng.randint(-10_000, 10_000)))
                    else:
                        args.append(encode(corpus_value(items, rng.randrange(len(items)))))
                cases.append({
                    "fn": function["name"],
                    "args": args,
                    "params": [param["name"] for param in params],
                })
    return cases


def mode_record(args) -> int:
    path = Path(args.path).resolve()
    package_root = Path(args.package_root).resolve() if args.package_root else None
    functions, _ = module_functions(path)
    module = load_module(path, args.module, package_root)

    cases = build_cases(functions, args.cases, args.seed)
    if args.inputs:
        for line in Path(args.inputs).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line:
                extra = json.loads(line)
                cases.append({"fn": extra["fn"], "args": extra["args"], "params": []})

    out = []
    seen: dict[str, int] = {}
    for case in cases:
        name = case["fn"]
        index = seen.get(name, 0)
        seen[name] = index + 1
        entry = {
            "id": f"{name}:{index}",
            "fn": name,
            "args": case["args"],
        }
        try:
            function = guarded(module, name)
            result = function(*[decode(item) for item in case["args"]])
            entry["expect"] = {"ok": encode(result)}
        except Exception as error:  # noqa: BLE001 - a failure is data
            entry["expect"] = {"err": {"type": type(error).__name__, "msg": str(error)}}
        out.append(entry)

    with open(args.out, "w", encoding="utf-8") as handle:
        for entry in out:
            handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
    print(json.dumps({"cases": len(out), "out": args.out, "module": args.module}))
    return 0


def mode_bench(args) -> int:
    path = Path(args.path).resolve()
    package_root = Path(args.package_root).resolve() if args.package_root else None
    module = load_module(path, args.module, package_root)

    cases = []
    with open(args.cases, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                cases.append(json.loads(line))
    if not cases:
        print(json.dumps({"error": "no cases"}))
        return 1

    prepared = []
    for case in cases:
        prepared.append((guarded(module, case["fn"]), [decode(item) for item in case["args"]]))

    sink = 0
    repeat = max(1, args.repeat)
    start = time.perf_counter()
    for _ in range(repeat):
        for function, cargs in prepared:
            try:
                sink += len(repr(function(*cargs)))
            except Exception as error:  # noqa: BLE001 - the original raises, count it
                sink += len(str(error))
    elapsed = time.perf_counter() - start
    calls = repeat * len(prepared)
    print(json.dumps({
        "calls": calls,
        "ns_per_call": elapsed / calls * 1e9,
        "sink": sink,
    }))
    return 0


def mode_profile(args) -> int:
    """Run a workload under cProfile and emit what it cost, as JSON."""
    import pstats
    import subprocess
    import tempfile

    command = list(args.command)
    if command and command[0] in ("python", "python3"):
        command = command[1:]
    if not command:
        print(json.dumps({"error": "no workload: pass -- python3 bench.py"}))
        return 2
    script = command[0]
    if not Path(script).exists():
        print(json.dumps({"error": f"{script}: no such file"}))
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        profile_path = str(Path(tmp) / "prof.out")
        run = subprocess.run(
            [sys.executable, "-m", "cProfile", "-o", profile_path, *command],
            capture_output=True,
            text=True,
        )
        if run.returncode != 0:
            print(json.dumps({
                "error": "the workload failed",
                "exit": run.returncode,
                "stderr": run.stderr[-4000:],
            }))
            return 1
        stats = pstats.Stats(profile_path)
        stats.calc_callees()

    def name_of(key):
        file, line, function = key
        return {"file": file, "line": line, "function": function}

    entries = []
    for key, (cc, nc, tt, ct, callers) in stats.stats.items():
        if key[0] == "~":
            continue
        if key[2] in ("<module>",) and key[0].endswith("probe.py"):
            continue
        entry = name_of(key)
        entry.update({
            "calls": nc,
            "primitive_calls": cc,
            "self_seconds": tt,
            "cum_seconds": ct,
            "per_call_seconds": (tt / nc) if nc else 0.0,
            "callers": [name_of(caller) for caller in callers.keys()],
        })
        entries.append(entry)
    entries.sort(key=lambda item: item["self_seconds"], reverse=True)
    print(json.dumps({"workload": args.command, "functions": entries}, ensure_ascii=False))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)

    analyze = sub.add_parser("analyze")
    analyze.add_argument("--root", required=True)
    analyze.set_defaults(handler=mode_analyze)

    inventory = sub.add_parser("inventory")
    inventory.add_argument("--path", required=True)
    inventory.add_argument("--module", required=True)
    inventory.add_argument("--package-root", default=None)
    inventory.add_argument("--unit", default=None)
    inventory.set_defaults(handler=mode_inventory)

    record = sub.add_parser("record")
    record.add_argument("--path", required=True)
    record.add_argument("--module", required=True)
    record.add_argument("--package-root", default=None)
    record.add_argument("--out", required=True)
    record.add_argument("--cases", type=int, default=16)
    record.add_argument("--seed", type=int, default=20260819)
    record.add_argument("--inputs", default=None)
    record.set_defaults(handler=mode_record)

    bench = sub.add_parser("bench")
    bench.add_argument("--path", required=True)
    bench.add_argument("--module", required=True)
    bench.add_argument("--package-root", default=None)
    bench.add_argument("--cases", required=True)
    bench.add_argument("--repeat", type=int, default=1000)
    bench.set_defaults(handler=mode_bench)

    profile = sub.add_parser("profile")
    profile.add_argument("--json", action="store_true", help=argparse.SUPPRESS)
    profile.add_argument("command", nargs=argparse.REMAINDER)
    profile.set_defaults(handler=mode_profile)

    args = parser.parse_args()
    if getattr(args, "command", None) and args.command and args.command[0] == "--":
        args.command = args.command[1:]
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
