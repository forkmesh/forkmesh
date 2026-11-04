#!/usr/bin/env python3
from __future__ import annotations

import math
import sys
from pathlib import Path


ICONS: dict[str, str] = {
    "home": "M3 11l9-7 9 7M5 9.5V20h5v-6h4v6h5V9.5",
    "layers": "M12 3l9 5-9 5-9-5zM3 12.5l9 5 9-5M3 16.5l9 5 9-5",
    "grid": "M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z",
    "clock": "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM12 7v5l3 2",
    "list": "M9 6h11M9 12h11M9 18h11M4.5 6h.01M4.5 12h.01M4.5 18h.01",
    "cube": "M12 3l8 4.5v9L12 21l-8-4.5v-9zM4 7.5l8 4.5 8-4.5M12 12v9",
    "compass": "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM15.5 8.5l-2 5-5 2 2-5z",
    "doc": "M14 3H6v18h12V7zM14 3v4h4M9 12h6M9 16h4",
    "shield": "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z",
    "shield-check": "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6zM8.5 12l2.5 2.5 4.5-5",
    "gauge": "M3.5 17a9 9 0 1 1 17 0M12 14l4.5-5M12 15.5a1.5 1.5 0 1 0 0-3 1.5 1.5 0 0 0 0 3z",
    "phone": "M8 2h8a2 2 0 0 1 2 2v16a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2zM11 18h2",
    "flag": "M5 21V4M5 4h11l-2 4 2 4H5",
    "monitor": "M3 4h18v12H3zM8 21h8M12 16v5",
    "browser": "M3 5h18v14H3zM3 9h18M6 7h.01M9 7h.01",
    "package": "M12 3l8 4.5v9L12 21l-8-4.5v-9zM4 7.5l8 4.5 8-4.5M12 12v9M8 5.3l8 4.5",
    "globe": "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM3 12h18M12 3c3 3.5 3 14.5 0 18M12 3c-3 3.5-3 14.5 0 18",
    "sliders": "M4 6h9M17 6h3M15 4v4M4 12h3M11 12h9M9 10v4M4 18h11M19 18h1M17 16v4",
    "braces": "M8 3H7a2 2 0 0 0-2 2v4a2 2 0 0 1-2 2 2 2 0 0 1 2 2v4a2 2 0 0 0 2 2h1M16 3h1a2 2 0 0 1 2 2v4a2 2 0 0 0 2 2 2 2 0 0 0-2 2v4a2 2 0 0 1-2 2h-1",
    "lock": "M5.5 11h13v10h-13zM8.5 11V7.5a3.5 3.5 0 0 1 7 0V11M12 15v2.5",
    "key": "M18.5 8.5a3.5 3.5 0 1 1-7 0 3.5 3.5 0 0 1 7 0zM12.6 11L4 19.5M7 16.5l2 2M9.5 14l2 2",
    "server": "M4 4h16v6H4zM4 14h16v6H4zM8 7h.01M8 17h.01",
    "mirror": "M8 8h11v11H8zM5 16V5h11",
    "network": "M12 7.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5zM5 21.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5zM19 21.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5zM12 7.5V12M12 12l-5.2 4.8M12 12l5.2 4.8",
    "eye": "M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12zM12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z",
    "radar": "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM12 16a4 4 0 1 0 0-8 4 4 0 0 0 0 8zM12 12l6-6",
    "users": "M9 11a4 4 0 1 0 0-8 4 4 0 0 0 0 8zM2 21v-1a6 6 0 0 1 6-6h2a6 6 0 0 1 6 6v1M16 3.5a4 4 0 0 1 0 7M22 21v-1a6 6 0 0 0-4-5.6",
    "activity": "M3 12h4l3-8 4 16 3-8h4",
    "merge": "M6 3.5a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM6 7.5V21M18 14.5a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM6 9.5c0 5 12 3 12 5",
    "download": "M12 3v12M7 10l5 5 5-5M5 21h14",
    "rocket": "M5 19c.8-2.6 2.4-4.4 4.5-5.5M14 4.5c3 0 5.5 2.5 5.5 5.5-1.6 4-5.6 7-8.8 8L6 13.3c1-3.2 4-7.2 8-8.8zM14.5 9.5h.01",
    "arrow": "M7 17L17 7M8.5 7H17v8.5",
    "chevron": "M9 6l6 6-6 6",
    "message": "M21 15a4 4 0 0 1-4 4H8l-5 3V7a4 4 0 0 1 4-4h10a4 4 0 0 1 4 4z",
    "spark": "M12 3l1.9 5.6L19.5 10l-5.6 1.9L12 17.5l-1.9-5.6L4.5 10l5.6-1.4z",
    "usb": "M10 2h4v5h-4zM8 7h8v8a4 4 0 0 1-8 0zM12 19v3",
}


