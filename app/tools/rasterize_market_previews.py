#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGES = ROOT / "marketplace" / "packages"
SIZE_RE = re.compile(r"<svg\b[^>]*?\bwidth=\"(\d+)\"[^>]*?\bheight=\"(\d+)\"", re.S)
BROWSER_NAMES = ("chromium", "chromium-browser", "google-chrome", "chrome-headless-shell")


def find_browser():
    explicit = os.environ.get("FORKMESH_CHROME", "")
    if explicit:
        return explicit
    for name in BROWSER_NAMES:
        found = shutil.which(name)
        if found:
            return found
    cache = Path.home() / ".cache" / "ms-playwright"
    shells = sorted(cache.glob("chromium_headless_shell-*/*/chrome-headless-shell"))
    return str(shells[-1]) if shells else ""


def svg_size(svg):
    match = SIZE_RE.search(svg.read_text(encoding="utf-8")[:4096])
    if not match:
        raise SystemExit(f"{svg}: the <svg> element needs integer width and height")
    return int(match.group(1)), int(match.group(2))


def rasterize(browser, svg, webp):
    from PIL import Image
    width, height = svg_size(svg)
    with tempfile.TemporaryDirectory(prefix="fm-market-preview-") as profile:
        png = Path(profile) / "shot.png"
        result = subprocess.run(
            [browser, "--headless", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
             "--user-data-dir=" + str(Path(profile) / "profile"), f"--window-size={width},{height}",
             "--screenshot=" + str(png), svg.resolve().as_uri()],
            capture_output=True, text=True, timeout=120, check=False)
        if result.returncode or not png.is_file():
            raise SystemExit(f"{svg}: rasterize failed\n{result.stderr[-2000:]}")
        with Image.open(png) as image:
            image.convert("RGB").save(webp, "WEBP", quality=85, method=6)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Write a WebP next to every Market SVG preview so phones can show it.")
    parser.add_argument("--force", action="store_true", help="re-render WebPs that already exist")
    args = parser.parse_args(argv)
    browser = find_browser()
    if not browser:
        raise SystemExit("No headless Chromium found; set FORKMESH_CHROME")
    for svg in sorted(PACKAGES.glob("*/preview/*.svg")):
        webp = svg.with_suffix(".webp")
        if webp.exists() and not args.force:
            continue
        rasterize(browser, svg, webp)
        print(webp.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
