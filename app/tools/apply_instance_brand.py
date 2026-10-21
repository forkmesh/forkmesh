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
BRAND_RE = re.compile(r"(?<![A-Za-z0-9_.-])ForkMesh(?![A-Za-z0-9_-])(?! [Dd]esktop)(?!, Inc)")
DESKTOP_RE = re.compile(r"(?<![A-Za-z0-9_.-])ForkMesh (?=[Dd]esktop)")
CODE_BLOCK_RE = re.compile(r"(<(script|style)\b[^>]*>.*?</\2\s*>)", re.IGNORECASE | re.DOTALL)


def rename_markup(text: str, escaped: str) -> str:
    parts = CODE_BLOCK_RE.split(text)
    out = []
    index = 0
    while index < len(parts):
        chunk = parts[index]
        if index % 3 == 0:
            chunk = BRAND_RE.sub(lambda _: escaped, DESKTOP_RE.sub("", chunk))
            out.append(chunk)
            index += 1
        else:
            out.append(chunk)
            index += 2
    return "".join(out)
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


HEADER_RE = re.compile(r"<header\b[^>]*>.*?</header\s*>", re.IGNORECASE | re.DOTALL)
ANCHOR_RE = re.compile(r"<a\b([^>]*)>(.*?)</a\s*>", re.IGNORECASE | re.DOTALL)
LABEL_RE = re.compile(r"<span data-instance-name-label[^>]*>.*?</span>", re.IGNORECASE | re.DOTALL)
LABEL = '<span data-instance-name-label style="margin-left:8px;font-weight:700;white-space:nowrap">%s</span>'


def _attribute(attributes: str, name: str) -> str | None:
    found = re.search(r'(?<![\w-])%s\s*=\s*"([^"]*)"' % name, attributes, re.IGNORECASE)
    return found.group(1) if found else None


def _logo_anchor(attributes: str) -> bool:
    classes = (_attribute(attributes, "class") or "").split()
    if "brand" in classes or "forkmesh-simple-brand" in classes:
        return True
    return _attribute(attributes, "href") == "/" and _attribute(attributes, "aria-label") is not None


def name_beside_logo(text: str, escaped: str) -> str:
    header = HEADER_RE.search(text)
    if not header:
        return text
    block = header.group(0)
    if LABEL_RE.search(block):
        branded = LABEL_RE.sub(lambda _: LABEL % escaped, block, count=1)
        return text[:header.start()] + branded + text[header.end():]
    for anchor in ANCHOR_RE.finditer(block):
        attributes = anchor.group(1)
        if not _logo_anchor(attributes):
            continue
        if _attribute(attributes, "style") is None:
            attributes += ' style="display:inline-flex;align-items:center"'
        branded = "<a%s>%s%s</a>" % (attributes, anchor.group(2), LABEL % escaped)
        block = block[:anchor.start()] + branded + block[anchor.end():]
        return text[:header.start()] + block + text[header.end():]
    return text


# GET /api/instance is one public document that the dashboard, instance-brand.js
# and genie-spider.js all read. A dashboard page asks for it from its first
# script, in parallel with the bundle download, instead of after the ~2 MB
# bundle has run (bootDashboard waits for the repository addresses in it on a
# first visit). The readers take this promise over (window.__forkmeshInstanceInfo).
INFO_EARLY = (
    '<script data-instance-info-early>try{window.__forkmeshInstanceInfo='
    'fetch("/api/instance",{credentials:"omit",headers:{accept:"application/json"}})'
    '.then(function(r){return r.ok?r.json():null}).catch(function(){return null})'
    '}catch(_){}</script>')
INFO_EARLY_RE = re.compile(r"<script data-instance-info-early>.*?</script>", re.DOTALL)
DASHBOARD_SCRIPT_RE = re.compile(r'<script\b[^>]*\bsrc="/dashboard\.js[?"]', re.IGNORECASE)
SCRIPT_OPEN_RE = re.compile(r"<script\b", re.IGNORECASE)


def instance_info_early(text: str) -> str:
    text = INFO_EARLY_RE.sub("", text)
    if not DASHBOARD_SCRIPT_RE.search(text):
        return text
    head_end = text.lower().find("</head>")
    if head_end == -1:
        return text
    first = SCRIPT_OPEN_RE.search(text, 0, head_end)
    at = first.start() if first else head_end
    return text[:at] + INFO_EARLY + text[at:]


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
    text = rename_markup(text, escaped)
    text = name_beside_logo(text, escaped)
    off = ' data-instance-powered-by="off"' if not powered_by else ""
    tag = THEME_TAG + '<script src="/instance-brand.js" defer data-instance-name="%s"%s %s></script>' % (escaped, off, MARKER)
    text = instance_info_early(text)
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


def name_manifest(dist: Path, name: str) -> None:
    manifest = dist / "favicon" / "site.webmanifest"
    if not manifest.is_file():
        return
    try:
        app = json.loads(manifest.read_text(encoding="utf-8"))
    except ValueError:
        return
    if not isinstance(app, dict) or (app.get("name") == name and app.get("short_name") == name):
        return
    app.update({"name": name, "short_name": name})
    manifest.write_text(json.dumps(app, separators=(",", ":")), encoding="utf-8")


def apply(dist: Path, name: str, logo: Path | None = None, powered_by: bool = True) -> int:
    if not NAME_RE.fullmatch(name):
        raise SystemExit("instance name: letters, numbers, spaces and ._&+- only, up to 40")
    if not dist.is_dir():
        raise SystemExit("no such directory: %s" % dist)
    shutil.copyfile(SCRIPT, dist / "instance-brand.js")
    logo_path = install_logo(dist, name, logo) if logo else ""
    name_manifest(dist, name)
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