def icon(name: str, cls: str = "dp-icon") -> str:
    return (
        f'<svg class="{cls}" viewBox="0 0 24 24" aria-hidden="true">'
        f'<path d="{ICONS[name]}"></path></svg>'
    )


COS = math.cos(math.radians(30))


class Canvas:
    def __init__(self, uid: str, width: int, height: int, cx: float, cy: float, scale: float):
        self.uid = uid
        self.width = width
        self.height = height
        self.cx = cx
        self.cy = cy
        self.scale = scale
        self.parts: list[str] = []

    def point(self, x: float, y: float, z: float) -> tuple[float, float]:
        return (
            self.cx + (x - y) * COS * self.scale,
            self.cy + (x + y) * 0.5 * self.scale - z * self.scale,
        )

    def poly(self, points: list[tuple[float, float]], cls: str, extra: str = "") -> None:
        text = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
        self.parts.append(f'<polygon class="{cls}" points="{text}"{extra}></polygon>')

    def add(self, markup: str) -> None:
        self.parts.append(markup)

    def box(self, x: float, y: float, z: float, w: float, d: float, h: float, kind: str = "slab", top_fill: str = "") -> None:
        p = self.point
        top = z + h
        self.poly([p(x, y + d, top), p(x + w, y + d, top), p(x + w, y + d, z), p(x, y + d, z)], f"a-{kind}-left")
        self.poly([p(x + w, y, top), p(x + w, y + d, top), p(x + w, y + d, z), p(x + w, y, z)], f"a-{kind}-right")
        fill = f' fill="url(#{self.uid}-{top_fill})"' if top_fill else ""
        self.poly([p(x, y, top), p(x + w, y, top), p(x + w, y + d, top), p(x, y + d, top)], f"a-{kind}-top", fill)

    def ellipse(self, x: float, y: float, z: float, radius: float, cls: str, extra: str = "") -> None:
        sx, sy = self.point(x, y, z)
        rx = radius * COS * self.scale * math.sqrt(2)
        ry = radius * 0.5 * self.scale * math.sqrt(2)
        self.parts.append(
            f'<ellipse class="{cls}" cx="{sx:.1f}" cy="{sy:.1f}" rx="{rx:.1f}" ry="{ry:.1f}"{extra}></ellipse>'
        )

    def arc(self, x: float, y: float, z: float, radius: float, cls: str, front: bool) -> None:
        sx, sy = self.point(x, y, z)
        rx = radius * COS * self.scale * math.sqrt(2)
        ry = radius * 0.5 * self.scale * math.sqrt(2)
        sweep = 0 if front else 1
        self.parts.append(
            f'<path class="{cls}" d="M{sx - rx:.1f},{sy:.1f} A{rx:.1f},{ry:.1f} 0 0 {sweep} {sx + rx:.1f},{sy:.1f}"></path>'
        )

    def line(self, a: tuple[float, float, float], b: tuple[float, float, float], cls: str = "a-link") -> None:
        (x1, y1), (x2, y2) = self.point(*a), self.point(*b)
        self.parts.append(f'<path class="{cls}" d="M{x1:.1f},{y1:.1f} L{x2:.1f},{y2:.1f}"></path>')

    def dot(self, x: float, y: float, r: float, cls: str = "a-dot") -> None:
        self.parts.append(f'<circle class="{cls}" cx="{x:.1f}" cy="{y:.1f}" r="{r:.1f}"></circle>')

    def defs(self) -> str:
        u = self.uid
        return (
            "<defs>"
            f'<linearGradient id="{u}-face" x1="0" y1="0" x2="1" y2="1">'
            '<stop offset="0" class="a-stop-a"></stop><stop offset="1" class="a-stop-b"></stop></linearGradient>'
            f'<linearGradient id="{u}-plate" x1="0" y1="0" x2="1" y2="1">'
            '<stop offset="0" class="a-stop-plate-a"></stop><stop offset="1" class="a-stop-plate-b"></stop></linearGradient>'
            f'<radialGradient id="{u}-glow" cx="0.5" cy="0.5" r="0.5">'
            '<stop offset="0" class="a-stop-glow"></stop><stop offset="1" class="a-stop-clear"></stop></radialGradient>'
            "</defs>"
        )

    def render(self, label: str = "") -> str:
        role = f' role="img" aria-label="{label}"' if label else ' aria-hidden="true"'
        return (
            f'<svg class="dp-art" viewBox="0 0 {self.width} {self.height}"{role} focusable="false">'
            + self.defs()
            + "".join(self.parts)
            + "</svg>"
        )


