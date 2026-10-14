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
    PUBLIC / "chat-call-face-expression.js",
    PUBLIC / "chat-call-face-custom.js",
    PUBLIC / "chat-call-segment.js",
    PUBLIC / "chat-call-face-mesh.js",
    PUBLIC / "chat-call-face-tracker.js",
    PUBLIC / "chat-call-outfits.js",
    PUBLIC / "chat-call-face-render.js",
    PUBLIC / "chat-call-backgrounds.js",
    PUBLIC / "chat-call-faces.js",
)
PHOTOS = PUBLIC / "assets" / "call-faces"
BACKGROUNDS = PUBLIC / "assets" / "call-backgrounds"
MODELS = PUBLIC / "assets" / "call-segmentation"
MODEL_NAMES = {
    "landscape": "selfie-landscape.fmnn",
    "square": "selfie-square.fmnn",
    "detector": "face-detector.fmnn",
    "mesh": "face-mesh.fmnn",
    "hair": "hair-192.fmnn",
}
PAGES = (
    REPOSITORY / "native" / "ios" / "ForkMesh" / "Features" / "Chat" / "Calls" / "direct-call.html",
    REPOSITORY / "native" / "android" / "app" / "src" / "main" / "assets" / "direct-call.html",
)
FACES_OPEN = '<script id="fm-call-faces">'
PHOTOS_OPEN = '<script id="fm-call-face-photos">'
BACKGROUNDS_OPEN = '<script id="fm-call-backgrounds">'
MODELS_OPEN = '<script id="fm-call-models">'
CLOSE = "</script>"
EXPORTS = (
    "CALL_FACES",
    "callFaceById",
    "callFaceChoices",
    "isCustomFace",
    "callFacesSupported",
    "createFaceStage",
    "CALL_BACKGROUNDS",
    "callBackgroundById",
    "callBackgroundsSupported",
)
IMPORT = re.compile(r'^import\s*\{[^}]*\}\s*from\s*"\./[^"]+";\n', re.MULTILINE)
EXPORTED = re.compile(r"^export\s+(?:async\s+)?(?:function\*?|const|let|class)\s+([A-Za-z_$][\w$]*)", re.MULTILINE)
TYPES = {".jpg": "image/jpeg", ".png": "image/png", ".webp": "image/webp"}


def module_body(path: Path) -> str:
    source = path.read_text(encoding="utf-8")
    names = EXPORTED.findall(source)
    source = IMPORT.sub("", source)
    body = re.sub(r"^export ", "", source, flags=re.MULTILINE).strip("\n")
    return f"const {{ {', '.join(names)} }} = (() => {{\n{body}\nreturn {{ {', '.join(names)} }};\n}})();"


def inline_script() -> str:
    body = "\n\n".join(module_body(path) for path in MODULES)
    names = ", ".join(EXPORTS)
    return f'{FACES_OPEN}\n"use strict";\nwindow.FmCallFaces = (() => {{\n{body}\nreturn {{ {names} }};\n}})();\n{CLOSE}'


def data_url(path: Path) -> str:
    data = base64.b64encode(path.read_bytes()).decode("ascii")
    return f"data:{TYPES[path.suffix]};base64,{data}"


def photos_script() -> str:
    entries = []
    for path in sorted(PHOTOS.glob("*.jpg")):
        entries.append(f'  "{path.stem}": "{data_url(path)}",')
        matte = path.with_name(f"{path.stem}-matte.png")
        if matte.exists():
            entries.append(f'  "{matte.stem}": "{data_url(matte)}",')
    lines = "\n".join(entries)
    return f"{PHOTOS_OPEN}\nwindow.FmCallFacePhotos = {{\n{lines}\n}};\n{CLOSE}"


def backgrounds_script() -> str:
    entries = [f'  "{path.stem}": "{data_url(path)}",' for path in sorted(BACKGROUNDS.glob("*.jpg"))]
    lines = "\n".join(entries)
    return f"{BACKGROUNDS_OPEN}\nwindow.FmCallBackgroundImages = {{\n{lines}\n}};\n{CLOSE}"


def models_script() -> str:
    entries = []
    for name, file in MODEL_NAMES.items():
        path = MODELS / file
        if path.exists():
            entries.append(f'  "{name}": "{base64.b64encode(path.read_bytes()).decode("ascii")}",')
    lines = "\n".join(entries)
    return f"{MODELS_OPEN}\nwindow.FmCallSegmentModels = {{\n{lines}\n}};\n{CLOSE}"


def replace_block(page: str, opening: str, block: str) -> str:
    if opening not in page:
        at = page.index(FACES_OPEN)
        return page[:at] + block + "\n" + page[at:]
    start = page.index(opening)
    end = page.index(CLOSE, start) + len(CLOSE)
    return page[:start] + block + page[end:]


def render(page: str) -> str:
    page = replace_block(page, PHOTOS_OPEN, photos_script())
    page = replace_block(page, BACKGROUNDS_OPEN, backgrounds_script())
    page = replace_block(page, MODELS_OPEN, models_script())
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
