#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import json
import math
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import cairo
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MASTER = ROOT / "logo.svg"
NS = "{http://www.w3.org/2000/svg}"

SVG_COPIES = [
    ROOT / "www/public/assets/logo.svg",
    ROOT / "www/public/favicon/favicon.svg",
    ROOT / "office/public/logo.svg",
    ROOT / "desktop/resources/icons/forkmesh.svg",
    ROOT / "native/ios/ForkMesh/Resources/Assets.xcassets/Logo.imageset/logo.svg",
]

BIMI = ROOT / "www/public/assets/forkmesh-bimi.svg"
BIMI_SIZE = 1350
BIMI_SCALE = 5.0
BIMI_BG = "#090909"

TILE_TOP = (0x30, 0x34, 0x3D)
TILE_BOTTOM = (0x0B, 0x0C, 0x10)
TILE_GLOW = (0x4A, 0x50, 0x5C)
TILE_FIT = 0.78

IOS_PHONE = ROOT / "native/ios/ForkMesh/Resources/Assets.xcassets/AppIcon.appiconset"
IOS_WATCH = ROOT / "native/ios/ForkMeshWatch/Assets.xcassets/AppIcon.appiconset"
ANDROID_RES = ROOT / "native/android/app/src/main/res"
ANDROID_LAUNCHER = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
ANDROID_ADAPTIVE = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}
ADAPTIVE_VISIBLE = 72 / 108
ADAPTIVE_FIT = 0.86

CUTOUTS = [
    ("logo.png", "cutout", 975, 1116),
    ("www/public/assets/logo.png", "cutout", 895, 1024),
    ("www/public/assets/logo-96.png", "cutout", 84, 96),
    ("www/public/assets/logo-mark.webp", "cutout", 184, 211),
    ("www/public/favicon/favicon-16x16.png", "square", 16, 16),
    ("www/public/favicon/favicon-32x32.png", "square", 32, 32),
    ("www/public/favicon/android-chrome-192x192.png", "square", 192, 192),
    ("www/public/favicon/android-chrome-512x512.png", "square", 512, 512),
    ("www/public/favicon/apple-touch-icon.png", "tile", 180, 180),
    ("www/public/favicon/favicon.ico", "ico", 48, 48),
    ("www/public/assets/fediverse-avatar.png", "padded", 400, 400),
    ("desktop/resources/icons/forkmesh.png", "square", 1024, 1024),
    ("desktop/resources/icons/forkmesh-256.png", "square", 256, 256),
    ("native/android/app/src/main/res/drawable-nodpi/fm_logo.png", "padded", 1024, 1024),
    ("marketplace/packages/browser-extension/content/extension/icons/icon16.png", "square", 16, 16),
    ("marketplace/packages/browser-extension/content/extension/icons/icon32.png", "square", 32, 32),
    ("marketplace/packages/browser-extension/content/extension/icons/icon48.png", "square", 48, 48),
    ("marketplace/packages/browser-extension/content/extension/icons/icon128.png", "square", 128, 128),
]