SHAPES: dict[str, str] = {
    "shield": "M0,-50 L40,-37 V-4 C40,24 23,41 0,50 C-23,41 -40,24 -40,-4 V-37 Z",
    "tile": "M-26,-38 H26 A12,12 0 0 1 38,-26 V26 A12,12 0 0 1 26,38 H-26 A12,12 0 0 1 -38,26 V-26 A12,12 0 0 1 -26,-38 Z",
    "circle": "M0,-40 A40,40 0 1 1 0,40 A40,40 0 1 1 0,-40 Z",
    "hex": "M0,-44 L38,-22 V22 L0,44 L-38,22 V-22 Z",
    "doc": "M-30,-44 H14 L32,-26 V44 H-30 Z",
    "phone": "M-17,-46 H17 A9,9 0 0 1 26,-37 V37 A9,9 0 0 1 17,46 H-17 A9,9 0 0 1 -26,37 V-37 A9,9 0 0 1 -17,-46 Z",
    "monitor": "M-42,-32 H42 A6,6 0 0 1 48,-26 V18 A6,6 0 0 1 42,24 H-42 A6,6 0 0 1 -48,18 V-26 A6,6 0 0 1 -42,-32 Z",
    "browser": "M-40,-34 H40 A8,8 0 0 1 48,-26 V26 A8,8 0 0 1 40,34 H-40 A8,8 0 0 1 -48,26 V-26 A8,8 0 0 1 -40,-34 Z",
}


def billboard(canvas: Canvas, shape: str, glyph: str, x: float, y: float, size: float = 1.0, depth: float = 7.0) -> None:
    path = SHAPES[shape]
    u = canvas.uid
    canvas.add(
        f'<g transform="translate({x:.1f} {y:.1f}) scale({size:.2f})">'
        f'<path class="a-deep" d="{path}" transform="translate({depth:.1f} {depth * 0.45:.1f})"></path>'
        f'<path class="a-face" d="{path}" fill="url(#{u}-face)"></path>'
        f'<path class="a-rim" d="{path}" transform="scale(0.86)"></path>'
        f'<g class="a-glyph" transform="translate(-19 -19) scale(1.6)"><path d="{ICONS[glyph]}"></path></g>'
        "</g>"
    )
    if shape == "monitor":
        canvas.add(
            f'<path class="a-deep" d="M{x - 10 * size:.1f},{y + 24 * size:.1f} h{20 * size:.1f} l{4 * size:.1f},{12 * size:.1f} h{-28 * size:.1f} z"></path>'
        )


def platform(canvas: Canvas, radius: float = 3.1, levels: int = 3) -> float:
    canvas.ellipse(0, 0, 0, radius * 1.35, "a-halo", f' fill="url(#{canvas.uid}-glow)"')
    z = 0.0
    r = radius
    for level in range(levels):
        thickness = 0.26 if level == 0 else 0.2
        canvas.box(-r, -r, z, 2 * r, 2 * r, thickness, "slab", "plate")
        z += thickness + 0.3
        r *= 0.74
    return z - 0.3


def sparkles(canvas: Canvas, points: list[tuple[float, float, float]]) -> None:
    for x, y, r in points:
        canvas.dot(x, y, r, "a-spark")


