#!/usr/bin/env python3
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "public" / "assets" / "badges"

GLOW = (64, 242, 219)
GLOW_DEEP = (13, 140, 143)
LILAC = (143, 102, 209)
VIOLET = (102, 61, 168)
PLUM = (54, 33, 97)
SHELL = (26, 26, 51)
SHELL_LIGHT = (56, 56, 92)
PITCH = (8, 8, 20)
DEEP_SEA = (26, 77, 102)
SILVER = (204, 209, 224)
SILVER_DARK = (117, 122, 143)
WHITE = (255, 255, 255)
GOLD = (255, 206, 84)
GOLD_LIGHT = (255, 240, 180)
GOLD_DEEP = (176, 120, 30)

SHOULDERS = [(35, 76), (30, 71), (28, 65), (29, 59)]
GROUND = 96.2
KNEES = [(27, 79), (20, 72), (15, 61), (13.5, 51)]
FEET = [(31, GROUND), (17.5, GROUND), (8.5, GROUND), (2.5, GROUND)]

TIERS = {
    "default": None,
    "bronze": {
        "metal": ((255, 214, 178), (214, 138, 86), (140, 76, 38), (74, 36, 16)),
        "etch": (92, 44, 18), "shine": (255, 232, 210), "pips": 1,
    },
    "silver": {
        "metal": ((255, 255, 255), (214, 219, 232), (140, 146, 166), (70, 74, 92)),
        "etch": (78, 82, 102), "shine": (255, 255, 255), "pips": 2,
    },
    "gold": {
        "metal": ((255, 248, 214), (255, 206, 84), (196, 138, 32), (110, 70, 10)),
        "etch": (120, 78, 8), "shine": (255, 252, 230), "pips": 3,
    },
}


def f(v):
    text = "%.2f" % v
    text = text.rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def rgb(c):
    return "#%02x%02x%02x" % tuple(int(round(x)) for x in c[:3])


class Svg:
    def __init__(self, prefix):
        self.prefix = prefix
        self.defs = []
        self.body = []
        self.count = 0

    def uid(self, kind):
        self.count += 1
        return "%s-%s%d" % (self.prefix, kind, self.count)

    def stops(self, colors):
        out = []
        for item in colors:
            offset, color = item[0], item[1]
            opacity = item[2] if len(item) > 2 else 1
            attrs = 'offset="%s" stop-color="%s"' % (f(offset), rgb(color))
            if opacity < 1:
                attrs += ' stop-opacity="%s"' % f(opacity)
            out.append("<stop %s/>" % attrs)
        return "".join(out)

    def radial(self, cx, cy, r, colors):
        gid = self.uid("r")
        self.defs.append(
            '<radialGradient id="%s" gradientUnits="userSpaceOnUse" '
            'cx="%s" cy="%s" r="%s">%s</radialGradient>'
            % (gid, f(cx), f(cy), f(max(r, 0.01)), self.stops(colors)))
        return "url(#%s)" % gid

    def linear(self, x1, y1, x2, y2, colors):
        gid = self.uid("l")
        self.defs.append(
            '<linearGradient id="%s" gradientUnits="userSpaceOnUse" '
            'x1="%s" y1="%s" x2="%s" y2="%s">%s</linearGradient>'
            % (gid, f(x1), f(y1), f(x2), f(y2), self.stops(colors)))
        return "url(#%s)" % gid

    def clip(self, inner):
        cid = self.uid("c")
        self.defs.append('<clipPath id="%s">%s</clipPath>' % (cid, inner))
        return "url(#%s)" % cid

    def add(self, text):
        self.body.append(text)

    def render(self, label):
        return (
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" '
            'width="128" height="128" role="img" aria-label="%s">\n'
            "<defs>%s</defs>\n%s\n</svg>\n"
            % (label, "".join(self.defs), "\n".join(self.body)))


def paint(color, opacity=1):
    out = 'fill="%s"' % rgb(color)
    if opacity < 1:
        out += ' fill-opacity="%s"' % f(opacity)
    return out


