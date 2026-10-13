#!/usr/bin/env python3

from __future__ import annotations

import base64
import re
import sys
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
PUBLIC = REPOSITORY / "app" / "public"
MODULES = (
    PUBLIC / "chat-call-face-geometry.js",
    PUBLIC / "chat-call-face-tracker.js",
    PUBLIC / "chat-call-face-render.js",
    PUBLIC / "chat-call-faces.js",
)
PHOTOS = PUBLIC / "assets" / "call-faces"
PAGES = (
    REPOSITORY / "native" / "ios" / "ForkMesh" / "Features" / "Chat" / "Calls" / "direct-call.html",
    REPOSITORY / "native" / "android" / "app" / "src" / "main" / "assets" / "direct-call.html",
)
FACES_OPEN = '<script id="fm-call-faces">'
PHOTOS_OPEN = '<script id="fm-call-face-photos">'
CLOSE = "</script>"
EXPORTS = ("CALL_FACES", "callFaceById", "callFacesSupported", "createFaceStage")
IMPORT = re.compile(r'^import\s*\{[^}]*\}\s*from\s*"\./[^"]+";\n', re.MULTILINE)


def module_body(path: Path) -> str:
    source = path.read_text(encoding="utf-8")
    source = IMPORT.sub("", source)
    return re.sub(r"^export ", "", source, flags=re.MULTILINE).strip("\n")


def inline_script() -> str:
    body = "\n\n".join(module_body(path) for path in MODULES)
    names = ", ".join(EXPORTS)
    return f'{FACES_OPEN}\n"use strict";\nwindow.FmCallFaces = (() => {{\n{body}\nreturn {{ {names} }};\n}})();\n{CLOSE}'


def photos_script() -> str:
    entries = []
    for path in sorted(PHOTOS.glob("*.jpg")):
        data = base64.b64encode(path.read_bytes()).decode("ascii")
        entries.append(f'  "{path.stem}": "data:image/jpeg;base64,{data}",')
    lines = "\n".join(entries)
    return f"{PHOTOS_OPEN}\nwindow.FmCallFacePhotos = {{\n{lines}\n}};\n{CLOSE}"


def replace_block(page: str, opening: str, block: str) -> str:
    start = page.index(opening)
    end = page.index(CLOSE, start) + len(CLOSE)
    return page[:start] + block + page[end:]


def render(page: str) -> str:
    page = replace_block(page, PHOTOS_OPEN, photos_script())
    return replace_block(page, FACES_OPEN, inline_script())


def main(argv: list[str]) -> int:
    check = "--check" in argv
    stale = []
    for path in PAGES:
        page = path.read_text(encoding="utf-8")
        updated = render(page)
        if updated == page:
            continue
        stale.append(path)
        if not check:
            path.write_text(updated, encoding="utf-8")
    for path in stale:
        print(("stale: " if check else "updated: ") + str(path.relative_to(REPOSITORY)))
    return 1 if check and stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