def mini_cube(canvas: Canvas, x: float, y: float, z: float, s: float = 0.55) -> None:
    canvas.box(x - s / 2, y - s / 2, z, s, s, s, "cube")


def ring(canvas: Canvas, z: float, radius: float, behind: bool) -> None:
    canvas.arc(0, 0, z, radius, "a-ring", front=not behind)


def scene(key: str, uid: str) -> Canvas:
    c = Canvas(uid, 240, 200, 120, 136, 17)
    top = platform(c)
    c.ellipse(0, 0, top, 1.05, "a-shadow")
    sx, sy = c.point(0, 0, top)
    obj_y = sy - 52
    sparkles(c, [(36, 40, 2.2), (206, 58, 1.8), (196, 120, 2.4), (44, 104, 1.6), (176, 22, 1.4)])
    if key == "shield":
        ring(c, top + 2.4, 2.4, True)
        billboard(c, "shield", "cube", sx, obj_y, 1.0)
        ring(c, top + 2.4, 2.4, False)
        mini_cube(c, -2.3, 1.2, top + 0.2)
        mini_cube(c, 1.6, -2.4, top + 3.8, 0.42)
        mini_cube(c, 2.5, 0.8, top + 0.3, 0.38)
    elif key == "lock":
        ring(c, top + 2.2, 2.3, True)
        billboard(c, "tile", "lock", sx, obj_y, 1.0)
        ring(c, top + 2.2, 2.3, False)
        for angle in (200, 320, 80):
            px = 2.3 * math.cos(math.radians(angle))
            py = 2.3 * math.sin(math.radians(angle))
            bx, by = c.point(px, py, top + 2.2)
            c.dot(bx, by, 4.2, "a-node")
    elif key == "mirror":
        billboard(c, "tile", "mirror", sx - 30, obj_y - 10, 0.72, 5)
        billboard(c, "tile", "merge", sx + 26, obj_y + 4, 0.86, 6)
        c.add(f'<path class="a-flow" d="M{sx - 52:.1f},{obj_y + 34:.1f} C{sx - 40:.1f},{obj_y + 64:.1f} {sx + 4:.1f},{obj_y + 66:.1f} {sx + 14:.1f},{obj_y + 46:.1f}"></path>')
        mini_cube(c, 2.0, 1.4, top + 0.1, 0.45)
    elif key == "server":
        for level in range(3):
            c.box(-1.05, -1.05, top + 0.1 + level * 0.95, 2.1, 2.1, 0.8, "cube")
            lx, ly = c.point(1.05, 0.55, top + 0.5 + level * 0.95)
            c.dot(lx, ly, 2.3, "a-led")
            ax, ay = c.point(-0.7, 1.05, top + 0.5 + level * 0.95)
            bx, by = c.point(0.4, 1.05, top + 0.5 + level * 0.95)
            c.add(f'<path class="a-slot" d="M{ax:.1f},{ay:.1f} L{bx:.1f},{by:.1f}"></path>')
        mini_cube(c, 2.0, -1.6, top + 0.1, 0.5)
        mini_cube(c, -1.9, 1.9, top + 0.1, 0.4)
    elif key == "network":
        nodes = [(-1.6, 1.0), (1.4, 1.3), (0.2, -1.7)]
        for i, a in enumerate(nodes):
            for b in nodes[i + 1:]:
                c.line((a[0], a[1], top + 0.35), (b[0], b[1], top + 0.35))
        for x, y in sorted(nodes, key=lambda n: n[0] + n[1]):
            c.box(x - 0.45, y - 0.45, top, 0.9, 0.9, 0.9, "cube")
        gx, gy = c.point(0, 0, top + 3.6)
        billboard(c, "circle", "globe", gx, gy, 0.55, 4)
    elif key == "radar":
        for radius in (0.9, 1.6, 2.3):
            c.ellipse(0, 0, top, radius, "a-ring-flat")
        billboard(c, "circle", "eye", sx, obj_y, 0.95)
        mini_cube(c, 1.7, 0.9, top + 0.05, 0.42)
        mini_cube(c, -1.2, -1.9, top + 0.05, 0.36)
    elif key == "package":
        c.box(-1.2, -1.2, top + 0.05, 2.4, 2.4, 2.0, "cube")
        p1 = c.point(-1.2, 0, top + 2.05)
        p2 = c.point(1.2, 0, top + 2.05)
        p3 = c.point(1.2, 0, top + 0.05)
        c.add(f'<path class="a-slot" d="M{p1[0]:.1f},{p1[1]:.1f} L{p2[0]:.1f},{p2[1]:.1f} L{p3[0]:.1f},{p3[1]:.1f}"></path>')
        mini_cube(c, 2.1, 1.6, top + 0.05, 0.45)
    elif key == "logo":
        logo_cube(c, sx, obj_y + 6, 0.5)
        mini_cube(c, 2.2, 1.2, top + 0.1, 0.45)
        mini_cube(c, -2.0, -1.4, top + 3.2, 0.36)
    else:
        shape, glyph = AREA_SCENES.get(key, ("tile", "cube"))
        ring(c, top + 2.2, 2.2, True)
        billboard(c, shape, glyph, sx, obj_y, 0.95)
        ring(c, top + 2.2, 2.2, False)
        mini_cube(c, 2.0, 1.3, top + 0.1, 0.42)
    return c