def line(color, width, opacity=1):
    out = ('fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round" '
           'stroke-linejoin="round"' % (rgb(color), f(width)))
    if opacity < 1:
        out += ' stroke-opacity="%s"' % f(opacity)
    return out


def circle(cx, cy, r, attrs):
    return '<circle cx="%s" cy="%s" r="%s" %s/>' % (f(cx), f(cy), f(r), attrs)


def ellipse(cx, cy, rx, ry, attrs):
    return '<ellipse cx="%s" cy="%s" rx="%s" ry="%s" %s/>' % (
        f(cx), f(cy), f(rx), f(ry), attrs)


def path(d, attrs):
    return '<path d="%s" %s/>' % (d, attrs)


def glow(svg, x, y, r, color, opacity):
    if r <= 0 or opacity <= 0:
        return ""
    return circle(x, y, r, 'fill="%s"' % svg.radial(
        x, y, r, [(0, color, opacity), (1, color, 0)]))


def star_path(cx, cy, outer, inner, points=5, turn=-90):
    pts = []
    for k in range(points * 2):
        r = outer if k % 2 == 0 else inner
        a = math.radians(turn + k * 180 / points)
        pts.append("%s %s" % (f(cx + math.cos(a) * r), f(cy + math.sin(a) * r)))
    return "M" + " L".join(pts) + " Z"


def sparkle_path(cx, cy, r, waist=0.22):
    w = r * waist
    return ("M%s %s Q%s %s %s %s Q%s %s %s %s Q%s %s %s %s Q%s %s %s %s Z" % (
        f(cx), f(cy - r), f(cx + w), f(cy - w), f(cx + r), f(cy),
        f(cx + w), f(cy + w), f(cx), f(cy + r),
        f(cx - w), f(cy + w), f(cx - r), f(cy),
        f(cx - w), f(cy - w), f(cx), f(cy - r)))


