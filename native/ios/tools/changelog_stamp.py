#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

SCHEMA = 1
MAX_ITEMS_PER_SECTION = 8
MAX_ITEMS_PER_ENTRY = 24


def norm_key(text: str) -> str:
    return " ".join(re.findall(r"[a-z0-9]+", text.lower()))


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
            text = str(item).strip()
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


def merge_notes(sections: list, notes: dict) -> list[dict]:
    noted = [dict(section, items=list(section.get("items") or []))
             for section in notes.get("sections") or [] if isinstance(section, dict)]
    if notes.get("replace"):
        return clean_sections(noted)
    by_title = {str(section.get("title")): section for section in noted}
    merged: list[dict] = list(noted)
    for section in sections or []:
        title = str(section.get("title"))
        if title in by_title:
            by_title[title]["items"] += list(section.get("items") or [])
        else:
            merged.append(section)
    return clean_sections(merged)


def build_number(value) -> int | None:
    text = str(value or "").strip()
    return int(text) if text.isdigit() else None


def notes_between(notes: dict, after: int | None, upto: int | None) -> list[dict]:
    if upto is None:
        return []
    keys = sorted(k for k in (build_number(key) for key in (notes or {})) if k is not None)
    return [notes[str(k)] for k in keys if k <= upto and (after is None or k > after)]


def apply_notes(sections: list, notes: list[dict]) -> list[dict]:
    result = clean_sections(sections)
    for note in notes:
        result = merge_notes(result, note)
    return result


def stamp(draft: dict | None, version: str, build: str, date: str = "") -> dict:
    draft = draft if isinstance(draft, dict) else {}
    entries = [entry for entry in draft.get("entries") or [] if isinstance(entry, dict)]
    current = next((entry for entry in entries if entry.get("pending")), None)
    rest = [entry for entry in entries if not entry.get("pending")]
    if current is None:
        current = {"sections": [], "date": date}
    after = build_number(current.get("after"))
    sections = apply_notes(current.get("sections") or [],
                           notes_between(draft.get("notes") or {}, after, build_number(build)))
    head = {"version": version, "build": str(build), "date": current.get("date") or date,
            "sections": sections}
    older = [entry for entry in rest if str(entry.get("build")) != str(build)]
    return {"schema": SCHEMA, "entries": [head, *[
        {"version": str(entry.get("version") or ""), "build": str(entry.get("build") or ""),
         "date": str(entry.get("date") or ""), "sections": clean_sections(entry.get("sections"))}
        for entry in older]]}


def read_draft(path: str) -> dict | None:
    if not path:
        return None
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Stamp a changelog draft with this build's number.")
    parser.add_argument("--draft", default="")
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--date", default="")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    draft = read_draft(args.draft)
    if draft is None:
        print(f"No changelog draft at {args.draft or '(none)'}: the app shows this build with no notes.")
    result = stamp(draft, args.version, args.build, args.date)
    Path(args.out).write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
    head = result["entries"][0]
    count = sum(len(section["items"]) for section in head["sections"])
    print(f"Changelog for {args.version} ({args.build}): {count} notes in "
          f"{len(head['sections'])} sections, {len(result['entries']) - 1} earlier builds.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