AREA_SCENES: dict[str, tuple[str, str]] = {
    "compass": ("tile", "compass"),
    "doc": ("doc", "doc"),
    "key": ("shield", "key"),
    "gauge": ("circle", "gauge"),
    "phone": ("phone", "phone"),
    "flag": ("tile", "flag"),
    "monitor": ("monitor", "monitor"),
    "browser": ("browser", "browser"),
    "globe": ("circle", "globe"),
    "sliders": ("tile", "sliders"),
    "braces": ("hex", "braces"),
    "activity": ("circle", "activity"),
    "merge": ("tile", "merge"),
    "users": ("circle", "users"),
    "clock": ("circle", "clock"),
    "grid": ("tile", "grid"),
    "usb": ("tile", "usb"),
}


def logo_cube(canvas: Canvas, x: float, y: float, size: float) -> None:
    hexagon = "0,-100 86.6,-50 86.6,50 0,100 -86.6,50 -86.6,-50"
    canvas.add(
        f'<g transform="translate({x:.1f} {y:.1f}) scale({size:.2f})">'
        f'<polygon class="a-deep" points="{hexagon}" transform="translate(10 6)"></polygon>'
        '<polygon class="a-logo-top" points="0,-100 86.6,-50 0,0 -86.6,-50"></polygon>'
        '<polygon class="a-logo-left" points="-86.6,-50 0,0 0,100 -86.6,50"></polygon>'
        '<polygon class="a-logo-right" points="86.6,-50 0,0 0,100 86.6,50"></polygon>'
        '<g transform="matrix(0.866 -0.5 0.866 0.5 -86.6 -50)" class="a-logo-mesh">'
        '<path d="M78 22 L22 78 M40 20 L40 60 M60 80 L60 40"></path>'
        '<circle cx="78" cy="22" r="7.5"></circle><circle cx="22" cy="78" r="7.5"></circle>'
        '<circle cx="40" cy="20" r="7.5"></circle><circle cx="60" cy="80" r="7.5"></circle></g>'
        '<g transform="matrix(0.866 0.5 0 1 -86.6 -50)" class="a-logo-mark"><path d="M7 9 H93 V31 H31 V45 H76 V65 H31 V93 H7 Z"></path></g>'
        '<g transform="matrix(0.866 -0.5 0 1 0 0)" class="a-logo-mark"><path d="M7 93 V9 H28 L50 42 L72 9 H93 V93 H72 V44 L50 77 L28 44 V93 Z"></path></g>'
        "</g>"
    )


def art(key: str, uid: str, label: str = "") -> str:
    return scene(key, uid).render(label)