def spider(svg, pose):
    gaze = pose.get("gaze", (0, 0))
    raised = pose.get("raised", 0)
    grin = pose.get("grin", 1.6)
    bust = pose.get("bust", False)
    eye_style = pose.get("eyes", "normal")
    mouth = pose.get("mouth", "smile")
    out = []
    g = 1.0

    def leg_pose(index, side):
        def m(q):
            return (q[0], q[1]) if side < 0 else (100 - q[0], q[1])
        shoulder = m(SHOULDERS[index])
        knee = m(KNEES[index])
        foot = m(FEET[index])
        if index == 0 and raised:
            foot = m((3, 44))
            knee = m((11, 68))
        return shoulder, knee, foot

    def draw_leg(shoulder, knee, foot, back):
        shade = 0.85 if back else 1
        sx, sy = shoulder
        kx, ky = knee
        fx, fy = foot
        out.append(circle(sx, sy, 3, 'fill="%s"' % svg.radial(
            sx - 0.8, sy - 0.9, 3.4, [(0, SHELL_LIGHT, shade), (1, PITCH, shade)])))
        d = "M%s %sL%s %s" % (f(sx), f(sy), f(kx), f(ky))
        out.append(path(d, line(PITCH, 6.2, shade)))
        out.append(path(d, line(SHELL, 4.6, shade)))
        out.append(path("M%s %sL%s %s" % (f(sx), f(sy - 1.1), f(kx), f(ky - 1.1)),
                        line(SHELL_LIGHT, 1.1, 0.75 * shade)))
        dx, dy = fx - kx, fy - ky
        length = max(math.hypot(dx, dy), 0.01)
        nx, ny = -dy / length, dx / length
        outward = -1 if fx < 50 else 1
        if nx * outward < 0 or (abs(nx) < 0.05 and ny > 0):
            nx, ny = -nx, -ny
        mx, my = kx + dx * 0.42, ky + dy * 0.42
        bulge = min(length * 0.24, 5.5)
        a = (kx + nx * 3.7, ky + ny * 3.7)
        c1 = (mx + nx * (bulge + 3), my + ny * (bulge + 3))
        c2 = (mx + nx * (bulge * 0.5 - 0.6), my + ny * (bulge * 0.5 - 0.6))
        b = (kx - nx * 3.1, ky - ny * 3.1)
        claw = "M%s %sQ%s %s %s %sQ%s %s %s %sZ" % (
            f(a[0]), f(a[1]), f(c1[0]), f(c1[1]), f(fx), f(fy),
            f(c2[0]), f(c2[1]), f(b[0]), f(b[1]))
        out.append(path(claw, 'fill="%s"' % svg.linear(kx, ky, fx, fy, [
            (0, SHELL_LIGHT, shade), (0.35, SHELL, shade), (0.72, PITCH, shade),
            (1, GLOW, g * shade)])))
        r0 = (kx + nx * 2.4, ky + ny * 2.4)
        r1 = (mx + nx * (bulge + 1.7), my + ny * (bulge + 1.7))
        r2 = (kx + dx * 0.82, ky + dy * 0.82)
        out.append(path("M%s %sQ%s %s %s %s" % (
            f(r0[0]), f(r0[1]), f(r1[0]), f(r1[1]), f(r2[0]), f(r2[1])),
            line(SILVER, 0.7, 0.28 * shade)))
        v0 = (kx + dx * 0.12, ky + dy * 0.12)
        v1 = (mx + nx * bulge * 0.8, my + ny * bulge * 0.8)
        v2 = (kx + dx * 0.62, ky + dy * 0.62)
        out.append(path("M%s %sQ%s %s %s %s" % (
            f(v0[0]), f(v0[1]), f(v1[0]), f(v1[1]), f(v2[0]), f(v2[1])),
            line(GLOW, 0.75, 0.7 * g * shade)))
        out.append(glow(svg, fx, fy, 3.6, GLOW, 0.6 * g * shade))
        out.append(glow(svg, kx, ky, 6, GLOW, 0.42 * g * shade))
        out.append(circle(kx, ky, 3.9, 'fill="%s"' % svg.radial(
            kx - 1, ky - 1.2, 4.4, [(0, SHELL_LIGHT, shade), (1, PITCH, shade)])))
        out.append(circle(kx, ky, 2.2, 'fill="%s"' % svg.radial(
            kx - 0.5, ky - 0.5, 2.4, [(0, WHITE, 0.9 * shade), (0.5, GLOW, g * shade),
                                      (1, GLOW_DEEP, shade)])))
        out.append(circle(kx, ky, 3.9, line(SILVER_DARK, 0.35, 0.5 * shade)))

    def legs(front):
        if bust:
            if front:
                for side in (-1, 1):
                    draw_leg(*leg_pose(0, side), back=False)
            return
        for index in ((1, 0) if front else (3, 2)):
            for side in (-1, 1):
                draw_leg(*leg_pose(index, side), back=not front)

    out.append(glow(svg, 50, 60, 40, GLOW, 0.2))
    tail = "" if bust else ("M45 79C39 84 49 87 42 92C35 97.5 47 101 53 98.5"
            "C49.5 97 44.5 96.2 45.8 93C58.5 87 55 82 56 79Z")
    if tail:
        out.append(path(tail, 'fill="%s"' % svg.linear(42, 80, 52, 99, [
            (0, SHELL_LIGHT), (0.5, SHELL), (1, PITCH)])))
        out.append(path(tail, line(GLOW, 0.45, 0.32)))
    legs(False)

    out.append(ellipse(50, 61, 25.5, 25, 'fill="%s"' % svg.radial(
        41, 54, 34, [(0, SHELL_LIGHT), (0.5, SHELL), (1, PITCH)])))
    arc = []
    for k in range(25):
        a = math.radians(15 + 65 * k / 24)
        arc.append("%s %s" % (f(50 + math.cos(a) * 24.6), f(61 + math.sin(a) * 24.6)))
    out.append(path("M" + " L".join(arc), line(GLOW, 0.8, 0.4)))
    blush = (255, 120, 170) if pose.get("blush") else VIOLET
    blush_alpha = 0.55 if pose.get("blush") else 0.24
    out.append(ellipse(36, 74, 4.6, 2.6, paint(blush, blush_alpha)))
    out.append(ellipse(64, 74, 4.6, 2.6, paint(blush, blush_alpha)))

    base, bend, tip = (50, 28), (44, 13), (55, 0.5)

    def spine(u):
        return (base[0] * (1 - u) ** 2 + bend[0] * 2 * (1 - u) * u + tip[0] * u * u,
                base[1] * (1 - u) ** 2 + bend[1] * 2 * (1 - u) * u + tip[1] * u * u)

    def normal(u):
        dx = (bend[0] - base[0]) * 2 * (1 - u) + (tip[0] - bend[0]) * 2 * u
        dy = (bend[1] - base[1]) * 2 * (1 - u) + (tip[1] - bend[1]) * 2 * u
        length = max(math.hypot(dx, dy), 0.01)
        return (-dy / length, dx / length)

    steps = 14
    left, right = [], []
    for k in range(steps + 1):
        u = k / steps
        notch = 0.74 if k % 2 == 1 else 1
        width = 7.4 * math.sin(math.pi * min(u * 1.05, 1)) ** 0.75 * (1 - u * 0.3) * notch
        at, n = spine(u), normal(u)
        left.append((at[0] + n[0] * width, at[1] + n[1] * width))
        right.append((at[0] - n[0] * width * 0.92, at[1] - n[1] * width * 0.92))
    leaf = "M50 28" + "".join("L%s %s" % (f(x), f(y)) for x, y in left) + \
        "".join("L%s %s" % (f(x), f(y)) for x, y in reversed(right)) + "Z"
    out.append(path(leaf, 'fill="%s"' % svg.linear(42, 8, 58, 26, [
        (0, SHELL_LIGHT), (0.5, SHELL), (1, PITCH)])))
    out.append(path(leaf, line(PITCH, 0.35, 0.6)))
    barbs = []
    for k in range(2, steps - 1, 2):
        sx, sy = spine(k / steps - 0.03)
        barbs.append("M%s %sL%s %sM%s %sL%s %s" % (
            f(sx), f(sy), f(left[k + 1][0]), f(left[k + 1][1]),
            f(sx), f(sy), f(right[k + 1][0]), f(right[k + 1][1])))
    out.append(path("".join(barbs), line(PITCH, 0.4, 0.55)))
    end = spine(0.94)
    vein = "M50 28Q44 13 %s %s" % (f(end[0]), f(end[1]))
    out.append(path(vein, line(GLOW, 2.4, 0.3)))
    out.append(path(vein, line(GLOW, 0.8, 0.95)))

    dome = ("M21.5 60C15 39 29 23 50 23C71 23 85 39 78.5 60"
            "C75.5 51 64 44.5 50 44.5C36 44.5 24.5 51 21.5 60Z")
    out.append(path(dome, 'fill="%s"' % svg.linear(36, 22, 58, 64, [
        (0, LILAC), (0.33, VIOLET), (0.67, PLUM), (1, SHELL)])))
    clip = svg.clip(path(dome, ""))
    folds = []
    for k in range(4):
        y = 58 - k * 8.5
        peak = 31 - k * 6
        d = "M18 %sC26 %s 70 %s 82 %s" % (f(y), f(peak), f(peak - 4), f(y - 3))
        folds.append(path(d, line(PITCH, 2.2, 0.42)))
        folds.append('<g transform="translate(0 1.5)">%s</g>' % path(d, line(LILAC, 0.9, 0.4)))
    folds.append(ellipse(37, 31, 10, 6, paint(WHITE, 0.1)))
    trim = "M23 55Q20.5 40 32 32M77 55Q79.5 40 68 32"
    folds.append(path(trim, line(GLOW, 2, 0.32)))
    folds.append(path(trim, line(GLOW, 0.6, 0.85)))
    out.append('<g clip-path="%s">%s</g>' % (clip, "".join(folds)))

    def silver(d, width):
        out.append(path(d, line(PITCH, width + 1, 0.55)))
        out.append(path(d, line(SILVER_DARK, width)))
        out.append(path(d, line(SILVER, width * 0.62)))
        out.append('<g transform="translate(0 %s)">%s</g>' % (
            f(-width * 0.18), path(d, line(WHITE, width * 0.18, 0.7))))

    silver("M22.4 59.6C25.4 51 36 44.5 50 44.5C64 44.5 74.6 51 77.6 59.6", 3.4)
    silver("M33.5 28.5Q43 31 50 40Q57 31 66.5 28.5", 2.6)
    out.append(glow(svg, 50, 40.5, 9, GLOW, 0.6))
    out.append(circle(50, 40.5, 5.4, 'fill="%s"' % svg.radial(
        48.2, 38.5, 7, [(0, WHITE), (0.5, SILVER), (1, SILVER_DARK)])))
    out.append(circle(50, 40.5, 3.9, paint(PITCH)))
    out.append(circle(50, 40.5, 3, 'fill="%s"' % svg.radial(
        49.2, 39.7, 3.4, [(0, WHITE), (0.5, GLOW), (1, GLOW_DEEP)])))
    out.append(ellipse(49, 39.4, 0.8, 0.7, paint(WHITE, 0.95)))
    out.append(path("M50 46.3L52.6 49.8L50 54L47.4 49.8Z", 'fill="%s"' % svg.linear(
        48, 46, 52, 54, [(0, WHITE), (0.5, SILVER), (1, SILVER_DARK)])))
    out.append(path("M50 48.2L51.2 49.9L50 51.8L48.8 49.9Z", paint(GLOW)))

    lx = max(-1, min(1, gaze[0])) * 1.5
    ly = max(-1, min(1, gaze[1])) * 1.3
    for cx, cy, r in ((43.6, 66.8, 6), (56.4, 66.8, 6), (33.4, 60.4, 4.4),
                      (66.6, 60.4, 4.4), (44.2, 55.6, 3.1), (55.8, 55.6, 3.1)):
        if eye_style == "star":
            out.append(glow(svg, cx, cy, r * 1.9, GOLD, 0.55))
            out.append(path(star_path(cx, cy, r * 1.45, r * 0.66),
                            'fill="%s" stroke="%s" stroke-width="%s" stroke-linejoin="round"' % (
                                svg.linear(cx - r, cy - r, cx + r, cy + r,
                                           [(0, GOLD_LIGHT), (0.5, GOLD), (1, GOLD_DEEP)]),
                                rgb((120, 70, 10)), f(max(r * 0.12, 0.35)))))
            out.append(ellipse(cx - r * 0.3, cy - r * 0.35, r * 0.24, r * 0.18, paint(WHITE, 0.9)))
            continue
        out.append(glow(svg, cx, cy, r * 1.55, GLOW, 0.3))
        out.append(circle(cx, cy, r * 1.13, paint(PITCH)))
        out.append(circle(cx, cy, r, line(GLOW, max(r * 0.11, 0.35))))
        sx, sy = cx + lx * r * 0.16, cy + ly * r * 0.16
        inner = [circle(cx, cy, r, 'fill="%s"' % svg.radial(
            sx, sy + r * 0.45, r * 1.05, [(0, DEEP_SEA), (1, PITCH)]))]
        inner.append(circle(sx, sy + r * 0.05, r * 0.58, paint(PITCH, 0.9)))
        pts = []
        for k in range(25):
            a = math.radians(25 + 130 * k / 24)
            pts.append("%s %s" % (f(sx + math.cos(a) * r * 0.72), f(sy + math.sin(a) * r * 0.72)))
        inner.append(path("M" + " L".join(pts), line(GLOW, r * 0.13, 0.65)))
        inner.append(ellipse(sx - r * 0.3, sy - r * 0.33, r * 0.27, r * 0.23, paint(WHITE, 0.96)))
        inner.append(circle(sx + r * 0.32, sy + r * 0.22, r * 0.11, paint(WHITE, 0.75)))
        eye_clip = svg.clip(circle(cx, cy, r * 0.93, ""))
        out.append('<g clip-path="%s">%s</g>' % (eye_clip, "".join(inner)))

    if mouth == "open":
        out.append(path("M44.5 75.6Q50 75 55.5 75.6Q55 84 50 84.4Q45 84 44.5 75.6Z",
                        'fill="%s" stroke="%s" stroke-width="0.7"' % (rgb(PITCH), rgb(GLOW))))
        out.append(path("M46.5 81.6Q50 79.4 53.5 81.6Q52.5 83.7 50 83.8Q47.5 83.7 46.5 81.6Z",
                        paint((255, 110, 150))))
        out.append(path("M45.6 76.4Q50 77.4 54.4 76.4", line(WHITE, 0.9, 0.9)))
    else:
        out.append(path("M47.4 76.4Q50 %s 52.6 76.4" % f(76.4 + grin), line(GLOW, 0.6, 0.95)))
    legs(True)
    return "".join(out)