def parse_color(value: str) -> tuple[float, float, float]:
    value = value.lstrip("#")
    if len(value) == 3:
        value = "".join(c * 2 for c in value)
    return tuple(int(value[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def parse_matrix(value: str | None) -> cairo.Matrix:
    if not value:
        return cairo.Matrix()
    match = re.fullmatch(r"\s*matrix\(([^)]*)\)\s*", value)
    if not match:
        raise ValueError("unsupported transform: " + value)
    a, b, c, d, e, f = (float(n) for n in re.split(r"[\s,]+", match.group(1).strip()))
    return cairo.Matrix(a, b, c, d, e, f)


def path_commands(data: str) -> list[tuple[str, list[float]]]:
    out = []
    for cmd, args in re.findall(r"([MLHVZmlhvz])([^MLHVZmlhvz]*)", data):
        if cmd.islower():
            raise ValueError("relative path commands are not supported: " + cmd)
        out.append((cmd, [float(n) for n in re.findall(r"-?\d*\.?\d+(?:e-?\d+)?", args)]))
    return out


def trace_path(ctx: cairo.Context, data: str) -> None:
    x = y = 0.0
    for cmd, nums in path_commands(data):
        if cmd == "Z":
            ctx.close_path()
            continue
        step = 1 if cmd in "HV" else 2
        for i in range(0, len(nums), step):
            if cmd == "H":
                x = nums[i]
            elif cmd == "V":
                y = nums[i]
            else:
                x, y = nums[i], nums[i + 1]
            if cmd == "M" and i == 0:
                ctx.move_to(x, y)
            else:
                ctx.line_to(x, y)


def trace_points(ctx: cairo.Context, points: str) -> None:
    nums = [float(n) for n in re.split(r"[\s,]+", points.strip())]
    ctx.move_to(nums[0], nums[1])
    for i in range(2, len(nums), 2):
        ctx.line_to(nums[i], nums[i + 1])
    ctx.close_path()


def trace_rect(ctx: cairo.Context, el: ET.Element) -> None:
    x, y = float(el.get("x", 0)), float(el.get("y", 0))
    w, h = float(el.get("width")), float(el.get("height"))
    r = min(float(el.get("rx", 0)), w / 2, h / 2)
    if r <= 0:
        ctx.rectangle(x, y, w, h)
        return
    ctx.new_sub_path()
    ctx.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    ctx.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    ctx.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    ctx.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    ctx.close_path()


def trace(ctx: cairo.Context, el: ET.Element) -> bool:
    tag = el.tag.replace(NS, "")
    if tag == "polygon":
        trace_points(ctx, el.get("points"))
    elif tag == "path":
        trace_path(ctx, el.get("d"))
    elif tag == "rect":
        trace_rect(ctx, el)
    elif tag == "circle":
        ctx.new_sub_path()
        ctx.arc(float(el.get("cx")), float(el.get("cy")), float(el.get("r")), 0, 2 * math.pi)
    else:
        return False
    return True


STYLE_KEYS = ("fill", "stroke", "stroke-width", "stroke-linejoin", "stroke-linecap")
JOINS = {"round": cairo.LINE_JOIN_ROUND, "bevel": cairo.LINE_JOIN_BEVEL, "miter": cairo.LINE_JOIN_MITER}
CAPS = {"round": cairo.LINE_CAP_ROUND, "square": cairo.LINE_CAP_SQUARE, "butt": cairo.LINE_CAP_BUTT}


def draw_element(ctx: cairo.Context, el: ET.Element, style: dict[str, str]) -> None:
    style = dict(style)
    for key in STYLE_KEYS:
        if el.get(key) is not None:
            style[key] = el.get(key)
    ctx.save()
    ctx.transform(parse_matrix(el.get("transform")))
    tag = el.tag.replace(NS, "")
    if tag in ("g", "svg"):
        for child in el:
            draw_element(ctx, child, style)
    else:
        ctx.new_path()
        if trace(ctx, el):
            fill = style.get("fill", "#000000")
            stroke = style.get("stroke", "none")
            if fill != "none":
                ctx.set_source_rgb(*parse_color(fill))
                ctx.fill_preserve()
            if stroke != "none":
                ctx.set_source_rgb(*parse_color(stroke))
                ctx.set_line_width(float(style.get("stroke-width", 1)))
                ctx.set_line_join(JOINS[style.get("stroke-linejoin", "miter")])
                ctx.set_line_cap(CAPS[style.get("stroke-linecap", "butt")])
                ctx.stroke_preserve()
        ctx.new_path()
    ctx.restore()


def view_box(root: ET.Element) -> tuple[float, float, float, float]:
    return tuple(float(n) for n in root.get("viewBox").split())


def draw_logo(ctx: cairo.Context, root: ET.Element, x: float, y: float, w: float, h: float) -> None:
    vx, vy, vw, vh = view_box(root)
    s = min(w / vw, h / vh)
    ctx.save()
    ctx.translate(x + (w - vw * s) / 2, y + (h - vh * s) / 2)
    ctx.scale(s, s)
    ctx.translate(-vx, -vy)
    draw_element(ctx, root, {})
    ctx.restore()


def rgb(color: tuple[int, int, int]) -> tuple[float, float, float]:
    return tuple(c / 255.0 for c in color)


def draw_tile_background(ctx: cairo.Context, size: float) -> None:
    gradient = cairo.LinearGradient(0, 0, 0, size)
    gradient.add_color_stop_rgb(0, *rgb(TILE_TOP))
    gradient.add_color_stop_rgb(1, *rgb(TILE_BOTTOM))
    ctx.rectangle(0, 0, size, size)
    ctx.set_source(gradient)
    ctx.fill()
    glow = cairo.RadialGradient(size * 0.5, size * 0.45, 0, size * 0.5, size * 0.45, size * 0.62)
    glow.add_color_stop_rgba(0, *rgb(TILE_GLOW), 0.55)
    glow.add_color_stop_rgba(1, *rgb(TILE_GLOW), 0.0)
    ctx.rectangle(0, 0, size, size)
    ctx.set_source(glow)
    ctx.fill()


def render(root: ET.Element, kind: str, width: int, height: int) -> cairo.ImageSurface:
    if kind == "tile":
        surface = cairo.ImageSurface(cairo.FORMAT_RGB24, width, height)
        ctx = cairo.Context(surface)
        ctx.set_antialias(cairo.ANTIALIAS_BEST)
        draw_tile_background(ctx, width)
        inner = width * TILE_FIT
        draw_logo(ctx, root, (width - inner) / 2, (height - inner) / 2, inner, inner)
    else:
        surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, width, height)
        ctx = cairo.Context(surface)
        ctx.set_antialias(cairo.ANTIALIAS_BEST)
        inset = (1 - TILE_FIT) / 2 if kind == "padded" else 0
        draw_logo(ctx, root, width * inset, height * inset, width * (1 - 2 * inset), height * (1 - 2 * inset))
    surface.flush()
    return surface


def adaptive_layer(root: ET.Element, layer: str, size: int) -> bytes:
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
    ctx = cairo.Context(surface)
    ctx.set_antialias(cairo.ANTIALIAS_BEST)
    inner = size * ADAPTIVE_VISIBLE * ADAPTIVE_FIT
    draw_logo(ctx, root, (size - inner) / 2, (size - inner) / 2, inner, inner)
    surface.flush()
    data = png_bytes(surface)
    if layer == "foreground":
        return data
    image = Image.open(io.BytesIO(data)).convert("RGBA")
    pixels = image.load()
    for y in range(image.height):
        for x in range(image.width):
            r, g, b, a = pixels[x, y]
            light = max(0.0, min(1.0, ((r + g + b) / 765 - 0.08) / 0.92))
            pixels[x, y] = (255, 255, 255, int(round(a * light)))
    buf = io.BytesIO()
    image.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_android_launcher(root: ET.Element) -> None:
    for density, size in ANDROID_LAUNCHER.items():
        (ANDROID_RES / f"mipmap-{density}" / "ic_launcher.png").write_bytes(
            png_bytes(render(root, "padded", size, size)))
    for density, size in ANDROID_ADAPTIVE.items():
        for layer in ("foreground", "monochrome"):
            (ANDROID_RES / f"mipmap-{density}" / f"ic_launcher_{layer}.png").write_bytes(adaptive_layer(root, layer, size))


def png_bytes(surface: cairo.ImageSurface) -> bytes:
    buf = io.BytesIO()
    surface.write_to_png(buf)
    return buf.getvalue()


def write_raster(root: ET.Element, rel: str, kind: str, width: int, height: int) -> None:
    path = ROOT / rel
    if kind == "ico":
        frames = [Image.open(io.BytesIO(png_bytes(render(root, "square", s, s)))) for s in (16, 32, 48)]
        frames[-1].save(path, format="ICO", sizes=[(16, 16), (32, 32), (48, 48)],
                        append_images=frames[:-1])
        return
    data = png_bytes(render(root, kind, width, height))
    if path.suffix == ".webp":
        Image.open(io.BytesIO(data)).save(path, format="WEBP", lossless=True, method=6)
    else:
        path.write_bytes(data)


def ios_icon_files(icon_set: Path) -> dict[str, int]:
    contents = json.loads((icon_set / "Contents.json").read_text())
    files = {}
    for image in contents["images"]:
        points = float(image["size"].split("x")[0])
        factor = int(image.get("scale", "1x").rstrip("x"))
        files[image["filename"]] = round(points * factor)
    return files


def fmt(n: float) -> str:
    text = ("%.3f" % n).rstrip("0").rstrip(".")
    return "0" if text == "-0" else text


def circle_path(cx: float, cy: float, r: float) -> str:
    k = 0.5522847498 * r
    return (
        f"M{fmt(cx + r)} {fmt(cy)}"
        f"C{fmt(cx + r)} {fmt(cy + k)} {fmt(cx + k)} {fmt(cy + r)} {fmt(cx)} {fmt(cy + r)}"
        f"C{fmt(cx - k)} {fmt(cy + r)} {fmt(cx - r)} {fmt(cy + k)} {fmt(cx - r)} {fmt(cy)}"
        f"C{fmt(cx - r)} {fmt(cy - k)} {fmt(cx - k)} {fmt(cy - r)} {fmt(cx)} {fmt(cy - r)}"
        f"C{fmt(cx + k)} {fmt(cy - r)} {fmt(cx + r)} {fmt(cy - k)} {fmt(cx + r)} {fmt(cy)}Z"
    )


def multiply(outer: cairo.Matrix, inner: cairo.Matrix) -> cairo.Matrix:
    return inner.multiply(outer)


def bimi_elements(el: ET.Element, matrix: cairo.Matrix, style: dict[str, str], out: list[str]) -> None:
    style = dict(style)
    for key in STYLE_KEYS:
        if el.get(key) is not None:
            style[key] = el.get(key)
    matrix = multiply(matrix, parse_matrix(el.get("transform")))
    tag = el.tag.replace(NS, "")
    if tag in ("g", "svg"):
        for child in el:
            bimi_elements(child, matrix, style, out)
        return
    if tag == "polygon":
        nums = re.split(r"[\s,]+", el.get("points").strip())
        pairs = [f"{nums[i]} {nums[i + 1]}" for i in range(0, len(nums), 2)]
        shape = ("path", {"d": "M" + "L".join(pairs) + "Z"})
    elif tag == "path":
        shape = ("path", {"d": el.get("d")})
    elif tag == "circle":
        shape = ("path", {"d": circle_path(float(el.get("cx")), float(el.get("cy")), float(el.get("r")))})
    elif tag == "rect":
        shape = ("rect", {k: el.get(k) for k in ("x", "y", "width", "height", "rx") if el.get(k) is not None})
    else:
        return
    name, attrs = shape
    attrs["transform"] = "matrix(%s)" % " ".join(fmt(v) for v in matrix)
    for key in STYLE_KEYS:
        if key in style:
            attrs[key] = style[key]
    out.append("<%s %s/>" % (name, " ".join(f'{k}="{v}"' for k, v in attrs.items())))


def bimi_svg(root: ET.Element) -> str:
    vx, vy, vw, vh = view_box(root)
    centre = cairo.Matrix(BIMI_SCALE, 0, 0, BIMI_SCALE,
                          BIMI_SIZE / 2 - (vx + vw / 2) * BIMI_SCALE,
                          BIMI_SIZE / 2 - (vy + vh / 2) * BIMI_SCALE)
    shapes: list[str] = []
    for child in root:
        bimi_elements(child, centre, {}, shapes)
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg version="1.2" baseProfile="tiny-ps" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {BIMI_SIZE} {BIMI_SIZE}">',
        "<title>ForkMesh</title>",
        f'<rect width="{BIMI_SIZE}" height="{BIMI_SIZE}" fill="{BIMI_BG}"/>',
        *shapes,
        "</svg>",
        "",
    ]
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Render every ForkMesh logo file from logo.svg.")
    parser.add_argument("--preview", type=Path, help="write only a 1024 px app-icon preview to this path")
    parser.add_argument("--android", action="store_true", help="write only the Android launcher icons")
    args = parser.parse_args(argv)
    master_text = MASTER.read_text(encoding="utf-8")
    root = ET.fromstring(master_text)
    if args.preview:
        args.preview.write_bytes(png_bytes(render(root, "tile", 1024, 1024)))
        return 0
    if args.android:
        write_android_launcher(root)
        return 0
    for copy in SVG_COPIES:
        copy.write_text(master_text, encoding="utf-8")
    BIMI.write_text(bimi_svg(root), encoding="utf-8")
    for rel, kind, width, height in CUTOUTS:
        write_raster(root, rel, kind, width, height)
    write_android_launcher(root)
    for icon_set in (IOS_PHONE, IOS_WATCH):
        for filename, pixels in ios_icon_files(icon_set).items():
            (icon_set / filename).write_bytes(png_bytes(render(root, "tile", pixels, pixels)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
