#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHANGELOG = ROOT / "changelog" / "changelog.json"
SCHEMA = 3
APP_SCHEMA = 1
MAX_ITEMS_PER_SECTION = 8
MAX_ITEMS_PER_ENTRY = 24
RELEASE_KEYS = ("date", "version", "title", "pitch")
NOTE_KEYS = ("date", "version", "area", "text")

AREAS = ("Highlights", "Market", "Calls", "Projects", "Chat", "Agents", "World", "Settings",
         "iPhone", "Android", "Desktop", "Server", "Other improvements")
HIDDEN_ON = {"ios": "Android", "android": "iPhone"}
DATE = re.compile(r"\d{4}-\d{2}(-\d{2})?\Z")
VERSION = re.compile(r"[0-9][0-9A-Za-z.-]{0,31}\Z")
BOLD = re.compile(r"\*\*(.+?)\*\*")


def load(path: Path = CHANGELOG) -> dict:
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: not a JSON object")
    return value


def line(value: dict, keys: tuple[str, ...]) -> str:
    known = {key: value[key] for key in keys if value.get(key)}
    extra = {key: item for key, item in value.items() if key not in keys}
    return json.dumps({**known, **extra}, ensure_ascii=False)


def block(name: str, rows: list, keys: tuple[str, ...]) -> list[str]:
    if not rows:
        return [f' "{name}": []']
    lines = [f"  {line(row, keys) if isinstance(row, dict) else json.dumps(row)}," for row in rows]
    lines[-1] = lines[-1][:-1]
    return [f' "{name}": [', *lines, " ]"]


def dumps(data: dict) -> str:
    extra = {key: value for key, value in data.items() if key not in ("schema", "releases", "notes")}
    lines = ["{", f' "schema": {json.dumps(data.get("schema", SCHEMA))},']
    lines.extend(f" {json.dumps(key)}: {json.dumps(value, ensure_ascii=False)}," for key, value in extra.items())
    lines.extend(block("releases", data.get("releases") or [], RELEASE_KEYS))
    lines[-1] += ","
    lines.extend(block("notes", data.get("notes") or [], NOTE_KEYS))
    lines.append("}")
    return "\n".join(lines) + "\n"


def save(data: dict, path: Path = CHANGELOG) -> None:
    Path(path).write_text(dumps(data), encoding="utf-8")


def plain(text: str) -> str:
    return BOLD.sub(r"\1", text).strip()


def norm_key(text: str) -> str:
    return " ".join(re.findall(r"[a-z0-9]+", plain(text).lower()))


def release_of(value: dict) -> tuple[str, str]:
    return str(value.get("date") or ""), str(value.get("version") or "")


def newest_first(key: tuple[str, str]) -> tuple:
    return key[0], tuple(int(part) if part.isdigit() else 0 for part in re.split(r"[.-]", key[1]) if part)


def problems(data: dict) -> list[str]:
    found: list[str] = []
    if data.get("schema") != SCHEMA:
        found.append(f"schema must be {SCHEMA}")
    notes = data.get("notes")
    if not isinstance(notes, list) or not notes:
        return found + ["notes must be a non-empty list"]
    days: set[tuple[str, str]] = set()
    seen: set[tuple[str, str, str]] = set()
    for index, note in enumerate(notes):
        where = f"note {index + 1}"
        if not isinstance(note, dict):
            found.append(f"{where}: not an object")
            continue
        date, area, text = note.get("date"), note.get("area"), note.get("text")
        if not isinstance(date, str) or not DATE.match(date):
            found.append(f"{where}: date must be YYYY-MM-DD or YYYY-MM")
            continue
        version = note.get("version", "")
        if version and not (isinstance(version, str) and VERSION.match(version)):
            found.append(f"{where}: bad version {version!r}")
        days.add(release_of(note))
        if area not in AREAS:
            found.append(f"{where}: area {area!r} is not one of {', '.join(AREAS)}")
        if not isinstance(text, str) or not text.strip():
            found.append(f"{where}: text must be a non-empty sentence")
            continue
        key = (*release_of(note), norm_key(text))
        if key in seen:
            found.append(f"{where}: the same note appears twice on {date}")
        seen.add(key)
    releases = data.get("releases", [])
    if not isinstance(releases, list):
        return found + ["releases must be a list"]
    named: set[tuple[str, str]] = set()
    for index, release in enumerate(releases):
        where = f"release {index + 1}"
        if not isinstance(release, dict):
            found.append(f"{where}: not an object")
            continue
        key = release_of(release)
        version = release.get("version", "")
        if version and not (isinstance(version, str) and VERSION.match(version)):
            found.append(f"{where}: bad version {version!r}")
        elif key not in days:
            found.append(f"{where}: no notes have date {key[0]!r}" + (f" and version {key[1]!r}" if key[1] else ""))
        elif key in named:
            found.append(f"{where}: {' '.join(filter(None, key))} has two releases")
        named.add(key)
        for key in ("title", "pitch"):
            if not isinstance(release.get(key, ""), str):
                found.append(f"{where}: {key} must be text")
    return found


def layout_problems(text: str, data: dict) -> list[str]:
    return [] if text == dumps(data) else [
        "not in the one-line-per-note layout; run python3 tools/changelog.py format"]


