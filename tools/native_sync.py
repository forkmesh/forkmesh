#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEDGER = ROOT / "docs" / "native" / "SYNC.md"
START = "<!-- ledger:start -->"
END = "<!-- ledger:end -->"
TRIAGED = re.compile(r"<!-- triaged-through: ([0-9a-f]{7,40}) -->")
HEADER = "| ID | Date | Area | Kind | Change on iOS | iOS commit | Android | Android commit or reason |"
RULE = "|---|---|---|---|---|---|---|---|"
KINDS = ("feature", "ui", "fix")
STATES = ("pending", "doing", "synced", "n/a")
SHA = re.compile(r"\b[0-9a-f]{7,40}\b")


@dataclass
class Row:
    id: str
    date: str
    area: str
    kind: str
    change: str
    ios: str
    android: str
    ref: str

    def cells(self) -> list[str]:
        return [self.id, self.date, self.area, self.kind, self.change, self.ios, self.android, self.ref]

    def line(self) -> str:
        return "| " + " | ".join(cell.replace("|", "\\|") for cell in self.cells()) + " |"


def split_cells(line: str) -> list[str]:
    body = line.strip()[1:-1]
    return [cell.strip().replace("\\|", "|") for cell in re.split(r"(?<!\\)\|", body)]


def read_ledger(path: Path | None = None) -> tuple[str, list[Row], str, str]:
    path = path or LEDGER
    text = path.read_text()
    if START not in text or END not in text:
        raise SystemExit(f"{path}: missing {START} / {END} markers")
    head, rest = text.split(START, 1)
    body, tail = rest.split(END, 1)
    rows = []
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith("|") or line in (HEADER, RULE):
            continue
        cells = split_cells(line)
        if len(cells) != 8:
            raise SystemExit(f"{path}: row has {len(cells)} cells, expected 8: {line}")
        rows.append(Row(*cells))
    match = TRIAGED.search(text)
    return head, rows, tail, match.group(1) if match else ""


def write_ledger(head: str, rows: list[Row], tail: str) -> None:
    lines = [HEADER, RULE] + [row.line() for row in rows]
    LEDGER.write_text(head + START + "\n" + "\n".join(lines) + "\n" + END + tail)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def problems(rows: list[Row]) -> list[str]:
    found = []
    seen = set()
    for row in rows:
        if not re.fullmatch(r"S\d{3,}", row.id):
            found.append(f"{row.id}: ID must look like S001")
        if row.id in seen:
            found.append(f"{row.id}: duplicate ID")
        seen.add(row.id)
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", row.date):
            found.append(f"{row.id}: date must be YYYY-MM-DD")
        if row.kind not in KINDS:
            found.append(f"{row.id}: kind must be one of {', '.join(KINDS)}")
        if row.android not in STATES:
            found.append(f"{row.id}: Android must be one of {', '.join(STATES)}")
        if not row.change:
            found.append(f"{row.id}: empty change")
        if row.android == "synced" and not SHA.search(row.ref):
            found.append(f"{row.id}: a synced row names the Android commit")
        if row.android == "n/a" and not row.ref:
            found.append(f"{row.id}: an n/a row says why")
    return found


def cited(rows: list[Row]) -> set[str]:
    return {sha for row in rows for sha in SHA.findall(row.ios)}


def is_cited(sha: str, shas: set[str]) -> bool:
    return any(sha.startswith(known) or known.startswith(sha) for known in shas)


IOS_PATHS = (
    "native/ios",
    ":(exclude)native/ios/ForkMesh/Resources/Fonts/material_icons_regular.otf",
    ":(exclude,glob)native/ios/**/*.md",
)


def untracked(rows: list[Row], since: str) -> list[tuple[str, str, str]]:
    span = f"{since}..HEAD" if since else "HEAD"
    out = git("log", "--no-merges", "--reverse", "--abbrev=10", "--format=%h%x09%ad%x09%s", "--date=short", span, "--", *IOS_PATHS)
    shas = cited(rows)
    pending = []
    for line in out.splitlines():
        sha, date, subject = line.split("\t", 2)
        if not is_cited(sha, shas):
            pending.append((sha, date, subject))
    return pending


def next_id(rows: list[Row]) -> str:
    numbers = [int(row.id[1:]) for row in rows if re.fullmatch(r"S\d+", row.id)]
    return f"S{(max(numbers) + 1 if numbers else 1):03d}"


def cmd_status(args) -> int:
    _, rows, _, since = read_ledger()
    counts = {state: sum(row.android == state for row in rows) for state in STATES}
    print(f"rows {len(rows)}: " + ", ".join(f"{state} {count}" for state, count in counts.items()))
    print(f"iOS triaged through {since or '(nothing)'}; untriaged iOS commits: {len(untracked(rows, since))}")
    for state in ("doing", "pending"):
        open_rows = [row for row in rows if row.android == state and (not args.area or row.area.lower() == args.area.lower())]
        if open_rows:
            print(f"\n{state}:")
            for row in sorted(open_rows, key=lambda r: (r.area, r.id)):
                print(f"  {row.id}  {row.area:<14} {row.kind:<8} {row.change}")
    return 0