def place(content, x, y, size):
    k = size / 100
    return '<g transform="translate(%s %s) scale(%s)">%s</g>' % (f(x), f(y), f(k), content)


def web_ring(cx, cy, r_in, r_out, spokes, color, opacity, width):
    parts = []
    for k in range(spokes):
        a = math.radians(-90 + k * 360 / spokes)
        parts.append("M%s %sL%s %s" % (
            f(cx + math.cos(a) * r_in), f(cy + math.sin(a) * r_in),
            f(cx + math.cos(a) * r_out), f(cy + math.sin(a) * r_out)))
    for t in (0.36, 0.72):
        r = r_in + (r_out - r_in) * t
        sag = (r_out - r_in) * 0.12
        ring = []
        for k in range(spokes + 1):
            a = math.radians(-90 + k * 360 / spokes)
            x, y = cx + math.cos(a) * r, cy + math.sin(a) * r
            if k == 0:
                ring.append("M%s %s" % (f(x), f(y)))
                continue
            am = math.radians(-90 + (k - 0.5) * 360 / spokes)
            qx, qy = cx + math.cos(am) * (r - sag), cy + math.sin(am) * (r - sag)
            ring.append("Q%s %s %s %s" % (f(qx), f(qy), f(x), f(y)))
        parts.append("".join(ring))
    return path("".join(parts), line(color, width, opacity))


