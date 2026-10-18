#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import html
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
SCRIPT = APP / "public" / "instance-brand.js"
MARKER = "data-instance-brand"
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._&+-]{0,39}$")
TITLE_RE = re.compile(r"(<title[^>]*>)(.*?)(</title>)", re.IGNORECASE | re.DOTALL)
META_RE = re.compile(
    r'(<meta\s+(?:name|property)="(?:application-name|apple-mobile-web-app-title|og:site_name)"\s+content=")'
    r'ForkMesh(")', re.IGNORECASE)
META_TEXT_RE = re.compile(
    r'(<meta\s+(?:name|property)="(?:description|og:title|og:description|twitter:title|twitter:description)"'
    r'\s+content=")([^"]*)(")', re.IGNORECASE)
BRAND_RE = re.compile(r"(?<![A-Za-z0-9_.-])ForkMesh(?![A-Za-z0-9_-])(?! [Dd]esktop)(?!, Inc)")
OLD_TAG_RE = re.compile(r'<script src="/instance-brand\.js"[^>]*' + MARKER + r'[^>]*></script>')
THEME_TAG = '<link rel="stylesheet" href="/instance-theme.css" data-instance-theme />'
ICON_RE = re.compile(r'[ \t]*<link\s+rel="(?:icon|shortcut icon|apple-touch-icon)"[^>]*>[ \t]*\n?', re.IGNORECASE)
LOGO_TYPES = {"png": "image/png", "jpg": "image/jpeg", "webp": "image/webp", "svg": "image/svg+xml"}
MAX_LOGO_BYTES = 200 * 1024
LOGO_UNSAFE_RE = re.compile(
    rb"<script|<foreignobject|<iframe|<embed|<object|\son[a-z]+\s*=|javascript:|<!entity", re.IGNORECASE)
LOGO_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">'
    '<image href="data:%s;base64,%s" width="256" height="256" preserveAspectRatio="xMidYMid meet"/></svg>\n')
LOGO_SVG_TARGETS = ("assets/logo.svg", "favicon/favicon.svg")
RASTER = "instance-logo.png"
RASTER_SIZE = 512


def logo_kind(data: bytes) -> str:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "png"
    if data.startswith(b"\xff\xd8\xff"):
        return "jpg"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    if re.search(rb"<svg[\s>]", data[:4096]) and not LOGO_UNSAFE_RE.search(data):
        return "svg"
    return ""


def logo_svg(data: bytes, kind: str) -> bytes:
    if kind == "svg":
        return data
    return (LOGO_SVG % (LOGO_TYPES[kind], base64.b64encode(data).decode("ascii"))).encode("ascii")


def logo_raster(dist: Path, data: bytes) -> str:
    tool = shutil.which("rsvg-convert")
    if not tool:
        return ""
    size = str(RASTER_SIZE)
    try:
        png = subprocess.run([tool, "--width", size, "--height", size, "--keep-aspect-ratio"],
                             input=data, capture_output=True, check=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return ""
    if logo_kind(png) != "png":
        return ""
    (dist / RASTER).write_bytes(png)
    return RASTER


def icon_links(logo: str) -> str:
    kind = LOGO_TYPES[logo.rsplit(".", 1)[1]]
    return ('<link rel="icon" href="%s" type="%s" data-instance-logo />'
            '<link rel="apple-touch-icon" href="%s" data-instance-logo />') % (logo, kind, logo)


def brand_page(text: str, name: str, logo: str = "", powered_by=True) -> str:
    escaped = html.escape(name, quote=True)
    text = OLD_TAG_RE.sub("", text)
    text = text.replace(THEME_TAG, "")
    if logo:
        found = ICON_RE.search(text)
        if found:
            text = ICON_RE.sub("", text)
            text = text[:found.start()] + icon_links(logo) + "\n" + text[found.start():]
    text = TITLE_RE.sub(
        lambda match: match.group(1) + match.group(2).replace("ForkMesh", escaped) + match.group(3),
        text, count=1)
    text = META_RE.sub(lambda match: match.group(1) + escaped + match.group(2), text)
    text = META_TEXT_RE.sub(
        lambda match: match.group(1) + BRAND_RE.sub(lambda _: escaped, match.group(2)) + match.group(3), text)
    off = ' data-instance-powered-by="off"' if not powered_by else ""
    tag = THEME_TAG + '<script src="/instance-brand.js" defer data-instance-name="%s"%s %s></script>' % (escaped, off, MARKER)
    lower = text.lower()
    index = lower.rfind("</head>")
    if index == -1:
        index = lower.rfind("</body>")
    if index == -1:
        return text + tag
    return text[:index] + tag + text[index:]


def install_logo(dist: Path, name: str, source: Path) -> str:
    data = source.read_bytes()
    kind = logo_kind(data)
    if not kind or len(data) > MAX_LOGO_BYTES:
        raise SystemExit("logo: a PNG, JPEG, WebP or SVG image of up to 200 KB, without scripts")
    for old in dist.glob("instance-logo.*"):
        old.unlink()
    target = "instance-logo." + kind
    (dist / target).write_bytes(data)
    icons = [{"src": "/" + target, "sizes": "any", "type": LOGO_TYPES[kind]}]
    raster = logo_raster(dist, data) if kind == "svg" else ""
    if raster:
        icons.insert(0, {"src": "/" + raster, "sizes": "%dx%d" % (RASTER_SIZE, RASTER_SIZE), "type": "image/png"})
    for relative in LOGO_SVG_TARGETS:
        path = dist / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(logo_svg(data, kind))
    manifest = dist / "favicon" / "site.webmanifest"
    if manifest.is_file():
        try:
            app = json.loads(manifest.read_text(encoding="utf-8"))
        except ValueError:
            app = {}
        app.update({"name": name, "short_name": name, "icons": icons})
        manifest.write_text(json.dumps(app, separators=(",", ":")), encoding="utf-8")
    return "/" + target


def apply(dist: Path, name: str, logo: Path | None = None, powered_by: bool = True) -> int:
    if not NAME_RE.fullmatch(name):
        raise SystemExit("instance name: letters, numbers, spaces and ._&+- only, up to 40")
    if not dist.is_dir():
        raise SystemExit("no such directory: %s" % dist)
    shutil.copyfile(SCRIPT, dist / "instance-brand.js")
    logo_path = install_logo(dist, name, logo) if logo else ""
    changed = 0
    for page in sorted(dist.rglob("*.html")):
        original = page.read_text(encoding="utf-8", errors="surrogateescape")
        branded = brand_page(original, name, logo_path, powered_by)
        if branded != original:
            page.write_text(branded, encoding="utf-8", errors="surrogateescape")
            changed += 1
    return changed


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Rename a built ForkMesh app for a white-label instance.")
    parser.add_argument("--name", required=True, help="the instance's project name")
    parser.add_argument("--dist", default=str(APP / "dist"), help="the built app directory")
    parser.add_argument("--logo", help="the instance's logo: a PNG, JPEG, WebP or SVG file")
    parser.add_argument("--no-powered-by", action="store_true", help="leave out the Powered by ForkMesh badge")
    args = parser.parse_args(argv)
    changed = apply(Path(args.dist), " ".join(args.name.split()), Path(args.logo) if args.logo else None,
                    powered_by=not args.no_powered_by)
    print("branded %d pages as %s" % (changed, args.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
