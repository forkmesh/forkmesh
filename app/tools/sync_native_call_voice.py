#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
PUBLIC = REPOSITORY / "app" / "public"
VOICE = PUBLIC / "chat-call-voice.js"
WORKLET = PUBLIC / "chat-call-voice-worklet.js"
PAGES = (
    REPOSITORY / "native" / "ios" / "ForkMesh" / "Features" / "Chat" / "Calls" / "direct-call.html",
    REPOSITORY / "native" / "android" / "app" / "src" / "main" / "assets" / "direct-call.html",
)
VOICE_OPEN = '<script id="fm-call-voice">'
WORKLET_OPEN = '<script type="text/plain" id="fm-call-voice-worklet">'
CLOSE = "</script>"
ANCHOR = "<script>\n"
EXPORTS = (
    "CALL_VOICES",
    "callVoiceById",
    "callVoicesSupported",
    "createVoiceStage",
    "normalizeVoiceChoice",
    "rememberCallerPitch",
    "resolveCallVoice",
    "savedCallerPitch",
)


def voice_script() -> str:
    source = VOICE.read_text(encoding="utf-8")
    body = re.sub(r"^export ", "", source, flags=re.MULTILINE).strip("\n")
    names = ", ".join(EXPORTS)
    return f'{VOICE_OPEN}\n"use strict";\nwindow.FmCallVoice = (() => {{\n{body}\nreturn {{ {names} }};\n}})();\n{CLOSE}'


def worklet_script() -> str:
    source = WORKLET.read_text(encoding="utf-8").strip("\n")
    if CLOSE in source:
        raise SystemExit("the voice worklet must not contain a closing script tag")
    return f"{WORKLET_OPEN}\n{source}\n{CLOSE}"


def place(page: str, opening: str, block: str) -> str:
    if opening in page:
        start = page.index(opening)
        end = page.index(CLOSE, start) + len(CLOSE)
        return page[:start] + block + page[end:]
    anchor = page.rindex(ANCHOR)
    return page[:anchor] + block + "\n" + page[anchor:]


def render(page: str) -> str:
    page = place(page, WORKLET_OPEN, worklet_script())
    return place(page, VOICE_OPEN, voice_script())


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