def frame(svg, tier, accent):
    spec = TIERS[tier]
    if spec is None:
        metal = (accent["rim_light"], accent["rim"], accent["rim_deep"], PITCH)
        etch, shine, pips = accent["etch"], accent["rim_light"], 0
    else:
        metal, etch, shine, pips = spec["metal"], spec["etch"], spec["shine"], spec["pips"]
    out = []
    out.append(circle(64, 64, 63, 'fill="%s"' % svg.linear(16, 6, 112, 122, [
        (0, metal[0]), (0.35, metal[1]), (0.75, metal[2]), (1, metal[3])])))
    out.append(circle(64, 64, 62.5, line(PITCH, 1, 0.55)))
    out.append(web_ring(64, 64, 50.5, 61, 16, etch, 0.55, 1.1))
    out.append(path("M14 50A51 51 0 0 1 50 14", line(shine, 2.2, 0.55)))
    out.append(circle(64, 64, 61, line(shine, 0.8, 0.35)))
    out.append(circle(64, 64, 51.2, 'fill="%s"' % svg.linear(64, 12, 64, 116, [
        (0, metal[3]), (1, metal[1])])))
    out.append(circle(64, 64, 51.4, line(shine, 0.9, 0.6)))
    if tier == "gold":
        for a in (-135, -45, 45, 135):
            r = math.radians(a)
            gx, gy = 64 + math.cos(r) * 56, 64 + math.sin(r) * 56
            out.append(glow(svg, gx, gy, 5, GOLD_LIGHT, 0.8))
            out.append(path(sparkle_path(gx, gy, 3.6), paint(WHITE, 0.95)))
    if pips:
        spread = 13
        for k in range(pips):
            a = math.radians(90 + (k - (pips - 1) / 2) * spread)
            px, py = 64 + math.cos(a) * 56, 64 + math.sin(a) * 56
            out.append(glow(svg, px, py, 5.5, GLOW, 0.55))
            out.append(path("M%s %sL%s %sL%s %sL%s %sZ" % (
                f(px), f(py - 4), f(px + 3), f(py), f(px), f(py + 4), f(px - 3), f(py)),
                'fill="%s" stroke="%s" stroke-width="0.6"' % (
                    svg.linear(px - 3, py - 4, px + 3, py + 4,
                               [(0, WHITE), (0.45, GLOW), (1, GLOW_DEEP)]),
                    rgb(PITCH))))
    return "".join(out)