def entries(data: dict) -> list[dict]:
    releases = {release_of(r): r for r in data.get("releases") or [] if isinstance(r, dict)}
    days: dict[tuple[str, str], dict[str, list[str]]] = {}
    seen: set[tuple[str, str, str]] = set()
    for note in data.get("notes") or []:
        key, area, text = release_of(note), note.get("area"), " ".join(str(note.get("text") or "").split())
        if not key[0] or area not in AREAS or not text or (*key, norm_key(text)) in seen:
            continue
        seen.add((*key, norm_key(text)))
        days.setdefault(key, {}).setdefault(area, []).append(text)
    out = []
    for key in sorted(days, key=newest_first, reverse=True):
        release = releases.get(key) or {}
        out.append({"date": key[0], "version": key[1],
                    "title": str(release.get("title") or ""), "pitch": str(release.get("pitch") or ""),
                    "sections": [{"title": area, "items": days[key][area]} for area in AREAS if area in days[key]]})
    return out


def clean_sections(sections: list) -> list[dict]:
    out: list[dict] = []
    seen: set[str] = set()
    total = 0
    for section in sections or []:
        title = str(section.get("title") or "").strip()
        items: list[str] = []
        for item in section.get("items") or []:
            text = plain(str(item))
            key = norm_key(text)
            if not text or not key or key in seen:
                continue
            if len(items) >= MAX_ITEMS_PER_SECTION or total >= MAX_ITEMS_PER_ENTRY:
                break
            seen.add(key)
            items.append(text)
            total += 1
        if title and items:
            out.append({"title": title, "items": items})
    return out


def export(data: dict, platform: str = "") -> dict:
    hidden = HIDDEN_ON.get(platform)
    out = []
    for entry in entries(data):
        sections = clean_sections([s for s in entry["sections"] if s["title"] != hidden])
        if sections:
            out.append({"version": entry["version"], "build": "", "date": entry["date"], "title": entry["title"],
                        "pitch": entry["pitch"], "sections": sections})
    return {"schema": APP_SCHEMA, "entries": out}


def add(data: dict, area: str, text: str, date: str) -> dict:
    if area not in AREAS:
        raise SystemExit(f"--area must be one of: {', '.join(AREAS)}")
    if not DATE.match(date):
        raise SystemExit("--date must be YYYY-MM-DD or YYYY-MM")
    text = " ".join(text.split())
    if not text:
        raise SystemExit("the note is empty")
    if not text.endswith((".", "!", "?")):
        text += "."
    notes = data.setdefault("notes", [])
    if any(release_of(n) == (date, "") and norm_key(str(n.get("text") or "")) == norm_key(text) for n in notes):
        return data
    notes.insert(0, {"date": date, "area": area, "text": text})
    return data


def draft(repo: Path, since: str, target: str = "HEAD") -> list[dict]:
    import changelog_draft
    return changelog_draft.sections_since(repo, since, target)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="The ForkMesh changelog: forkmesh.com/changelog and the apps' What's new.")
    parser.add_argument("--file", type=Path, default=CHANGELOG)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check", help="validate the changelog and its one-line-per-note layout")
    commands.add_parser("format", help="rewrite the changelog in the one-line-per-note layout")
    adding = commands.add_parser("add", help="add a note dated today")
    adding.add_argument("--area", required=True, choices=AREAS)
    adding.add_argument("--date", default="")
    adding.add_argument("text", nargs="+")
    exporting = commands.add_parser("export", help="write the changelog the phone apps bundle")
    exporting.add_argument("--platform", choices=("ios", "android"), default="")
    exporting.add_argument("--out", type=Path, required=True)
    drafting = commands.add_parser("draft", help="suggest notes from git history since a date or commit")
    drafting.add_argument("--repo", type=Path, default=ROOT)
    drafting.add_argument("--since", default="")
    drafting.add_argument("--commit", default="HEAD")
    args = parser.parse_args(argv)
    data = load(args.file)
    if args.command == "check":
        found = problems(data) + layout_problems(args.file.read_text(encoding="utf-8"), data)
        for problem in found:
            print(f"{args.file}: {problem}", file=sys.stderr)
        if not found:
            print(f"{args.file}: {len(data['notes'])} notes in {len(entries(data))} entries")
        return 1 if found else 0
    if args.command == "format":
        save(data, args.file)
        print(f"{args.file}: {len(data.get('notes') or [])} notes")
        return 0
    if args.command == "add":
        date = args.date or dt.date.today().isoformat()
        save(add(data, args.area, " ".join(args.text), date), args.file)
        found = problems(data)
        for problem in found:
            print(f"{args.file}: {problem}", file=sys.stderr)
        print(f"{args.file}: added to {args.area} on {date}")
        return 1 if found else 0
    if args.command == "export":
        result = export(data, args.platform)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"{args.out}: {len(result['entries'])} entries")
        return 0
    since = args.since or max(str(note.get("date") or "") for note in data.get("notes") or [{}])
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    for section in draft(args.repo, since, args.commit):
        print(section["title"])
        for item in section["items"]:
            print(f"  {item}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
