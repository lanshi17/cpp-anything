#!/usr/bin/env python3
"""Sync data/matrix.json from cppreference's compiler support tables.

The matrix is the factual backbone of cpp-anything: every C++ feature it knows
about, with the first GCC / Clang / MSVC (+ Apple Clang, EDG, ICC, NVCC, ...)
release that implements it, the feature-test macro, and the paper it comes from.

Why the wikitext and not the rendered page: cppreference keeps each table in a
template (`Template:cpp/compiler_support/23`). The wikitext is the table, one
`{{compiler_support_row}}` per feature, so an extractor stays exact while the
HTML changes. Every run records the page revision and a SHA-256 of what it read,
so a stale or fabricated entry can be traced and reproduced.

Usage:
    scripts/sync_matrix.py                 # fetch, parse, write data/matrix.json
    scripts/sync_matrix.py --offline       # parse the cached copies only
    scripts/sync_matrix.py --check         # exit 1 if data/matrix.json is stale

Stdlib only. No third-party imports.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
CACHE = ROOT / "scripts" / ".cache"
WIKI = "https://en.cppreference.com/w/index.php"
UA = "cpp-anything-matrix-sync/1 (+https://github.com/cpp-anything/cpp-anything)"

# Standards we publish guides for. 11 and 14 are legacy: kept so the matrix can
# answer "what does my old toolchain support", but the guides lead with 20 and 23.
STANDARDS = ["11", "14", "17", "20", "23", "26"]

# Toolchains we track. `gcc`/`clang`/`msvc` are the ones the guides are built
# around; the rest are reported when the source data has them.
TOOLCHAINS = {
    "gcc": "GCC (libstdc++)",
    "clang": "Clang (libc++)",
    "msvc": "MSVC (MSVC STL)",
    "aclang": "Apple Clang (libc++)",
    "edg": "EDG",
    "icc": "Intel oneAPI (icc)",
    "icx": "Intel oneAPI (icx)",
    "nvcc": "NVIDIA nvcc",
    "cray": "Cray",
    "embar": "Embarcadero",
    "ibmopenxlaix": "IBM Open XL (AIX)",
    "ibmopenxlzos": "IBM Open XL (z/OS)",
    "pgi": "PGI/NVIDIA HPC",
}
CORE_TOOLCHAINS = {"gcc", "clang", "msvc", "aclang", "edg", "icc", "icx", "nvcc",
                   "cray", "embar", "ibmopenxlaix", "ibmopenxlzos", "pgi"}

STATUS_YES = "yes"
STATUS_PARTIAL = "partial"
STATUS_NO = "no"


# --------------------------------------------------------------------------
# fetching
# --------------------------------------------------------------------------

def fetch(title: str, *, offline: bool) -> str:
    CACHE.mkdir(parents=True, exist_ok=True)
    cached = CACHE / (re.sub(r"[^A-Za-z0-9_.-]+", "_", title) + ".wiki")
    if offline:
        if not cached.exists():
            sys.exit(f"--offline: no cached copy of {title!r} at {cached}")
        return cached.read_text(encoding="utf-8")
    url = f"{WIKI}?title={urllib.parse.quote(title)}&action=raw"
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as resp:
        text = resp.read().decode("utf-8")
    cached.write_text(text, encoding="utf-8")
    return text


def fetch_revision(title: str, *, offline: bool) -> dict:
    """Page revision id and timestamp, for provenance. Empty dict offline."""
    if offline:
        return {}
    url = (f"{WIKI.replace('/w/index.php', '/w/api.php')}"
           f"?action=query&prop=revisions&rvprop=ids%7Ctimestamp&format=json"
           f"&titles={urllib.parse.quote(title)}")
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            doc = json.loads(resp.read().decode("utf-8"))
        pages = doc["query"]["pages"]
        for page in pages.values():
            rev = page.get("revisions", [{}])[0]
            return {"revid": rev.get("revid"), "rev_timestamp": rev.get("timestamp")}
    except Exception as exc:  # provenance is best effort, the table is not
        print(f"warning: revisions for {title}: {exc}", file=sys.stderr)
    return {}


# --------------------------------------------------------------------------
# wikitext helpers
# --------------------------------------------------------------------------

def split_params(inner: str) -> list[str]:
    """Split template parameters on top-level `|`, ignoring `{{ }}` and `[[ ]]`."""
    out, cur, braces, links = [], [], 0, 0
    i = 0
    while i < len(inner):
        two = inner[i:i + 2]
        if two == "{{":
            braces += 1
            cur.append(two)
            i += 2
            continue
        if two == "}}":
            braces = max(0, braces - 1)
            cur.append(two)
            i += 2
            continue
        if two == "[[":
            links += 1
            cur.append(two)
            i += 2
            continue
        if two == "]]":
            links = max(0, links - 1)
            cur.append(two)
            i += 2
            continue
        ch = inner[i]
        if ch == "|" and braces == 0 and links == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
        i += 1
    out.append("".join(cur))
    return out


def balanced_template(text: str, start: int) -> tuple[str, int]:
    """Return (inner text, index after the closing `}}`) of the template at `start`."""
    assert text.startswith("{{", start)
    depth, i = 0, start
    while i < len(text):
        if text.startswith("{{", i):
            depth += 1
            i += 2
        elif text.startswith("}}", i):
            depth -= 1
            i += 2
            if depth == 0:
                return text[start + 2:i - 2], i
        else:
            i += 1
    raise ValueError("unbalanced template")


def params_of(inner: str) -> dict[str, str]:
    params: dict[str, str] = {}
    for part in split_params(inner):
        if "=" not in part:
            continue
        key, _, value = part.partition("=")
        params[key.strip()] = value.strip()
    return params


def strip_comments(text: str) -> str:
    return re.sub(r"<!--.*?-->", "", text, flags=re.S)


LINK_RE = re.compile(r"\[\[([^\]|]+)(?:\|([^\]]*))?\]\]")
TEMPLATE_RE = re.compile(r"\{\{([^{}]*)\}\}")


def clean(text: str) -> str:
    """Reduce wikitext markup to the plain label a reader sees."""
    text = strip_comments(text)
    for _ in range(12):  # templates nest; converge
        before = text
        text = TEMPLATE_RE.sub(lambda m: _template_label(m.group(1)), text)
        if text == before:
            break
    text = LINK_RE.sub(lambda m: m.group(2) or m.group(1).split("#")[-1].split("/")[-1], text)
    text = text.replace("{{=", "{{=").replace("<br>", " ")
    text = (text.replace("&nbsp;", " ").replace("&amp;", "&").replace("&lt;", "<")
                .replace("&gt;", ">").replace("&quot;", '"').replace("&#160;", " "))
    text = re.sub(r"'''?", "", text)
    return re.sub(r"\s+", " ", text).strip()


def _template_label(inner: str) -> str:
    parts = split_params(inner)
    name = parts[0].strip().lower()
    args = [p for p in parts[1:] if "=" not in p.split("|")[0] or name.startswith("popup")]
    if name in ("header",):
        return "<" + clean(args[0]) + ">" if args else ""
    if name in ("ltt", "lt", "ltf", "lc", "ls", "lts"):
        # {{ls|cpp/language/x#Section}} and {{lt|cpp/language/lambda}} have no
        # label: the reader sees the section, or the last path component.
        if len(args) >= 2:
            return clean(args[-1])
        target = args[0] if args else ""
        if "#" in target:
            return clean(target.split("#", 1)[1])
        return clean(target.rstrip("/").split("/")[-1].replace("_", " "))
    if name in ("tt", "ttb", "code", "c"):
        return clean(args[-1]) if args else ""
    if name == "popup":
        # {{popup|note|value}} renders `value` with a tooltip; the note is captured
        # separately when a cell is parsed, so the label is the value.
        return clean(args[1]) if len(args) > 1 else ""
    if name in ("small", "petty", "yes", "no", "n/a", "maybe", "partial", "unknown"):
        return clean(args[-1]) if args else ""
    if name == "=":
        return "="
    if name.startswith("msvc ver"):
        return clean(args[-1]) if args else ""
    if name.startswith("aclang ver"):
        return clean(args[-1]) if args else ""
    if name in ("stddoc", "paper", "wg21"):
        return clean(args[-1]) if args else ""
    if name in ("ttb",):
        return clean(args[-1]) if args else ""
    return clean(args[-1]) if args else ""


def version_key(text: str) -> tuple[int, ...]:
    nums = re.findall(r"\d+", text or "")
    return tuple(int(n) for n in nums) if nums else ()


# --------------------------------------------------------------------------
# cells
# --------------------------------------------------------------------------

def parse_msvc_numbers(text: str) -> dict[str, str]:
    """`Template:msvc_ver/num` + `/title`: `2022.7` -> `19.37`, `19.44` -> `VS 2022 17.14`."""
    numbers, titles = {}, {}
    for suffix, target in (("num", numbers), ("title", titles)):
        raw = fetch(f"Template:msvc_ver/{suffix}", offline=False)
        for line in raw.splitlines():
            line = line.strip()
            if not line.startswith("|V") or "=" not in line:
                continue
            lhs, _, rhs = line.partition("=")
            rhs = clean(rhs)
            for alias in lhs.split("|"):
                alias = alias.strip()
                if alias.startswith("V"):
                    target[alias[1:]] = rhs
    return {"numbers": numbers, "titles": titles}


def resolve_version(text: str, msvc: dict[str, str]) -> str:
    text = clean(text)
    # `{{msvc ver|2022.11}}` renders as 19.41 (`msvc_ver/num`), and a bare
    # `2022.11` in a cell means the same: map aliases through the same table.
    if text in msvc:
        return msvc[text].split(" ")[0]
    return text


def parse_cell(raw: str, toolchain: str, msvc: dict[str, str]) -> dict:
    """One toolchain cell -> {status, min, notes}.

    `{{yes|13}}` -> yes/13; `{{n/a}}` -> no; `{{maybe|note|17}}` -> partial/17 with
    the note. Multiple releases in one cell (`12 (partial)<br>14`) mean the last
    one is the first complete release: `min=14`, the earlier one lands in `notes`.
    """
    cell = raw.strip()
    status = STATUS_YES
    inner = cell
    if cell.startswith("{{"):
        try:
            template_inner, _ = balanced_template(cell, 0)
        except ValueError:
            template_inner = cell[2:-2]
        parts = split_params(template_inner)
        name = parts[0].strip().lower()
        rest = "|".join(parts[1:])
        if name in ("n/a", "no", "none"):
            return {"status": STATUS_NO, "min": None, "notes": []}
        if name in ("maybe", "partial"):
            status = STATUS_PARTIAL
        elif name not in ("yes", ""):
            return {"status": STATUS_NO, "min": None, "notes": [clean(rest)] if rest else []}
        inner = rest

    notes: list[str] = []
    segments = [seg for seg in re.split(r"<br\s*/?>", inner) if seg.strip()]
    min_version = None
    for index, segment in enumerate(segments):
        note = ""
        value = segment
        for _ in range(4):
            m = re.search(r"\{\{popup\|([^{}]*)\}\}", value)
            if not m:
                break
            popup_args = split_params(m.group(1))
            note = clean(popup_args[0]) if popup_args else ""
            # {{popup|note|value}} keeps `value`; {{popup|note}} adds only the note
            value = value[:m.start()] + (clean(popup_args[1]) if len(popup_args) > 1 else "") + value[m.end():]
        value = resolve_version_templates(value, toolchain, msvc)
        # A paper reference in parentheses is not a version: `19.43 (P2242R3)`.
        value = re.sub(r"\([^)]*\)", " ", value)
        value = clean(value)
        nums = re.findall(r"\d+(?:\.\d+)*", value)
        is_last = index == len(segments) - 1
        if nums:
            if is_last:
                min_version = max(nums, key=version_key)
            if note or not is_last:
                notes.append((note + ": " if note else "") + value)
        elif note:
            notes.append(note)
    if status == STATUS_YES and not min_version:
        notes.append("release not recorded")
    return {"status": status, "min": min_version, "notes": [n for n in notes if n]}


def resolve_version_templates(text: str, toolchain: str, msvc: dict[str, str]) -> str:
    """`{{msvc ver|2022.7}}` -> `19.37`; other vendors pass their number through.

    The MSVC table maps Visual Studio aliases (`2022.7`, `2019.5`) to compiler
    versions, and must only ever be applied to the MSVC column: ICC's `2022.2` is a
    different product and a real version.
    """
    pattern = re.compile(r"\{\{\s*([a-z0-9 ]*?ver)\s*\|([^{}]*)\}\}")

    def replace(match):
        vendor, value = match.group(1), clean(match.group(2))
        if vendor.startswith("msvc") and toolchain == "msvc":
            # VS 2015 aliases are older than the mapping template keeps.
            if value.startswith("2015."):
                return "19.0"
            return msvc.get(value, value)
        if vendor.startswith("aclang"):
            return value
        return value

    for _ in range(4):
        updated = pattern.sub(replace, text)
        if updated == text:
            break
        text = updated
    return text


# --------------------------------------------------------------------------
# rows
# --------------------------------------------------------------------------

ROW_TEMPLATES = ("compiler_support_row", "standard library support row",
                 "standard_library_support_row", "compiler support row")


def extract_rows(text: str) -> list[dict]:
    text = strip_comments(text)
    rows: list[dict] = []
    i = 0
    while True:
        start = text.find("{{", i)
        if start < 0:
            break
        try:
            inner, after = balanced_template(text, start)
        except ValueError:
            i = start + 2
            continue
        parts = split_params(inner)
        name = parts[0].strip().lower()
        if name in ROW_TEMPLATES:
            params = params_of(inner)
            params["__kind__"] = "library" if "library" in name else "core"
            rows.append(params)
            i = after
        else:
            i = start + 2
    return rows


def slugify(text: str) -> str:
    text = text.lower()
    text = re.sub(r"std::|__cpp_lib_|__cpp_", "", text)
    text = re.sub(r"[^a-z0-9]+", "-", text).strip("-")
    return re.sub(r"-{2,}", "-", text)


def build_features() -> tuple[list[dict], list[dict], dict]:
    previous = {}
    existing = DATA / "matrix.json"
    if (ARGS.offline or ARGS.check_offline) and existing.exists():
        try:
            for source in json.loads(existing.read_text(encoding="utf-8")).get("sources", []):
                previous[source.get("title")] = source
        except Exception:
            previous = {}
    msvc_map = parse_msvc_numbers("")
    msvc_numbers = msvc_map["numbers"]
    features: list[dict] = []
    sources: list[dict] = []
    seen: set[str] = set()

    for std in STANDARDS:
        title = f"Template:cpp/compiler_support/{std}"
        text = fetch(title, offline=ARGS.offline or ARGS.check_offline)
        entry = {
            "title": title,
            "url": f"{WIKI}?title={urllib.parse.quote(title)}&action=raw",
            "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        }
        revision = fetch_revision(title, offline=ARGS.offline or ARGS.check_offline)
        if not revision and title in previous:
            # Offline runs keep the provenance of the last online sync.
            revision = {
                "revid": previous[title].get("revid"),
                "rev_timestamp": previous[title].get("rev_timestamp"),
            }
        entry.update(revision)
        sources.append(entry)
        for row in extract_rows(text):
            name = clean(row.get("feature", ""))
            if not name:
                continue
            kind = row["__kind__"]
            macro = clean(row.get("id", ""))
            paper = clean(row.get("reference", ""))
            version = clean(row.get("version", f"c++{std}")) or f"c++{std}"
            support = {}
            for toolchain in TOOLCHAINS:
                raw = row.get(toolchain, "")
                if not raw:
                    continue
                support[toolchain] = parse_cell(raw, toolchain, msvc_numbers)
            feature_id = f"cpp{std}.{kind}.{slugify(name)}"
            if feature_id in seen:
                continue
            seen.add(feature_id)
            features.append({
                "id": feature_id,
                "name": name,
                "kind": kind,
                "std": f"c++{std}",
                "version": version,
                "macro": macro,
                "paper": paper,
                "support": support,
            })

    license = {
        "title": "cppreference.com compiler support tables",
        "url": "https://en.cppreference.com/w/cpp/compiler_support",
        "license": "CC-BY-SA 4.0 / GFDL",
        "note": "Feature names, versions and feature-test macros extracted from the "
                "cppreference compiler support templates; see the URL above.",
    }
    return features, sources, {"msvc": msvc_map, "license": license}


# --------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------

def render_matrix(features: list[dict], sources: list[dict], meta: dict) -> dict:
    counts: dict[str, dict[str, int]] = {}
    for feature in features:
        bucket = counts.setdefault(feature["std"], {})
        bucket[feature["kind"]] = bucket.get(feature["kind"], 0) + 1
    return {
        "schema": "cpp-anything/matrix@1",
        "generated_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "generator": "scripts/sync_matrix.py",
        "source_license": meta["license"],
        "sources": sources,
        "toolchains": {t: {"label": label} for t, label in TOOLCHAINS.items()},
        "standards": [{"std": f"c++{std}", "features": counts.get(f"c++{std}", {})}
                      for std in STANDARDS],
        "msvc_versions": {
            "numbers": meta["msvc"]["numbers"],
            "titles": meta["msvc"]["titles"],
        },
        "features": features,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--offline", action="store_true",
                        help="parse only the cached wikitext (no network)")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if data/matrix.json differs from what a sync would write")
    parser.add_argument("--check-offline", action="store_true", help=argparse.SUPPRESS)
    global ARGS
    ARGS = parser.parse_args()

    features, sources, meta = build_features()
    matrix = render_matrix(features, sources, meta)
    target = DATA / "matrix.json"
    rendered = json.dumps(matrix, indent=1, ensure_ascii=False, sort_keys=False) + "\n"

    if ARGS.check:
        if not target.exists():
            print(f"{target} missing; run scripts/sync_matrix.py", file=sys.stderr)
            return 1
        current = json.loads(target.read_text(encoding="utf-8"))
        same_sources = current.get("sources") == sources
        same_features = current.get("features") == features
        if same_sources and same_features:
            print(f"{target}: up to date ({len(features)} features)")
            return 0
        print(f"{target}: stale (sources equal: {same_sources}, "
              f"features equal: {same_features}); run scripts/sync_matrix.py", file=sys.stderr)
        return 1

    DATA.mkdir(parents=True, exist_ok=True)
    target.write_text(rendered, encoding="utf-8")
    total = len(features)
    print(f"wrote {target.relative_to(ROOT)}: {total} features across "
          f"{len(STANDARDS)} standards, {len(sources)} source pages")
    for std in STANDARDS:
        core = sum(1 for f in features if f["std"] == f"c++{std}" and f["kind"] == "core")
        lib = sum(1 for f in features if f["std"] == f"c++{std}" and f["kind"] == "library")
        print(f"  c++{std:>2}: {core:>3} core, {lib:>3} library")
    return 0


if __name__ == "__main__":
    sys.exit(main())
