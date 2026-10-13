#!/usr/bin/env python3
"""The inverse of tools/rehost.py, for the production cutover (docs/CUTOVER.md)."""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import rehost  # noqa: E402

DEFAULT_ROOTS = ("app/src", "app/public", "app/tools", "app/tests", "app/browser_tests",
                 "app/wrangler.toml", "world", "marketplace")
SKIP_FILES = frozenset({
    "app/tests/test_next_no_production_hosts.py",
})

SKIP_PREFIXES = ("app/tests/host/",)

INVERSE_MAP = {
    "next.forkmesh.com": "forkmesh.com",
    "app.next.forkmesh.com": "app.forkmesh.com",
    "world.next.forkmesh.com": "world.forkmesh.com",
    "ssh.next.forkmesh.com": "ssh.forkmesh.com",
    "users.noreply.next.forkmesh.com": "users.noreply.forkmesh.com",
}

HOST_RE = re.compile(
    r"(?:(?<=%2F)|(?<=%2f)|(?<=:-)|(?<![A-Za-z0-9._%-]))"
    r"((?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)*)next\.forkmesh\.com"
    r"(?![A-Za-z0-9-]|\.[A-Za-z0-9])"
)
SUFFIX_RE = re.compile(
    r"(?<![A-Za-z0-9_-])\.next\.forkmesh\.com(?![A-Za-z0-9-]|\.[A-Za-z0-9])")


DUPLICATE_APEX_RE = re.compile(
    r"(?P<q>[\"'])(?P<scheme>(?:https://)?)forkmesh\.com(?P=q)(?P<sep>\s*,\s*)"
    r"(?P=q)(?P=scheme)forkmesh\.com(?P=q)")


def _restore_www(match):
    q, scheme = match.group("q"), match.group("scheme") or ""
    return "%s%sforkmesh.com%s%s%s%swww.forkmesh.com%s" % (
        q, scheme, q, match.group("sep"), q, scheme, q)


def rewrite_text(text: str):
    count = 0
    kept = []
    out = []
    last = 0
    for match in HOST_RE.finditer(text):
        host = match.group(0).lower()
        target = INVERSE_MAP.get(host)
        if target is None:
            kept.append(host)
            continue
        out.append(text[last:match.start()])
        out.append(target)
        last = match.end()
        count += 1
    out.append(text[last:])
    text, suffixes = SUFFIX_RE.subn(".forkmesh.com", "".join(out))
    text, restored = DUPLICATE_APEX_RE.subn(_restore_www, text)
    return text, count + suffixes + restored, kept


def iter_files(root: Path, roots):
    for item in roots:
        base = root / item
        if base.is_file():
            yield base
            continue
        if not base.is_dir():
            continue
        stack = [base]
        while stack:
            current = stack.pop()
            for entry in sorted(current.iterdir()):
                if entry.is_symlink():
                    continue
                if entry.is_dir():
                    if entry.name not in rehost.SKIP_DIRS:
                        stack.append(entry)
                    continue
                if entry.suffix.lower() in rehost.TEXT_SUFFIX_DENY:
                    continue
                yield entry


def scan(root: Path, roots=DEFAULT_ROOTS, apply=False):
    counts, kept = {}, {}
    for path in iter_files(root, roots):
        rel = path.relative_to(root).as_posix()
        if (rel in SKIP_FILES or rel in rehost.FROZEN_FILES
                or rel.startswith(SKIP_PREFIXES)):
            continue
        text = rehost._read_text(path)
        if text is None or "next.forkmesh.com" not in text:
            continue
        new_text, count, file_kept = rewrite_text(text)
        if count:
            counts[rel] = count
            if apply:
                path.write_text(new_text, encoding="utf-8")
        if file_kept:
            kept[rel] = sorted(set(file_kept))
    return counts, kept


def compare(root: Path, production: Path, roots=DEFAULT_ROOTS):
    report = {}
    for path in iter_files(root, roots):
        rel = path.relative_to(root).as_posix()
        other = production / rel
        if rel in SKIP_FILES or not other.is_file():
            continue
        text = rehost._read_text(path)
        theirs = rehost._read_text(other)
        if text is None or theirs is None or "forkmesh.com" not in text + theirs:
            continue
        mine = rewrite_text(text)[0]
        if mine == theirs:
            continue
        diff = [line for line in difflib.unified_diff(
            theirs.splitlines(), mine.splitlines(), lineterm="", n=0)
            if line[:1] in "+-" and not line.startswith(("+++", "---"))
            and "forkmesh.com" in line]
        if diff:
            report[rel] = diff
    return report


def on_main_line(root: Path) -> bool:
    import subprocess
    try:
        branch = subprocess.run(
            ["git", "-C", str(root), "symbolic-ref", "--short", "-q", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return False
    return branch in ("main", "master")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--apply", action="store_true")
    mode.add_argument("--compare", action="store_true")
    parser.add_argument("--root", default=str(HERE.parent))
    parser.add_argument("--production", default=None)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    if args.apply and on_main_line(root):
        print("cutover_rehost: refusing to rewrite a checkout of main; --apply "
              "only on a release worktree (docs/CUTOVER.md)", file=sys.stderr)
        return 2
    if args.compare:
        if not args.production:
            parser.error("--compare needs --production")
        report = compare(root, Path(args.production).expanduser())
        lines = sum(len(v) for v in report.values())
        for rel, diff in sorted(report.items()):
            print("%s: %d line(s)" % (rel, len(diff)))
            if args.verbose:
                for line in diff[:12]:
                    print("    " + line[:160])
        print("compare: %d file(s), %d line(s) naming forkmesh.com differ from "
              "production" % (len(report), lines))
        return 0
    counts, kept = scan(root, apply=args.apply)
    verb = "rewrote" if args.apply else "would rewrite"
    per_dir = {}
    for rel, count in counts.items():
        top = "/".join(rel.split("/")[:2])
        per_dir[top] = per_dir.get(top, 0) + count
    for top, count in sorted(per_dir.items()):
        print("%s %d name(s) in %s" % (verb, count, top))
    names = sorted({h for hosts in kept.values() for h in hosts})
    if names:
        print("kept (Next node names): %s" % ", ".join(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
