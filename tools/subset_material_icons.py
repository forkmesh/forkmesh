#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FULL_TABLE = ROOT / "tools" / "material_icons" / "MaterialIcons.codepoints"
SHIPPED_TABLE = ROOT / "native" / "android" / "app" / "src" / "main" / "assets" / "fonts" / "MaterialIcons.codepoints"
IOS_TABLE = ROOT / "native" / "ios" / "ForkMesh" / "Theme" / "MaterialIconCodepoints.swift"
FONTS = (
    ROOT / "native" / "android" / "app" / "src" / "main" / "res" / "font" / "material_icons_regular.otf",
    ROOT / "native" / "ios" / "ForkMesh" / "Resources" / "Fonts" / "material_icons_regular.otf",
)
FULL_FONT_REVISION = "ae985af229"
FULL_FONT_PATH = "native/android/app/src/main/res/font/material_icons_regular.otf"
ALWAYS = ("help_outline",)
TOKEN = re.compile(r"\b[a-z0-9][a-z0-9_]*\b")
QUOTED = re.compile(r"[\"'`]([a-z0-9][a-z0-9_]*)[\"'`]")
SKIP_SUFFIXES = (".otf", ".ttf", ".woff", ".woff2", ".png", ".jpg", ".jpeg", ".webp", ".gif", ".ico", ".pdf", ".zip")


def read_table(path: Path) -> dict[str, int]:
    table = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2:
            table[parts[0]] = int(parts[1], 16)
    return table


def resolve(table: dict[str, int], name: str) -> str | None:
    if name in table:
        return name
    if name + "_baseline" in table:
        return name + "_baseline"
    return None


def tracked_files(*paths: str) -> list[Path]:
    output = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z", "--", *paths],
                            capture_output=True, text=True, check=True).stdout
    skipped = {FULL_TABLE.resolve(), SHIPPED_TABLE.resolve()}
    files = []
    for relative in output.split("\0"):
        if not relative or relative.lower().endswith(SKIP_SUFFIXES):
            continue
        path = ROOT / relative
        if path.resolve() in skipped or not path.is_file():
            continue
        files.append(path)
    return files


def referenced_names(table: dict[str, int], files: list[Path], pattern: re.Pattern) -> set[str]:
    names = set()
    for path in files:
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for token in pattern.findall(text):
            name = resolve(table, token)
            if name:
                names.add(name)
    return names


def native_literal_names(table: dict[str, int]) -> set[str]:
    files = [path for path in tracked_files("native") if path.suffix in (".kt", ".swift")]
    return referenced_names(table, files, QUOTED)


def ios_codepoints() -> set[int]:
    return {int(code, 16) for code in re.findall(r'^\s+"[^"]+": 0x([0-9a-fA-F]+),', IOS_TABLE.read_text(encoding="utf-8"), re.M)}


def selected_names(table: dict[str, int]) -> set[str]:
    names = referenced_names(table, tracked_files("."), TOKEN)
    names |= {resolve(table, name) for name in ALWAYS} - {None}
    codes = ios_codepoints()
    names |= {name for name, code in table.items() if code in codes}
    return names


def full_font_bytes(font: str | None) -> bytes:
    if font:
        return Path(font).read_bytes()
    return subprocess.run(["git", "-C", str(ROOT), "show", f"{FULL_FONT_REVISION}:{FULL_FONT_PATH}"],
                          capture_output=True, check=True).stdout


def write(font: str | None) -> int:
    from fontTools import subset
    from fontTools.ttLib import TTFont

    table = read_table(FULL_TABLE)
    names = selected_names(table)
    lines = [line for line in FULL_TABLE.read_text(encoding="utf-8").splitlines()
             if line.split()[:1] and line.split()[0] in names]
    SHIPPED_TABLE.write_text("\n".join(lines) + "\n", encoding="utf-8")
    options = subset.Options()
    options.layout_features = []
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.glyph_names = False
    source = TTFont(io.BytesIO(full_font_bytes(font)))
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=sorted({table[name] for name in names}))
    subsetter.subset(source)
    buffer = io.BytesIO()
    source.save(buffer)
    for path in FONTS:
        path.write_bytes(buffer.getvalue())
    print(f"{len(names)} icons, {len(buffer.getvalue())} bytes")
    return 0


def check() -> int:
    full = read_table(FULL_TABLE)
    shipped = read_table(SHIPPED_TABLE)
    missing = sorted(native_literal_names(full) - set(shipped))
    for name in missing:
        print(f"missing from the subset: {name}")
    return 1 if missing else 0


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Subset the native apps' Material Icons font to the icon names the repository uses.")
    parser.add_argument("--check", action="store_true", help="report native icon names the subset lacks")
    parser.add_argument("--font", help="full MaterialIcons-Regular.otf (default: the copy in git history)")
    parsed = parser.parse_args(arguments)
    return check() if parsed.check else write(parsed.font)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
