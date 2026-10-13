#!/usr/bin/env python3
"""Static + runtime health report for the desktop codebase.

Emits one JSON document describing every source file as a "component": its
size, its functions, the patterns that historically produced UI stalls, and
the stall time the diagnostics logs actually attribute to it. The desktop's
Component health page renders this; it is also usable straight from a shell
for stall triage.

The suspicious-pattern rules are not generic lint. Every one of them is a
class that has already frozen this app at least once (see the stall-watchdog
rounds): git waits, per-widget setStyleSheet, QSettings built per call,
regular expressions compiled per iteration, and event-loop pumping inside a
loop body.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402  (sibling module; tools run as scripts)

# --- source scanning -------------------------------------------------------

# A definition line like "void MainWindow::foo(...)" or "QString Helper::bar("
DEFINITION = re.compile(
    r"^(?:[A-Za-z_][\w:<>,&*\s]*?[\s*&])?"      # optional return type, `T *` too
    r"(?P<qual>[A-Za-z_]\w*(?:::[A-Za-z_~]\w*)+|[A-Za-z_]\w*)"
    r"\s*\((?![^)]*\)\s*;)"                      # not a declaration
)
LOOP = re.compile(r"\b(for|while)\s*\(")
# Each rule: (key, regex, human explanation). Matched inside loop bodies only.
LOOP_HAZARDS = [
    ("git-in-loop",
     re.compile(r"\b(runGit|waitForGit|waitForFinishedKeepAlive|QProcess)\b"),
     "spawns or waits on a subprocess per iteration"),
    ("stylesheet-in-loop",
     re.compile(r"\bsetStyleSheet\s*\("),
     "re-runs the QSS selector match per widget instead of once on the parent"),
    ("settings-in-loop",
     re.compile(r"\bQSettings\s+\w+|\bQSettings\s*\("),
     "constructs QSettings per iteration (reparses the whole INI)"),
    ("regex-in-loop",
     # A `static` local compiles once, so a line declaring one is skipped.
     re.compile(r"^(?![^\n]*\bstatic\b)[^\n]*\bQRegularExpression\s*(?:\w+\s*)?\(", re.M),
     "compiles a regular expression per iteration"),
    ("pump-in-loop",
     re.compile(r"\b(processEvents|pumpKeepAlive)\s*\("),
     "pumps the event loop per iteration (re-entrancy risk)"),
    ("dir-walk-in-loop",
     re.compile(r"\b(QDirIterator|entryList|entryInfoList|removeRecursively)\b"),
     "walks the filesystem per iteration"),
    ("icon-build-in-loop",
     re.compile(r"\b(QPainter|QFontMetrics|QImageReader|QPixmap\s*\()"),
     "rasterises per iteration instead of caching by key"),
]
# Cheap whole-function signals, independent of loops.
BLOCKING_CALL = re.compile(
    r"\b(waitForGit|waitForFinished|pumpKeepAlive|processEvents)\s*\(")


def strip_noise(text: str) -> str:
    """Blank comments and literal bodies, preserving every line and column.

    Line-oriented on purpose: an earlier character-stream version let an
    unterminated literal run past the newline, which silently deleted lines and
    threw every reported line number off by hundreds.
    """
    out = []
    in_block = False
    for line in text.split("\n"):
        chars = list(line)
        i, n = 0, len(chars)
        result = []
        while i < n:
            ch = chars[i]
            if in_block:
                if ch == "*" and i + 1 < n and chars[i + 1] == "/":
                    in_block = False
                    result.append("  ")
                    i += 2
                else:
                    result.append(" ")
                    i += 1
                continue
            if ch == "/" and i + 1 < n and chars[i + 1] == "/":
                result.append(" " * (n - i))
                i = n
                continue
            if ch == "/" and i + 1 < n and chars[i + 1] == "*":
                in_block = True
                result.append("  ")
                i += 2
                continue
            if ch in "\"'":
                quote = ch
                j = i + 1
                while j < n and chars[j] != quote:
                    j += 2 if chars[j] == "\\" else 1
                # An unterminated literal ends at the newline, never beyond it.
                j = min(j + 1, n)
                result.append(" " * (j - i))
                i = j
                continue
            result.append(ch)
            i += 1
        out.append("".join(result))
    return "\n".join(out)


def scan_functions(clean: str, raw_lines: list[str]) -> list[dict]:
    """Brace-match every definition to get real spans and loop nesting."""
    functions = []
    lines = clean.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if (not stripped or stripped.startswith("#")
                or stripped.startswith("}")
                or re.match(r"^(if|for|while|switch|catch|return|else)\b",
                            stripped)):
            i += 1
            continue
        match = DEFINITION.match(line)
        if not match or "::" not in match.group("qual") and not line.startswith(
                ("void ", "bool ", "int ", "QString ", "static ")):
            i += 1
            continue
        # Find the opening brace within a few lines (multi-line signatures).
        depth, start, opened = 0, i, False
        j = i
        while j < len(lines) and j < i + 12 and not opened:
            if "{" in lines[j]:
                opened = True
                break
            if ";" in lines[j]:
                break
            j += 1
        if not opened:
            i += 1
            continue
        # Brace-match to the end of the body.
        k, depth, body_start = j, 0, j
        while k < len(lines):
            depth += lines[k].count("{") - lines[k].count("}")
            if depth <= 0 and k >= body_start:
                break
            k += 1
        end = min(k, len(lines) - 1)
        body = "\n".join(lines[body_start:end + 1])
        if end - start < 1:
            i = j + 1
            continue
        functions.append({
            "name": match.group("qual"),
            "line": start + 1,
            "endLine": end + 1,
            "loc": end - start + 1,
            "body": body,
        })
        i = end + 1
    return functions


def statement_end(text: str, pos: int) -> int:
    """Offset one past the statement starting at ``pos``.

    A braced block ends at its matching brace; anything else at the first ';'
    outside parentheses and braces, so ``for (...) x;`` ends there instead of
    running on to whatever block closes next and swallowing sibling loops.
    """
    n = len(text)
    i = pos
    while i < n and text[i].isspace():
        i += 1
    parens = braces = 0
    while i < n:
        ch = text[i]
        if ch == "(":
            parens += 1
        elif ch == ")":
            parens -= 1
        elif ch == "{":
            braces += 1
        elif ch == "}":
            braces -= 1
            if braces < 0:
                return i
            if braces == 0 and parens == 0:
                k = i + 1
                while k < n and text[k].isspace():
                    k += 1
                if not text.startswith("else", k):
                    return k + 1 if text.startswith(";", k) else i + 1
        elif ch == ";" and parens <= 0 and braces == 0:
            return i + 1
        i += 1
    return n


def loop_bodies(body: str):
    """Yield (nesting_depth, text) for each loop in a function body."""
    spans = []
    for match in LOOP.finditer(body):
        i, parens = match.end() - 1, 0
        semicolon = range_colon = False
        while i < len(body):
            ch = body[i]
            if ch == "(":
                parens += 1
            elif ch == ")":
                parens -= 1
                if parens == 0:
                    break
            elif parens == 1 and ch == ";":
                semicolon = True
            elif (parens == 1 and ch == ":" and body[i - 1] != ":"
                  and body[i + 1:i + 2] != ":"):
                range_colon = True
            i += 1
        # A range-for header runs once; a classic for/while header every pass.
        range_for = match.group(1) == "for" and range_colon and not semicolon
        repeats = min(i + 1, len(body)) if range_for else match.start()
        spans.append((match.start(), repeats, statement_end(body, i + 1)))
    for index, (head, repeats, end) in enumerate(spans):
        nesting = 1 + sum(1 for _, _, other_end in spans[:index] if other_end > head)
        yield nesting, body[repeats:end]


def blank_window_test_only(lines: list[str]) -> list[str]:
    """Blank code compiled only into the window-test binary.

    Its test hooks pump the event loop on purpose and the shipped app never
    runs them. Line count is kept; an ``#else`` branch is the app's and stays.
    """
    out, stack = [], []
    for line in lines:
        directive = line.strip()
        if directive.startswith("#"):
            word = directive[1:].strip()
            if word.startswith("if"):
                stack.append(word.startswith("ifdef")
                             and word[5:].strip() == "FORKMESH_WINDOW_TESTS")
            elif word.startswith("el") and stack:
                stack[-1] = False
            elif word.startswith("endif") and stack:
                stack.pop()
            out.append(line)
            continue
        out.append(" " * len(line) if any(stack) else line)
    return out


ACKNOWLEDGED = re.compile(
    r"code-health:\s*allow\(\s*([a-z-]+(?:\s*,\s*[a-z-]+)*)\s*\)\s*\S")


def acknowledged_kinds(raw_lines: list[str], first: int, last: int) -> set[str]:
    """Kinds cleared by ``// code-health: allow(kind) reason`` for one function.

    The marker sits in the comment block directly above the definition or in
    its body, and needs a reason, so every exemption stays reviewable.
    """
    start = max(0, min(first, len(raw_lines)))
    while start > 0 and raw_lines[start - 1].strip().startswith(("//", "*", "/*")):
        start -= 1
    kinds = set()
    for line in raw_lines[start:last]:
        match = ACKNOWLEDGED.search(line)
        if match:
            kinds.update(k.strip() for k in match.group(1).split(","))
    return kinds


def analyse_file(path: str, root: str) -> dict:
    raw = open(path, encoding="utf-8", errors="replace").read()
    raw_lines = raw.splitlines()
    clean = "\n".join(blank_window_test_only(strip_noise(raw).split("\n")))
    functions = scan_functions(clean, raw_lines)
    findings = []
    acknowledged = 0
    out_functions = []
    for fn in functions:
        fn_findings = []
        worst_nesting = 0
        for nesting, chunk in loop_bodies(fn["body"]):
            worst_nesting = max(worst_nesting, nesting)
            for key, rule, why in LOOP_HAZARDS:
                if rule.search(chunk):
                    fn_findings.append({"kind": key, "why": why})
        if worst_nesting >= 3:
            fn_findings.append({
                "kind": "deep-nested-loop",
                "why": f"{worst_nesting} nested loop levels",
            })
        if BLOCKING_CALL.search(fn["body"]):
            fn_findings.append({
                "kind": "blocking-wait",
                "why": "waits on the event loop or a worker from this function",
            })
        allowed = (acknowledged_kinds(raw_lines, fn["line"] - 1, fn["endLine"])
                   if fn_findings else set())
        # de-duplicate by kind, keep first explanation
        seen, unique = set(), []
        for f in fn_findings:
            if f["kind"] in seen:
                continue
            if f["kind"] in allowed:
                seen.add(f["kind"])
                acknowledged += 1
                continue
            seen.add(f["kind"])
            unique.append(f)
        out_functions.append({
            "name": fn["name"],
            "line": fn["line"],
            "loc": fn["loc"],
            "loopDepth": worst_nesting,
            "findings": unique,
        })
        findings.extend(unique)
    rel = os.path.relpath(path, root)
    return {
        "path": rel,
        "component": component_for(rel),
        "loc": len(raw_lines),
        "codeLoc": sum(
            1 for line in clean.splitlines() if line.strip()),
        "functions": out_functions,
        "functionCount": len(out_functions),
        "findingCount": len(findings),
        "acknowledgedCount": acknowledged,
        "findingKinds": dict(collections.Counter(f["kind"] for f in findings)),
    }


def component_for(rel: str) -> str:
    """Group files into the areas a person actually thinks in."""
    base = os.path.basename(rel)
    stem = base.rsplit(".", 1)[0]
    if stem.startswith("MainWindow") and stem != "MainWindow":
        return "UI · " + re.sub(r"(?<!^)(?=[A-Z])", " ", stem[len("MainWindow"):])
    for prefix, label in (("Agent", "Agents"), ("Chat", "Chat"),
                          ("Pull", "Pulls"), ("Issue", "Issues"),
                          ("Mirror", "Mirrors"), ("Node", "Mesh"),
                          ("Stats", "Stats"), ("Stall", "Diagnostics"),
                          ("Repo", "Repositories")):
        if stem.startswith(prefix):
            return label
    return "Core"


# --- runtime attribution ---------------------------------------------------

STALL_RECORD = re.compile(
    r"===== (STALL-\S+) BEGIN =====(.*?)===== \1 END =====", re.S)
MANGLED = re.compile(r"\b(_Z[A-Za-z0-9_]+)")
PLAIN_SYMBOL = re.compile(r"\b([A-Za-z_]\w*(?:::[A-Za-z_~]\w*)+)\s*\(")


def demangle_all(symbols: list[str]) -> dict[str, str]:
    if not symbols:
        return {}
    import subprocess
    try:
        proc = subprocess.run(["c++filt"], input="\n".join(symbols),
                              capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return {}
    out = proc.stdout.splitlines()
    return dict(zip(symbols, out))


DISPATCH_RESIDUE = {
    "MainWindow::handleFramelessResizeEvent",
    "MainWindow::resizeEdgesAtGlobalPos",
    "MainWindow::updateFramelessResizeCursor",
}


def is_dispatch_residue(name: str) -> bool:
    return name.endswith("::eventFilter") or name in DISPATCH_RESIDUE


def stall_attribution(stalls_path: str) -> dict:
    """Map stall milliseconds onto the function names the samples name."""
    if not os.path.exists(stalls_path):
        return {"functions": {}, "totalMs": 0, "records": 0, "unattributedMs": 0,
                "starvedMs": 0}
    txt = open(stalls_path, errors="replace").read()
    per_function = collections.Counter()
    total = unattributed = starved = 0
    records = 0
    pending = []
    for sid, body in STALL_RECORD.findall(txt):
        m = re.search(r"UI stalled ~(\d+) ms", body)
        ms = int(m.group(1)) if m else 0
        total += ms
        records += 1
        # The watchdog's own verdict that the machine was paging: the frames
        # show where the GUI thread waited, not code at fault.
        if "verdict: machine-wide memory/io starvation" in body:
            starved += ms
            continue
        # Only stack frames and "likely origin:" name code. Prose parenthesises
        # ordinary words: "git rev-parse main (forkmesh)" is not a frame.
        frames = "\n".join(line for line in body.split("\n")
                           if line.startswith("likely origin:")
                           or "+0x" in line or "[0x" in line)
        names = set(PLAIN_SYMBOL.findall(frames))
        mangled = set(MANGLED.findall(frames))
        pending.append((ms, names, mangled))
    table = demangle_all(sorted({s for _, _, m in pending for s in m}))
    for ms, names, mangled in pending:
        resolved = set(names)
        for sym in mangled:
            dem = table.get(sym, "")
            hit = PLAIN_SYMBOL.search(dem + "(")
            if hit:
                resolved.add(hit.group(1))
        # Keep only app-looking symbols; Qt/std frames are not our components.
        app = {n for n in resolved
               if not n.startswith(("Q", "std::", "__gnu", "boost::"))
               and not is_dispatch_residue(n)}
        if not app:
            unattributed += ms
            continue
        share = ms / len(app)
        for name in app:
            per_function[name] += share
    return {
        "functions": {k: round(v) for k, v in per_function.items()},
        "totalMs": total,
        "records": records,
        "unattributedMs": unattributed,
        "starvedMs": starved,
    }


def actions_attribution(actions_path: str, limit_bytes: int = 24_000_000) -> dict:
    """Sum ui-blocking milliseconds per action detail from actions.jsonl."""
    if not os.path.exists(actions_path):
        return {}
    per_detail = collections.Counter()
    counts = collections.Counter()
    started: dict[str, tuple] = {}
    size = os.path.getsize(actions_path)
    with open(actions_path, errors="replace") as handle:
        if size > limit_bytes:
            handle.seek(size - limit_bytes)
            handle.readline()
        for line in handle:
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("execution") != "ui-blocking":
                continue
            key = normalise_detail(str(row.get("detail", "")))
            phase, ticket = row.get("phase"), row.get("id")
            if phase == "started" and ticket:
                started[ticket] = (key, row.get("ms") or row.get("at"))
            elif phase == "finished":
                ms = row.get("durationMs")
                if ms is None:
                    continue
                prior = started.pop(ticket, None)
                key = prior[0] if prior else key
                per_detail[key] += int(ms)
                counts[key] += 1
    return {
        "byDetail": {k: v for k, v in per_detail.most_common(200)},
        "counts": {k: counts[k] for k, _ in per_detail.most_common(200)},
    }


def normalise_detail(detail: str) -> str:
    detail = re.sub(r"\b[0-9a-f]{7,40}\b", "<sha>", detail)
    detail = re.sub(r"\b\d+\b", "<n>", detail)
    return detail[:120]


# --- report ----------------------------------------------------------------

def build(root: str, sources: list[str], diagnostics: str) -> dict:
    files = []
    for source_dir in sources:
        base = os.path.join(root, source_dir)
        if not os.path.isdir(base):
            continue
        for entry in sorted(os.listdir(base)):
            if not entry.endswith((".cpp", ".h")):
                continue
            files.append(analyse_file(os.path.join(base, entry), root))
    stalls = stall_attribution(os.path.join(diagnostics, "stalls.log"))
    actions = actions_attribution(os.path.join(diagnostics, "actions.jsonl"))

    # Fold stall milliseconds into the file that defines each named function.
    owner = {}
    for record in files:
        for fn in record["functions"]:
            owner.setdefault(fn["name"], record["path"])
    for record in files:
        record["stallMs"] = 0
        record["stallFunctions"] = {}
    index = {record["path"]: record for record in files}
    for name, ms in stalls["functions"].items():
        path = owner.get(name)
        if not path:
            # Try the class half: "MainWindow::foo" defined across MainWindow*.cpp
            cls = name.split("::")[0]
            candidates = [p for p in index if p.endswith(f"{cls}.cpp")]
            path = candidates[0] if candidates else None
        if not path:
            continue
        index[path]["stallMs"] += ms
        index[path]["stallFunctions"][name] = ms
        for fn in index[path]["functions"]:
            if fn["name"] == name:
                fn["stallMs"] = ms

    for record in files:
        record["health"] = score(record)
    files.sort(key=lambda r: (r["health"], -r["stallMs"]))
    return {
        "schema": "forkmesh.code-health/v1",
        "root": root,
        "files": files,
        "acknowledgedCount": sum(r["acknowledgedCount"] for r in files),
        "stalls": {k: v for k, v in stalls.items() if k != "functions"},
        "actions": actions,
        "components": component_rollup(files),
    }


def score(record: dict) -> int:
    """0-100. Size alone is not unhealthy; unbounded work on the UI is."""
    points = 100
    points -= min(30, record["findingCount"] * 3)
    points -= min(30, int(record["stallMs"] / 1000))
    longest = max((f["loc"] for f in record["functions"]), default=0)
    if longest > 400:
        points -= 15
    elif longest > 200:
        points -= 8
    if record["loc"] > 8000:
        points -= 10
    elif record["loc"] > 3000:
        points -= 5
    return max(0, points)


def component_rollup(files: list[dict]) -> list[dict]:
    groups: dict[str, dict] = {}
    for record in files:
        g = groups.setdefault(record["component"], {
            "component": record["component"], "loc": 0, "files": 0,
            "functions": 0, "findings": 0, "stallMs": 0})
        g["loc"] += record["loc"]
        g["files"] += 1
        g["functions"] += record["functionCount"]
        g["findings"] += record["findingCount"]
        g["stallMs"] += record["stallMs"]
    for g in groups.values():
        g["health"] = max(0, 100 - min(40, g["findings"])
                          - min(40, int(g["stallMs"] / 1000)))
    return sorted(groups.values(), key=lambda g: g["health"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=os.getcwd())
    parser.add_argument("--source", action="append",
                        default=None, help="directory under root to scan")
    parser.add_argument("--diagnostics",
                        default=str(forkmesh_paths.resolve().diagnostics))
    parser.add_argument("--out", default="-")
    parser.add_argument("--top", type=int, default=0,
                        help="print a human summary of the N worst files")
    args = parser.parse_args()
    sources = args.source or ["desktop/src"]
    report = build(args.root, sources, args.diagnostics)
    if args.top:
        print(f"{len(report['files'])} files | "
              f"{sum(f['loc'] for f in report['files']):,} lines | "
              f"{sum(f['functionCount'] for f in report['files']):,} functions")
        s = report["stalls"]
        print(f"stalls: {s['records']} records, {s['totalMs']/1000:.0f}s total, "
              f"{s['unattributedMs']/1000:.0f}s with no app frame")
        print(f"\n{'health':>6}  {'lines':>6}  {'stall':>7}  {'flags':>5}  file")
        for record in report["files"][:args.top]:
            print(f"{record['health']:6d}  {record['loc']:6d}  "
                  f"{record['stallMs']/1000:6.1f}s  {record['findingCount']:5d}  "
                  f"{record['path']}")
        print("\n=== worst components ===")
        for g in report["components"][:10]:
            print(f"{g['health']:6d}  {g['loc']:7d} lines  "
                  f"{g['stallMs']/1000:6.1f}s  {g['findings']:4d} flags  "
                  f"{g['component']}")
    if args.out != "-":
        with open(args.out, "w") as handle:
            json.dump(report, handle, indent=1)
        print(f"wrote {args.out}", file=sys.stderr)
    elif not args.top:
        json.dump(report, sys.stdout, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