def cmd_untracked(args) -> int:
    _, rows, _, since = read_ledger()
    for sha, date, subject in untracked(rows, since):
        print(f"{sha}  {date}  {subject}")
    return 0


def cmd_add(args) -> int:
    head, rows, tail, _ = read_ledger()
    if args.kind not in KINDS or args.android not in STATES:
        raise SystemExit("bad --kind or --android")
    date = args.date or git("log", "-1", "--format=%ad", "--date=short", args.sha).strip()
    sha = git("rev-parse", "--short=10", args.sha).strip()
    row = Row(next_id(rows), date, args.area, args.kind, args.change, sha, args.android, args.ref or "")
    rows.append(row)
    bad = problems([row])
    if bad:
        raise SystemExit("\n".join(bad))
    write_ledger(head, rows, tail)
    print(row.id)
    return 0


def cmd_mark(args) -> int:
    head, rows, tail, _ = read_ledger()
    wanted = set(args.ids)
    hit = [row for row in rows if row.id in wanted]
    if len(hit) != len(wanted):
        raise SystemExit(f"unknown IDs: {', '.join(sorted(wanted - {row.id for row in hit}))}")
    for row in hit:
        if args.synced:
            row.android, row.ref = "synced", git("rev-parse", "--short=10", args.synced).strip()
        elif args.na:
            row.android, row.ref = "n/a", args.na
        elif args.doing:
            row.android = "doing"
        else:
            row.android, row.ref = "pending", ""
    bad = problems(hit)
    if bad:
        raise SystemExit("\n".join(bad))
    write_ledger(head, rows, tail)
    return 0


def cmd_triage(args) -> int:
    text = LEDGER.read_text()
    _, rows, _, since = read_ledger()
    sha = git("rev-parse", "--short=10", args.sha or "HEAD").strip()
    after = set(git("log", "--no-merges", "--abbrev=10", "--format=%h", f"{sha}..HEAD", "--", *IOS_PATHS).split())
    missing = [item for item in untracked(rows, since) if not is_cited(item[0], after)]
    if missing and not args.force:
        print("these iOS commits have no row yet; add them, or pass --force once you have checked they change nothing a user sees:")
        for item in missing:
            print(f"  {item[0]}  {item[1]}  {item[2]}")
        return 1
    LEDGER.write_text(TRIAGED.sub(f"<!-- triaged-through: {sha} -->", text, count=1))
    print(sha)
    return 0


def cmd_check(args) -> int:
    _, rows, _, since = read_ledger()
    bad = problems(rows)
    if not since:
        bad.append("missing <!-- triaged-through: <sha> --> marker")
    if args.strict:
        bad += [f"untriaged iOS commit {sha} {subject}" for sha, _, subject in untracked(rows, since)]
    for line in bad:
        print(line)
    return 1 if bad else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Track iOS changes that Android still has to match (docs/native/SYNC.md).")
    sub = parser.add_subparsers(dest="command", required=True)
    status = sub.add_parser("status", help="counts and the open rows")
    status.add_argument("--area")
    status.set_defaults(func=cmd_status)
    sub.add_parser("untracked", help="iOS commits after the triage marker with no row").set_defaults(func=cmd_untracked)
    add = sub.add_parser("add", help="register an iOS change")
    add.add_argument("sha")
    add.add_argument("change", help="one plain sentence a user would understand")
    add.add_argument("--area", required=True)
    add.add_argument("--kind", default="feature", choices=KINDS)
    add.add_argument("--android", default="pending", choices=STATES)
    add.add_argument("--ref", help="Android commit (synced) or reason (n/a)")
    add.add_argument("--date")
    add.set_defaults(func=cmd_add)
    mark = sub.add_parser("mark", help="change the Android state of rows")
    mark.add_argument("ids", nargs="+")
    group = mark.add_mutually_exclusive_group()
    group.add_argument("--synced", metavar="ANDROID_SHA")
    group.add_argument("--na", metavar="REASON")
    group.add_argument("--doing", action="store_true")
    mark.set_defaults(func=cmd_mark)
    triage = sub.add_parser("triage", help="move the triage marker once every iOS commit up to SHA has a row or was skipped")
    triage.add_argument("sha", nargs="?")
    triage.add_argument("--force", action="store_true")
    triage.set_defaults(func=cmd_triage)
    check = sub.add_parser("check", help="validate the ledger")
    check.add_argument("--strict", action="store_true", help="also fail on untriaged iOS commits")
    check.set_defaults(func=cmd_check)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