def hero(uid: str, label: str = "") -> str:
    c = Canvas(uid, 520, 380, 260, 236, 24)
    c.ellipse(0, 0, 0, 6.2, "a-halo", f' fill="url(#{uid}-glow)"')
    c.box(-4.6, -4.6, 0, 9.2, 9.2, 0.3, "slab", "plate")
    ring_nodes = []
    for index in range(6):
        angle = math.radians(index * 60 + 15)
        ring_nodes.append((3.4 * math.cos(angle), 3.4 * math.sin(angle)))
    c.ellipse(0, 0, 0.3, 3.4, "a-ring-flat")
    for a, b in zip(ring_nodes, ring_nodes[1:] + ring_nodes[:1]):
        c.line((a[0], a[1], 0.62), (b[0], b[1], 0.62), "a-link-soft")
    back = [n for n in ring_nodes if n[0] + n[1] < 0]
    front = [n for n in ring_nodes if n[0] + n[1] >= 0]
    for x, y in sorted(back, key=lambda n: n[0] + n[1]):
        c.line((x, y, 0.62), (0, 0, 1.6), "a-link")
        c.box(x - 0.42, y - 0.42, 0.3, 0.84, 0.84, 0.84, "cube")
    c.box(-2.0, -2.0, 0.6, 4.0, 4.0, 0.24, "slab", "plate")
    c.box(-1.35, -1.35, 1.14, 2.7, 2.7, 0.2, "slab", "plate")
    sx, sy = c.point(0, 0, 1.34)
    c.arc(0, 0, 4.2, 2.9, "a-ring", front=False)
    logo_cube(c, sx, sy - 74, 0.62)
    c.arc(0, 0, 4.2, 2.9, "a-ring", front=True)
    for x, y in sorted(front, key=lambda n: n[0] + n[1]):
        c.line((x, y, 0.62), (0, 0, 1.6), "a-link")
        c.box(x - 0.42, y - 0.42, 0.3, 0.84, 0.84, 0.84, "cube")
    for (x, y, z, glyph) in ((-3.9, -1.2, 4.6, "lock"), (1.4, -4.2, 5.2, "shield-check"), (3.6, 1.0, 3.9, "mirror")):
        px, py = c.point(x, y, z)
        billboard(c, "tile", glyph, px, py, 0.42, 4)
    sparkles(c, [(70, 70, 2.6), (452, 92, 2.2), (430, 300, 2.8), (92, 286, 2.0), (360, 30, 1.8), (150, 30, 1.6)])
    return c.render(label)


PARTIALS = Path(__file__).resolve().parents[2] / "www" / "templates" / "partials" / "docs-art"
LANDING_ART: dict[str, str] = {
    "pillar-mirror": "mirror",
    "pillar-radar": "radar",
    "pillar-lock": "lock",
    "pillar-server": "server",
    "pillar-activity": "activity",
    "pillar-merge": "merge",
}


def partials() -> dict[str, str]:
    files = {"home-hero.html": hero("home-hero") + "\n"}
    for name, key in LANDING_ART.items():
        files[f"{name}.html"] = art(key, name) + "\n"
    return files


def stale_partials() -> list[str]:
    return [
        name
        for name, text in partials().items()
        if not (PARTIALS / name).is_file() or (PARTIALS / name).read_text(encoding="utf-8") != text
    ]


def write_partials() -> list[str]:
    PARTIALS.mkdir(parents=True, exist_ok=True)
    written = stale_partials()
    for name, text in partials().items():
        (PARTIALS / name).write_text(text, encoding="utf-8")
    return written


def main(arguments: list[str]) -> int:
    if arguments[:1] == ["--check"]:
        stale = stale_partials()
        for name in stale:
            print(f"stale: {PARTIALS / name}")
        return 1 if stale else 0
    if arguments[:1] == ["--preview"]:
        out = Path(arguments[1]) if len(arguments) > 1 else Path("docs-art-preview.html")
        keys = ["shield", "lock", "mirror", "server", "network", "radar", "package", "logo", *AREA_SCENES]
        cells = "".join(f'<figure>{art(k, f"p{i}")}<figcaption>{k}</figcaption></figure>' for i, k in enumerate(keys))
        out.write_text(
            '<!doctype html><html class="light"><head><link rel="stylesheet" href="/docs-page.css"></head>'
            f'<body class="dp-body"><div style="display:grid;grid-template-columns:repeat(4,260px);gap:16px">{hero("hero")}{cells}</div></body></html>',
            encoding="utf-8",
        )
        return 0
    for name in write_partials():
        print(f"wrote {PARTIALS / name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