def face(svg, sky):
    return circle(64, 64, 49, 'fill="%s"' % svg.radial(
        64, 40, 66, [(0, sky[0]), (0.55, sky[1]), (1, sky[2])]))


def heart_path(cx, cy, w):
    k = w / 16

    def p(x, y):
        return "%s %s" % (f(cx + (x - 8) * k), f(cy + (y - 7.5) * k))
    return ("M%s C%s %s %s C%s %s %s C%s %s %s C%s %s %s C%s %s %s C%s %s %s Z" % (
        p(8, 15), p(8, 15), p(0, 9.5), p(0, 4.5), p(0, 1.5), p(2.5, 0), p(4.5, 0),
        p(6.5, 0), p(8, 1.5), p(8, 3), p(8, 1.5), p(9.5, 0), p(11.5, 0),
        p(13.5, 0), p(16, 1.5), p(16, 4.5), p(16, 9.5), p(8, 15), p(8, 15)))


def sparkles(points, color, opacity=0.95):
    return "".join(path(sparkle_path(x, y, r), paint(color, opacity))
                   for x, y, r in points)


def stargazer(svg, tier):
    accent = {"rim_light": (190, 160, 235), "rim": LILAC, "rim_deep": PLUM,
              "etch": (30, 18, 70)}
    out = [frame(svg, tier, accent),
           face(svg, ((236, 120, 214), (128, 72, 214), (40, 22, 104)))]
    scene = []
    for k in range(12):
        a = math.radians(-90 + k * 30)
        scene.append(path("M64 64L%s %sL%s %sZ" % (
            f(64 + math.cos(a - 0.12) * 70), f(64 + math.sin(a - 0.12) * 70),
            f(64 + math.cos(a + 0.12) * 70), f(64 + math.sin(a + 0.12) * 70)),
            paint(WHITE, 0.07)))
    scene.append(sparkles(((24, 34, 4.2), (104, 30, 5), (20, 80, 3.2),
                           (108, 84, 3.6), (36, 18, 2.6), (92, 16, 2.4)), GOLD_LIGHT))
    scene.append(place(spider(svg, {"bust": True, "eyes": "star",
                                    "mouth": "open", "blush": True}),
                       13, 6, 102))
    clip = svg.clip(circle(64, 64, 49, ""))
    out.append('<g clip-path="%s">%s</g>' % (clip, "".join(scene)))
    out.append(circle(64, 64, 49, line(PITCH, 1.2, 0.6)))
    return out


