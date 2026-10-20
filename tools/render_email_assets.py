#!/usr/bin/env python3
import colorsys
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "www" / "public" / "assets"
OUT = ASSETS / "email"

PAGE = (11, 11, 12)
PATTERN = (58, 58, 64)
ACCENT = (88, 166, 255)

FLOW_SCREENS = ("prompt", "models", "start-agent", "inbox", "diff", "review")


def shared_palette(frames, colors):
    sheet = Image.new("RGB", (frames[0].width, frames[0].height * len(frames)))
    for i, frame in enumerate(frames):
        sheet.paste(frame.convert("RGB"), (0, i * frames[0].height))
    return sheet.quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)


def save_gif(path, frames, colors, durations):
    palette = shared_palette(frames, colors)
    out = [frame.convert("RGB").quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]
    out[0].save(path, save_all=True, append_images=out[1:], duration=durations, loop=0,
                optimize=True, disposal=1)


def font(path, size):
    return ImageFont.truetype(str(path), size)


def rounded_mask(size, radius):
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    return mask


def phone(screen, width):
    pad = max(4, round(width * 0.03))
    inner_w = width - 2 * pad
    inner_h = round(inner_w * 1391 / 640)
    height = inner_h + 2 * pad
    body = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(body)
    draw.rounded_rectangle((0, 0, width - 1, height - 1), round(width * 0.175), fill=(26, 26, 28, 255))
    draw.rounded_rectangle((1, 1, width - 2, height - 2), round(width * 0.17),
                           outline=(74, 74, 76, 255), width=max(1, width // 160))
    shot = screen.convert("RGB").resize((inner_w, inner_h), Image.LANCZOS)
    body.paste(shot, (pad, pad), rounded_mask((inner_w, inner_h), round(width * 0.145)))
    return body


def hero():
    w, h = 1200, 720
    canvas = Image.new("RGBA", (w, h), PAGE + (255,))
    lx0, ly0, lw = 250, 70, 860
    shot = Image.open(ASSETS / "desktop-mockup" / "desktop-app.webp").convert("RGB")
    sw = lw - 24
    sh = round(sw * shot.height / shot.width)
    lh = sh + 24
    draw = ImageDraw.Draw(canvas)
    draw.rounded_rectangle((lx0, ly0, lx0 + lw, ly0 + lh), 22, fill=(32, 34, 37, 255),
                           outline=(64, 66, 72, 255), width=2)
    canvas.paste(shot.resize((sw, sh), Image.LANCZOS), (lx0 + 12, ly0 + 12),
                 rounded_mask((sw, sh), 10))
    notch_w = 120
    draw.rounded_rectangle((lx0 + lw / 2 - notch_w / 2, ly0 + 12, lx0 + lw / 2 + notch_w / 2, ly0 + 32),
                           8, fill=(0, 0, 0, 255))
    base_y = ly0 + lh
    draw.rounded_rectangle((lx0 - 26, base_y - 2, lx0 + lw + 26, base_y + 18), 10, fill=(52, 54, 59, 255))
    draw.rounded_rectangle((lx0 + lw / 2 - 60, base_y - 2, lx0 + lw / 2 + 60, base_y + 7), 4,
                           fill=(22, 23, 25, 255))
    device = phone(Image.open(ASSETS / "mobile" / "start-agent.webp"), 250)
    px, py = 90, h - device.height - 40
    canvas.alpha_composite(device, (px, py))
    x0, y0, x1, y1 = canvas.convert("RGB").point(lambda v: 255 if v > 14 else 0).getbbox()
    canvas = canvas.crop((max(0, x0 - 8), max(0, y0 - 8), min(w, x1 + 8), min(h, y1 + 8)))
    canvas.convert("RGB").save(OUT / "deck-cover.jpg", quality=88, optimize=True, progressive=True,
                               subsampling=0)


def agent_flow():
    pw = 270
    screens = [phone(Image.open(ASSETS / "mobile" / (name + ".webp")), pw) for name in FLOW_SCREENS]
    w = 480
    top = 24
    h = top + screens[0].height + 64
    base = Image.new("RGBA", (w, h), PAGE + (255,))
    frames = []
    for index, device in enumerate(screens):
        frame = base.copy()
        frame.alpha_composite(device, ((w - pw) // 2, top))
        draw = ImageDraw.Draw(frame)
        dots_y = top + device.height + 32
        for d in range(len(screens)):
            x = w / 2 + (d - 2.5) * 22
            color = ACCENT if d == index else PATTERN
            draw.ellipse((x - 5, dots_y - 5, x + 5, dots_y + 5), fill=color + (255,))
        frames.append(frame.convert("RGB"))
    save_gif(OUT / "deck-flow.gif", frames, 128, 2100)


LENS_LEFT = ((263, 62), (212, 52), (120, 48), (74, 56), (46, 61), (38, 94), (47, 132),
             (60, 188), (102, 228), (157, 228), (204, 228), (238, 200), (250, 164),
             (260, 132), (268, 90), (263, 62))
SANS = ASSETS / "fonts" / "Lato-Regular.woff2"
SANS_BOLD = ASSETS / "fonts" / "Lato-Bold.woff2"
CODE = ASSETS / "fonts" / "DejaVuSansMono.woff2"
WATCH_ROWS = (
    ("Cl", 24, "Fix flaky deploy test", "forkmesh/app", "Running · 14t", (48, 209, 88), 0.62),
    ("Co", 160, "SSO audit export", "forkmesh/server", "Waiting", (255, 214, 10), 0.34),
    ("Cl", 265, "Mirror failover", "forkmesh/world", "Merged", (142, 142, 147), 0.88),
)
AGENT_LINES = (
    ("$ pytest app/tests -k deploy", (110, 118, 129)),
    ("✓ 48 passed in 3.1s", (201, 209, 217)),
    ("- await sleep(2)", (248, 81, 73)),
    ("+ await wait_for(ready, timeout=10)", (63, 185, 80)),
    ("Opening a signed pull request…", (110, 118, 129)),
)


def bezier(points, steps=24):
    out = [points[0]]
    for i in range(0, len(points) - 1, 3):
        p0, p1, p2, p3 = points[i:i + 4]
        for s in range(1, steps + 1):
            t = s / steps
            u = 1 - t
            out.append(tuple(u ** 3 * p0[c] + 3 * u * u * t * p1[c] + 3 * u * t * t * p2[c]
                             + t ** 3 * p3[c] for c in range(2)))
    return out


def lens_polygons(scale, ox, oy):
    left = [(ox + x * scale, oy + y * scale) for x, y in bezier(LENS_LEFT)]
    right = [(ox + (600 - x) * scale, oy + y * scale) for x, y in bezier(LENS_LEFT)]
    return left, right


def vertical_gradient(size, stops):
    w, h = size
    column = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        for (a, ca), (b, cb) in zip(stops, stops[1:]):
            if a <= t <= b:
                k = (t - a) / (b - a) if b > a else 0
                column.putpixel((0, y), tuple(round(ca[c] + (cb[c] - ca[c]) * k) for c in range(4)))
                break
    return column.resize((w, h))


def watch_face(ww):
    pad = round(ww * 0.055)
    strap_h = round(ww * 0.42)
    case_h = round(ww * 1.2)
    w, h = ww + round(ww * 0.08), case_h + 2 * strap_h
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    sx0, sx1 = round(ww * 0.14), round(ww * 0.86)
    strap = vertical_gradient((sx1 - sx0, strap_h), ((0, (30, 31, 35, 0)), (0.7, (30, 31, 35, 255)),
                                                     (1, (30, 31, 35, 255))))
    layer.alpha_composite(strap, (sx0, 0))
    layer.alpha_composite(strap.transpose(Image.FLIP_TOP_BOTTOM), (sx0, strap_h + case_h))
    cy = strap_h
    draw.rounded_rectangle((ww - 2, cy + round(case_h * 0.24), ww + round(ww * 0.05),
                            cy + round(case_h * 0.24) + round(ww * 0.17)), 4, fill=(96, 99, 106, 255))
    draw.rounded_rectangle((0, cy, ww - 1, cy + case_h - 1), round(ww * 0.27), fill=(24, 25, 28, 255),
                           outline=(86, 89, 96, 255), width=max(2, ww // 70))
    sx, sy = pad, cy + pad
    sw, sh = ww - 2 * pad, case_h - 2 * pad
    draw.rounded_rectangle((sx, sy, sx + sw, sy + sh), round(ww * 0.22), fill=(0, 0, 0, 255))
    unit = ww * 0.062
    title = font(SANS_BOLD, round(unit * 1.45))
    bold = font(SANS_BOLD, round(unit))
    small = font(SANS, round(unit * 0.85))
    ix = sx + round(ww * 0.065)
    y = sy + round(ww * 0.07)
    draw.text((ix, y), "Agents", font=title, fill=(88, 166, 255))
    clock = "9:41"
    draw.text((sx + sw - round(ww * 0.065) - draw.textlength(clock, font=bold), y + unit * 0.4), clock,
              font=bold, fill=(255, 255, 255))
    y += round(unit * 2.3)
    row_h = round(unit * 3.85)
    for face, hue, name, repo, status, color, share in WATCH_ROWS:
        x0, x1 = ix - round(unit * 0.4), sx + sw - round(ww * 0.065) + round(unit * 0.4)
        draw.rounded_rectangle((x0, y, x1, y + row_h), round(unit * 0.9), fill=(28, 28, 30, 255))
        r = unit * 0.95
        fx, fy = x0 + unit * 0.55 + r, y + row_h / 2
        rgb = tuple(round(v * 255) for v in colorsys.hls_to_rgb(hue / 360, 0.72, 0.55))
        draw.ellipse((fx - r, fy - r, fx + r, fy + r), fill=rgb + (255,))
        tw = draw.textlength(face, font=small)
        draw.text((fx - tw / 2, fy - unit * 0.5), face, font=small, fill=(20, 20, 20))
        tx = fx + r + unit * 0.5
        limit = x1 - unit * 2.4 - tx
        text = name
        while draw.textlength(text + "…", font=bold) > limit and len(text) > 3:
            text = text[:-1]
        draw.text((tx, y + unit * 0.45), text if text == name else text + "…", font=bold,
                  fill=(255, 255, 255))
        draw.text((tx, y + unit * 1.55), repo, font=small, fill=(142, 142, 147))
        dot = unit * 0.24
        dy = y + unit * 2.85
        draw.ellipse((tx, dy - dot, tx + 2 * dot, dy + dot), fill=color + (255,))
        draw.text((tx + unit * 0.7, y + unit * 2.4), status, font=small, fill=(235, 235, 240))
        gr = unit * 0.8
        gx, gy = x1 - unit * 0.55 - gr, y + row_h / 2
        width = max(2, round(unit * 0.32))
        draw.arc((gx - gr, gy - gr, gx + gr, gy + gr), 0, 360, fill=(58, 58, 60), width=width)
        draw.arc((gx - gr, gy - gr, gx + gr, gy + gr), -90, -90 + 360 * share, fill=(10, 132, 255),
                 width=width)
        y += row_h + round(unit * 0.4)
    return layer


def xr():
    k = 2
    w, h = 1072 * k, 430 * k
    bg = (11, 11, 12)
    scale = 1.36 * k
    ox, oy = 22 * k, 16 * k
    canvas = Image.new("RGBA", (w, h), bg + (255,))
    world = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    world.alpha_composite(vertical_gradient((w, h), ((0, (14, 30, 50, 255)), (0.55, (12, 24, 40, 255)),
                                                      (1, (8, 14, 24, 255)))))
    draw = ImageDraw.Draw(world)
    horizon = oy + 190 * scale
    for i in range(14):
        y = horizon + (i ** 1.7) * 2.2 * k
        draw.line((0, y, w, y), fill=(88, 166, 255, 60), width=k)
    for i in range(-16, 17):
        x = ox + 300 * scale + i * 46 * k
        draw.line((ox + 300 * scale + i * 10 * k, horizon, x, h), fill=(88, 166, 255, 46), width=k)

    def pane(box, radius):
        x0, y0, x1, y1 = box
        glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(glow).rounded_rectangle(box, radius, outline=(88, 166, 255, 150), width=10 * k)
        world.alpha_composite(glow.filter(ImageFilter.GaussianBlur(12 * k)))
        ImageDraw.Draw(world).rounded_rectangle(box, radius, fill=(20, 22, 27, 240),
                                                outline=(88, 166, 255, 150), width=k)

    shot = Image.open(ASSETS / "desktop-mockup" / "desktop-app.webp").convert("RGB")
    dx0, dy0 = ox + 66 * scale, oy + 84 * scale
    dw = round(200 * scale)
    dh = round(dw * shot.height / shot.width)
    pane((dx0 - 2 * k, dy0 - 2 * k, dx0 + dw + 2 * k, dy0 + dh + 2 * k), 6 * k)
    world.paste(shot.resize((dw, dh), Image.LANCZOS), (round(dx0), round(dy0)),
                rounded_mask((dw, dh), 4 * k))
    ax0, ay0 = ox + 352 * scale, oy + 82 * scale
    ax1 = ox + 538 * scale
    line_h = 13 * k
    ay1 = ay0 + 20 * k + line_h * len(AGENT_LINES) + 44 * k
    pane((ax0, ay0, ax1, ay1), 6 * k)
    draw = ImageDraw.Draw(world)
    head = font(SANS_BOLD, 12 * k)
    code = font(CODE, 10 * k)
    tag = font(SANS, 10 * k)
    px, py = ax0 + 12 * k, ay0 + 12 * k
    draw.ellipse((px, py + 3 * k, px + 7 * k, py + 10 * k), fill=(63, 185, 80, 255))
    draw.text((px + 12 * k, py), "Claude Code", font=head, fill=(255, 255, 255))
    branch = "fix/flaky-deploy"
    draw.text((ax1 - 12 * k - draw.textlength(branch, font=tag), py + k), branch, font=tag,
              fill=(139, 148, 158))
    py += 22 * k
    for text, color in AGENT_LINES:
        draw.text((px, py), text, font=code, fill=color)
        py += line_h
    py += 8 * k
    draw.rounded_rectangle((px, py, ax1 - 12 * k, py + 24 * k), 5 * k, fill=(31, 52, 78, 255))
    draw.text((px + 8 * k, py + 6 * k), "PR #482 · +128 −14", font=head, fill=(255, 255, 255))
    review = "Review"
    rw = draw.textlength(review, font=head) + 14 * k
    draw.rounded_rectangle((ax1 - 16 * k - rw, py + 4 * k, ax1 - 16 * k, py + 20 * k), 4 * k,
                           fill=(35, 134, 54, 255))
    draw.text((ax1 - 16 * k - rw + 7 * k, py + 5 * k), review, font=head, fill=(255, 255, 255))

    left, right = lens_polygons(scale, ox, oy)
    mask = Image.new("L", (w, h), 0)
    mdraw = ImageDraw.Draw(mask)
    mdraw.polygon(left, fill=255)
    mdraw.polygon(right, fill=255)
    ghost = world.filter(ImageFilter.GaussianBlur(34 * k))
    ghost.putalpha(ghost.getchannel("A").point(lambda v: v * 0.45))
    canvas.alpha_composite(ghost)
    inside = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    inside.paste(world, (0, 0), mask)
    canvas.alpha_composite(inside)
    top, bottom = round(oy + 48 * scale), round(oy + 228 * scale)
    tint = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    tint.paste(vertical_gradient((w, bottom - top), ((0, (27, 37, 51, 140)), (0.55, (88, 166, 255, 16)),
                                                      (1, (27, 37, 51, 100)))), (0, top))
    sheen = Image.new("L", (w, h), 0)
    for shift in (0, 284):
        ImageDraw.Draw(sheen).polygon(
            ((ox + (120 + shift) * scale, oy + 40 * scale), (ox + (170 + shift) * scale, oy + 40 * scale),
             (ox + (90 + shift) * scale, oy + 240 * scale), (ox + (40 + shift) * scale, oy + 240 * scale)),
            fill=34)
    tint.alpha_composite(Image.merge("RGBA", (Image.new("L", (w, h), 255),) * 3 + (sheen,)))
    lensed = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    lensed.paste(tint, (0, 0), mask)
    canvas.alpha_composite(lensed)

    frame = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(frame)
    for points in (left, right):
        draw.line(points + [points[0]], fill=(185, 201, 220, 130), width=2 * k, joint="curve")

    def pt(x, y):
        return ox + x * scale, oy + y * scale

    for cx, angle in ((280, -18), (320, 18)):
        pad = Image.new("RGBA", (round(20 * scale), round(36 * scale)), (0, 0, 0, 0))
        ImageDraw.Draw(pad).ellipse((2 * k, 2 * k, pad.width - 2 * k, pad.height - 2 * k),
                                    fill=(14, 16, 19, 255))
        pad = pad.rotate(-angle, resample=Image.BICUBIC, expand=True)
        px, py = pt(cx, 176)
        canvas.alpha_composite(pad, (round(px - pad.width / 2), round(py - pad.height / 2)))
    draw.line((pt(272, 110), pt(279, 160)), fill=(141, 147, 156, 255), width=round(2 * scale))
    draw.line((pt(328, 110), pt(321, 160)), fill=(141, 147, 156, 255), width=round(2 * scale))
    bar = [pt(x, y) for x, y in bezier(((58, 52), (180, 44), (420, 44), (542, 52)))]
    draw.line([(x, y + 2 * k) for x, y in bar], fill=(10, 11, 13, 160), width=round(5 * scale),
              joint="curve")
    draw.line(bar, fill=(176, 182, 191, 255), width=round(3.6 * scale), joint="curve")
    draw.line([(x, y - 1.1 * scale) for x, y in bar], fill=(238, 240, 243, 255),
              width=max(k, round(1.1 * scale)), joint="curve")
    bridge = [pt(x, y) for x, y in bezier(((262, 88), (286, 80), (314, 80), (338, 88)))]
    draw.line(bridge, fill=(169, 175, 184, 255), width=round(7 * scale), joint="curve")
    draw.line([(x, y - 2 * scale) for x, y in bridge], fill=(238, 240, 243, 255),
              width=round(2 * scale), joint="curve")
    for hx in (26, 544):
        x0, y0 = pt(hx, 46)
        x1, y1 = pt(hx + 30, 82)
        draw.rounded_rectangle((x0, y0, x1, y1), round(6 * scale), fill=(40, 43, 49, 255))
        draw.rounded_rectangle((x0 + 3 * k, y0 + 3 * k, x1 - 3 * k, y0 + 7 * k), 2 * k,
                               fill=(255, 255, 255, 46))
    canvas.alpha_composite(frame)

    device = watch_face(140 * k).rotate(-8, resample=Image.BICUBIC, expand=True)
    wx, wy = w - device.width - 16 * k, h - device.height + 34 * k
    shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shade).ellipse((wx + 20 * k, wy + device.height * 0.3, wx + device.width,
                                   wy + device.height * 0.75), fill=(0, 0, 0, 200))
    canvas.alpha_composite(shade.filter(ImageFilter.GaussianBlur(24 * k)))
    canvas.alpha_composite(device, (wx, wy))
    out = canvas.convert("RGB").resize((w // k, h // k), Image.LANCZOS)
    out.save(OUT / "xr.jpg", quality=88, optimize=True, progressive=True)


def avatars():
    for name in ("jett", "kaif"):
        image = Image.open(ASSETS / (name + ".webp")).convert("RGBA").resize((144, 144), Image.LANCZOS)
        out = Image.new("RGBA", (144, 144), (0, 0, 0, 0))
        out.paste(image, (0, 0), rounded_mask((144, 144), 34))
        out.save(OUT / (name + ".png"), optimize=True)


def slab_edges():
    k = 4
    w, depth, shoulder, radius = 1200 * k, 40 * k, 168 * k, 48 * k
    mask = Image.new("L", (w, depth), 0)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle((0, 0, w - 1, depth + radius * 2), radius, fill=255)
    draw.polygon(((shoulder, 0), (w - shoulder, 0), (w - shoulder - depth, depth),
                  (shoulder + depth, depth)), fill=0)
    mask = mask.resize((w // k, depth // k), Image.LANCZOS)
    top = Image.new("RGBA", mask.size, PAGE + (0,))
    top.putalpha(mask)
    top.save(OUT / "slab-top.png", optimize=True)
    top.transpose(Image.FLIP_TOP_BOTTOM).save(OUT / "slab-bottom.png", optimize=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    slab_edges()
    hero()
    agent_flow()
    xr()
    avatars()
    for path in sorted(OUT.iterdir()):
        print("%-16s %7.1f KB" % (path.name, path.stat().st_size / 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
