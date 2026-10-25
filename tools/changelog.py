#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHANGELOG = ROOT / "changelog" / "changelog.json"
SCHEMA = 2
APP_SCHEMA = 1
MAX_ITEMS_PER_SECTION = 8
MAX_ITEMS_PER_ENTRY = 24

AREAS = ("Highlights", "Market", "Calls", "Projects", "Chat", "Agents", "World", "Settings",
         "iPhone", "Android", "Desktop", "Server", "Other improvements")
HIDDEN_ON = {"ios": "Android", "android": "iPhone"}
DATE = re.compile(r"\d{4}-\d{2}(-\d{2})?\Z")
VERSION = re.compile(r"[0-9][0-9A-Za-z.-]{0,31}\Z")
BOLD = re.compile(r"\*\*(.+?)\*\*")
MONTHS = ("January", "February", "March", "April", "May", "June", "July", "August", "September",
          "October", "November", "December")


def load(path: Path = CHANGELOG) -> dict:
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: not a JSON object")
    return value


def save(data: dict, path: Path = CHANGELOG) -> None:
    Path(path).write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


def problems(data: dict) -> list[str]:
    found: list[str] = []
    if data.get("schema") != SCHEMA:
        found.append(f"schema must be {SCHEMA}")
    entries = data.get("entries")
    if not isinstance(entries, list) or not entries:
        return found + ["entries must be a non-empty list"]
    previous = ""
    for index, entry in enumerate(entries):
        where = f"entry {index + 1}"
        if not isinstance(entry, dict):
            found.append(f"{where}: not an object")
            continue
        date = entry.get("date")
        if not isinstance(date, str) or not DATE.match(date):
            found.append(f"{where}: date must be YYYY-MM-DD or YYYY-MM")
        elif previous and date > previous:
            found.append(f"{where}: entries must be newest first")
        else:
            previous = date
        version = entry.get("version", "")
        if version and not (isinstance(version, str) and VERSION.match(version)):
            found.append(f"{where}: bad version {version!r}")
        for key in ("title", "pitch"):
            if not isinstance(entry.get(key, ""), str):
                found.append(f"{where}: {key} must be text")
        sections = entry.get("sections")
        if not isinstance(sections, list) or not sections:
            found.append(f"{where}: sections must be a non-empty list")
            continue
        titles: set[str] = set()
        for section in sections:
            title = section.get("title") if isinstance(section, dict) else None
            if title not in AREAS:
                found.append(f"{where}: section {title!r} is not one of {', '.join(AREAS)}")
                continue
            if title in titles:
                found.append(f"{where}: section {title!r} appears twice")
            titles.add(title)
            items = section.get("items")
            if not isinstance(items, list) or not items or not all(isinstance(i, str) and i.strip() for i in items):
                found.append(f"{where}: {title} needs a list of non-empty items")
    return found


def plain(text: str) -> str:
    return BOLD.sub(r"\1", text).strip()


def norm_key(text: str) -> str:
    return " ".join(re.findall(r"[a-z0-9]+", plain(text).lower()))


def clean_sections(sections: list) -> list[dict]:
    out: list[dict] = []
    seen: set[str] = set()
    total = 0
    for section in sections or []:
        if not isinstance(section, dict):
            continue
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
    entries = []
    for entry in data.get("entries") or []:
        sections = clean_sections([s for s in entry.get("sections") or [] if s.get("title") != hidden])
        if sections:
            entries.append({"version": str(entry.get("version") or ""), "build": "",
                            "date": str(entry.get("date") or ""), "title": str(entry.get("title") or ""),
                            "pitch": str(entry.get("pitch") or ""), "sections": sections})
    return {"schema": APP_SCHEMA, "entries": entries}


def when(date: str) -> str:
    parts = [int(part) for part in date.split("-")]
    month = MONTHS[parts[1] - 1]
    return f"{month} {parts[2]}, {parts[0]}" if len(parts) == 3 else f"{month} {parts[0]}"


def rich(text: str) -> str:
    return BOLD.sub(r"<strong>\1</strong>", html.escape(text, quote=False))


def entry_html(entry: dict) -> str:
    date = entry["date"]
    meta = [f'          <span class="tag">v{html.escape(entry["version"])}</span>'] if entry.get("version") else []
    meta.append(f'          <time class="when" datetime="{date}">{when(date)}</time>')
    body = []
    if entry.get("title"):
        body.append(f"          <h2>{rich(entry['title'])}</h2>")
    if entry.get("pitch"):
        body.append(f'          <p class="pitch">{rich(entry["pitch"])}</p>')
    sections = entry["sections"]
    for section in sections:
        if len(sections) > 1:
            body.append(f"          <h3>{html.escape(section['title'])}</h3>")
        body.append("          <ul>")
        body.extend(f"            <li>{rich(item)}</li>" for item in section["items"])
        body.append("          </ul>")
    anchor = f"v{entry['version']}" if entry.get("version") else date
    return "\n".join([
        f'      <section class="release" id="{html.escape(anchor)}">',
        '        <div class="release-meta">', *meta, "        </div>",
        '        <div class="release-body">', *body, "        </div>",
        "      </section>"])


def releases_html(data: dict) -> str:
    return "\n\n".join(entry_html(entry) for entry in data["entries"])


def add(data: dict, area: str, text: str, date: str) -> dict:
    if area not in AREAS:
        raise SystemExit(f"--area must be one of: {', '.join(AREAS)}")
    text = " ".join(text.split())
    if not text:
        raise SystemExit("the note is empty")
    if not text.endswith((".", "!", "?")):
        text += "."
    entries = data.setdefault("entries", [])
    if not entries or entries[0].get("date") != date:
        entries.insert(0, {"date": date, "version": "", "title": "", "pitch": "", "sections": []})
    sections = entries[0]["sections"]
    section = next((s for s in sections if s.get("title") == area), None)
    if section is None:
        section = {"title": area, "items": []}
        sections.append(section)
        sections.sort(key=lambda s: AREAS.index(s["title"]) if s["title"] in AREAS else len(AREAS))
    if norm_key(text) not in {norm_key(item) for item in section["items"]}:
        section["items"].append(text)
    return data


def draft(repo: Path, since: str, target: str = "HEAD") -> list[dict]:
    import changelog_draft
    return changelog_draft.sections_since(repo, since, target)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="The ForkMesh changelog: forkmesh.com/changelog and the apps' What's new.")
    parser.add_argument("--file", type=Path, default=CHANGELOG)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check", help="validate the changelog")
    adding = commands.add_parser("add", help="add a note to today's entry")
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
        found = problems(data)
        for problem in found:
            print(f"{args.file}: {problem}", file=sys.stderr)
        if not found:
            print(f"{args.file}: {len(data['entries'])} entries")
        return 1 if found else 0
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
    since = args.since or str(data["entries"][0]["date"])
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    for section in draft(args.repo, since, args.commit):
        print(section["title"])
        for item in section["items"]:
            print(f"  {item}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
