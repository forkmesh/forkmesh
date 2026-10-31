#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SOURCE = REPOSITORY / "app" / "public" / "call-quality-meter.js"
PAGES = (
    REPOSITORY / "native" / "ios" / "ForkMesh" / "Features" / "FaceChanger" / "direct-call.html",
    REPOSITORY / "native" / "android" / "app" / "src" / "main" / "assets" / "direct-call.html",
)
OPEN = '<script id="fm-call-quality">'
CLOSE = "</script>"
ANCHOR = "<script>\n"
EXPORTS = (
    "createCallQualityMeter",
    "rtcStatsReader",
    "qualityEvent",
)


def page_script() -> str:
    source = SOURCE.read_text(encoding="utf-8")
    if CLOSE in source:
        raise SystemExit("the quality meter must not contain a closing script tag")
    body = re.sub(r"^export ", "", source, flags=re.MULTILINE).strip("\n")
    names = ", ".join(EXPORTS)
    return f'{OPEN}\n"use strict";\nwindow.FmCallQuality = (() => {{\n{body}\nreturn {{ {names} }};\n}})();\n{CLOSE}'


def render(page: str) -> str:
    block = page_script()
    if OPEN in page:
        start = page.index(OPEN)
        end = page.index(CLOSE, start) + len(CLOSE)
        return page[:start] + block + page[end:]
    anchor = page.rindex(ANCHOR)
    return page[:anchor] + block + "\n" + page[anchor:]


def main(argv: list[str]) -> int:
    check = "--check" in argv
    stale = []
    for path in PAGES:
        current = path.read_text(encoding="utf-8")
        wanted = render(current)
        if current == wanted:
            continue
        stale.append(path)
        if not check:
            path.write_text(wanted, encoding="utf-8")
    for path in stale:
        print(("stale: " if check else "updated: ") + str(path.relative_to(REPOSITORY)))
    return 1 if check and stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