def genies_gift(svg, tier):
    accent = {"rim_light": GOLD_LIGHT, "rim": GOLD, "rim_deep": GOLD_DEEP,
              "etch": (110, 70, 10)}
    out = [frame(svg, tier, accent),
           face(svg, ((92, 232, 214), (22, 150, 160), (10, 52, 74)))]
    scene = []
    scene.append(sparkles(((26, 40, 3.6), (100, 98, 3), (30, 96, 2.4),
                           (40, 24, 2.2)), WHITE))
    smoke = "M100 68C112 56 100 48 88 52C74 56 70 46 74 40"
    scene.append(path(smoke, line(LILAC, 11, 0.45)))
    scene.append(path(smoke, line((214, 190, 250), 5, 0.9)))
    scene.append(glow(svg, 72, 32, 22, (255, 120, 170), 0.6))
    scene.append(path(heart_path(72, 32, 26), 'fill="%s" stroke="%s" stroke-width="1.4" stroke-linejoin="round"' % (
        svg.linear(60, 20, 84, 46, [(0, (255, 190, 210)), (0.5, (255, 92, 150)), (1, (190, 30, 90))]),
        rgb((130, 20, 60)))))
    scene.append(ellipse(66, 27, 3.2, 2.2, paint(WHITE, 0.75)))
    scene.append(glow(svg, 58, 84, 34, GOLD, 0.45))
    metal = 'fill="%s" stroke="%s" stroke-width="0.7" stroke-linejoin="round"' % (
        svg.linear(0, -22, 0, 2, [(0, GOLD_LIGHT), (0.45, GOLD), (1, GOLD_DEEP)]), rgb((120, 80, 20)))
    lamp = "".join([
        path("M-12 -9Q-21 -11 -18 -4Q-16 -1 -11 -4", line(GOLD_DEEP, 3.2)),
        path("M-12 -9Q-21 -11 -18 -4Q-16 -1 -11 -4", line(GOLD, 2)),
        path("M10 -9Q19 -9 24 -16L25.5 -15Q21 -5 11 -4Z", metal),
        ellipse(0, -1.5, 7, 2.2, metal),
        path("M-12 -3Q-16 -12 0 -13Q16 -12 12 -3Q0 1 -12 -3Z", metal),
        path("M-6 -12.5Q0 -20 6 -12.5Z", metal),
        circle(0, -20.5, 1.8, metal),
        path("M-7 -9Q-3 -11 2 -10.5", line(WHITE, 1, 0.8)),
        circle(0, -8, 1.7, 'fill="%s"' % svg.radial(
            -0.4, -8.4, 1.9, [(0, WHITE), (0.5, GLOW), (1, GLOW_DEEP)])),
    ])
    scene.append(ellipse(62, 104, 30, 4, paint(PITCH, 0.35)))
    scene.append('<g transform="translate(60 101) scale(1.85)">%s</g>' % lamp)
    clip = svg.clip(circle(64, 64, 49, ""))
    out.append('<g clip-path="%s">%s</g>' % (clip, "".join(scene)))
    out.append(circle(64, 64, 49, line(PITCH, 1.2, 0.6)))
    return out


MEDALLIONS = {
    "stargazer": ("Stargazer", stargazer, ("default", "bronze", "silver", "gold")),
    "genies_gift": ("Genie's Gift", genies_gift, ("default",)),
}


def build(slug, tier):
    label, draw, _ = MEDALLIONS[slug]
    prefix = slug.replace("_", "") + ("" if tier == "default" else tier[0])
    svg = Svg(prefix)
    for part in draw(svg, tier):
        svg.add(part)
    title = label if tier == "default" else "%s %s" % (tier.capitalize(), label)
    return svg.render(title.replace("'", "&#39;"))


def file_name(slug, tier):
    return slug + (".svg" if tier == "default" else "-%s.svg" % tier)


def main(argv):
    check = "--check" in argv
    stale = []
    for slug, (_, _, tiers) in MEDALLIONS.items():
        for tier in tiers:
            target = OUT / file_name(slug, tier)
            text = build(slug, tier)
            if check:
                if not target.exists() or target.read_text(encoding="utf-8") != text:
                    stale.append(target.name)
                continue
            target.write_text(text, encoding="utf-8")
    if stale:
        print("stale medallions: " + ", ".join(stale))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
