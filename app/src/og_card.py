"""Social-preview (OpenGraph) card renderer for repo and referral pages.

Pure (builtin-only, no ``js``/``workers`` runtime, no ``env``/DB access): a
Mastodon/Slack/Twitter unfurl of a repo URL used to show only the ForkMesh
logo (issue adhoc #46); now it gets a 1200x630 PNG info card - repo name,
description, and the catalog stats - with the logo small in the top-right.

Everything is rendered in-process with the stdlib (Pyodide workers have no
Pillow): pixels go into a flat RGB bytearray, text comes from an embedded
classic 5x7 bitmap font scaled up per use, the logo is a pre-scaled RGBA
blob baked in at authoring time (tools/embed_og_logo.py regenerates it from
www/public/assets/logo.png), and the PNG is assembled by hand with zlib. Drawing
sticks to bytearray row-slice writes so a card costs only a few thousand
Python-level ops - cheap enough for a worker request, and each colo renders
a given repo's card at most once per edge-cache TTL anyway.

Colors mirror the dashboard's dark theme variables in
public/dashboard/repo.html so the card looks like the page it links to.
"""

import base64
import struct
import zlib
from datetime import datetime, timezone

CARD_W = 1200
CARD_H = 630

_BG = (1, 4, 9)            # --background
_CARD = (13, 17, 23)       # --card
_BORDER = (48, 54, 61)     # --dashboard-border-rgb
_FG = (240, 246, 252)      # --foreground
_TEXT = (201, 209, 217)    # --dashboard-secondary-foreground-rgb
_MUTED = (139, 148, 158)   # --dashboard-muted-foreground-rgb
_ACCENT = (88, 166, 255)   # --dashboard-accent-rgb

# Classic 5x7 ASCII font (chars 32..126), 5 column bytes per glyph, bit 0 =
# top row. The old Adafruit/HD44780 table - public domain and legible even at
# small scales.
_FONT = bytes.fromhex(
    "0000000000"  # ' '
    "00005f0000"  # '!'
    "0007000700"  # '"'
    "147f147f14"  # '#'
    "242a7f2a12"  # '$'
    "2313086462"  # '%'
    "3649552250"  # '&'
    "0005030000"  # '''
    "001c224100"  # '('
    "0041221c00"  # ')'
    "082a1c2a08"  # '*'
    "08083e0808"  # '+'
    "0050300000"  # ','
    "0808080808"  # '-'
    "0060600000"  # '.'
    "2010080402"  # '/'
    "3e5149453e"  # '0'
    "00427f4000"  # '1'
    "4261514946"  # '2'
    "2141454b31"  # '3'
    "1814127f10"  # '4'
    "2745454539"  # '5'
    "3c4a494930"  # '6'
    "0171090503"  # '7'
    "3649494936"  # '8'
    "064949291e"  # '9'
    "0036360000"  # ':'
    "0056360000"  # ';'
    "0008142241"  # '<'
    "1414141414"  # '='
    "4122140800"  # '>'
    "0201510906"  # '?'
    "324979413e"  # '@'
    "7e1111117e"  # 'A'
    "7f49494936"  # 'B'
    "3e41414122"  # 'C'
    "7f4141221c"  # 'D'
    "7f49494941"  # 'E'
    "7f09090901"  # 'F'
    "3e4149497a"  # 'G'
    "7f0808087f"  # 'H'
    "00417f4100"  # 'I'
    "2040413f01"  # 'J'
    "7f08142241"  # 'K'
    "7f40404040"  # 'L'
    "7f020c027f"  # 'M'
    "7f0408107f"  # 'N'
    "3e4141413e"  # 'O'
    "7f09090906"  # 'P'
    "3e4151215e"  # 'Q'
    "7f09192946"  # 'R'
    "4649494931"  # 'S'
    "01017f0101"  # 'T'
    "3f4040403f"  # 'U'
    "1f2040201f"  # 'V'
    "3f4038403f"  # 'W'
    "6314081463"  # 'X'
    "0708700807"  # 'Y'
    "6151494543"  # 'Z'
    "007f414100"  # '['
    "0204081020"  # '\'
    "0041417f00"  # ']'
    "0402010204"  # '^'
    "4040404040"  # '_'
    "0001020400"  # '`'
    "2054545478"  # 'a'
    "7f48444438"  # 'b'
    "3844444420"  # 'c'
    "384444487f"  # 'd'
    "3854545418"  # 'e'
    "087e090102"  # 'f'
    "0c5252523e"  # 'g'
    "7f08040478"  # 'h'
    "00447d4000"  # 'i'
    "2040443d00"  # 'j'
    "7f10284400"  # 'k'
    "00417f4000"  # 'l'
    "7c04180478"  # 'm'
    "7c08040478"  # 'n'
    "3844444438"  # 'o'
    "7c14141408"  # 'p'
    "081414187c"  # 'q'
    "7c08040408"  # 'r'
    "4854545420"  # 's'
    "043f444020"  # 't'
    "3c4040207c"  # 'u'
    "1c2040201c"  # 'v'
    "3c4030403c"  # 'w'
    "4428102844"  # 'x'
    "0c5050503c"  # 'y'
    "4464544c44"  # 'z'
    "0008364100"  # '{'
    "00007f0000"  # '|'
    "0041360800"  # '}'
    "0201020402"  # '~'
)

# ForkMesh mark, pre-scaled to 98x112 raw RGBA (zlib+base64). Regenerate with
# tools/embed_og_logo.py whenever www/public/assets/logo.png changes.
_LOGO_W = 98
_LOGO_H = 112
_LOGO_B64 = (
    "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"
    "yXfo/5R1re0="
)


def _logo_rgba():
    return zlib.decompress(base64.b64decode(_LOGO_B64))


class _Canvas:
    """Flat RGB byte canvas with just the ops the card needs."""

    def __init__(self, w, h, color):
        self.w = w
        self.h = h
        self.buf = bytearray(bytes(color) * (w * h))

    def fill_rect(self, x, y, w, h, color):
        x0 = max(0, x)
        y0 = max(0, y)
        x1 = min(self.w, x + w)
        y1 = min(self.h, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(color) * (x1 - x0)
        stride = self.w * 3
        for yy in range(y0, y1):
            off = yy * stride + x0 * 3
            self.buf[off:off + len(row)] = row

    def frame_rect(self, x, y, w, h, thickness, color):
        self.fill_rect(x, y, w, thickness, color)
        self.fill_rect(x, y + h - thickness, w, thickness, color)
        self.fill_rect(x, y, thickness, h, color)
        self.fill_rect(x + w - thickness, y, thickness, h, color)

    def text(self, x, y, s, scale, color):
        """Draw s at (x, y) top-left, each font pixel a scale x scale block."""
        pen = x
        for ch in s:
            code = ord(ch)
            if not 32 <= code <= 126:
                code = 63  # '?'
            base = (code - 32) * 5
            for col in range(5):
                bits = _FONT[base + col]
                if not bits:
                    continue
                for row in range(7):
                    if bits & (1 << row):
                        self.fill_rect(pen + col * scale, y + row * scale,
                                       scale, scale, color)
            pen += 6 * scale
        return pen

    def blit_rgba(self, x, y, w, h, rgba):
        """Alpha-blend a raw RGBA blob onto the canvas."""
        stride = self.w * 3
        buf = self.buf
        for yy in range(h):
            ty = y + yy
            if ty < 0 or ty >= self.h:
                continue
            src = yy * w * 4
            dst = ty * stride + x * 3
            for xx in range(w):
                a = rgba[src + 3]
                if a == 255:
                    buf[dst:dst + 3] = rgba[src:src + 3]
                elif a:
                    inv = 255 - a
                    buf[dst] = (rgba[src] * a + buf[dst] * inv) // 255
                    buf[dst + 1] = (rgba[src + 1] * a + buf[dst + 1] * inv) // 255
                    buf[dst + 2] = (rgba[src + 2] * a + buf[dst + 2] * inv) // 255
                src += 4
                dst += 3

    def png(self):
        stride = self.w * 3
        raw = bytearray()
        for yy in range(self.h):
            raw.append(0)  # filter: None
            raw += self.buf[yy * stride:(yy + 1) * stride]
        out = bytearray(b"\x89PNG\r\n\x1a\n")
        for tag, body in (
                (b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)),
                (b"IDAT", zlib.compress(bytes(raw), 6)),
                (b"IEND", b"")):
            out += struct.pack(">I", len(body)) + tag + body
            out += struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
        return bytes(out)


def text_width(s, scale):
    return max(0, len(s) * 6 * scale - scale)


def _fit(s, scale, max_w):
    """Truncate s with an ellipsis so it fits max_w at scale."""
    if text_width(s, scale) <= max_w:
        return s
    while s and text_width(s + "...", scale) > max_w:
        s = s[:-1]
    return s + "..."


def _wrap(s, scale, max_w, max_lines):
    words = s.split()
    lines = []
    cur = ""
    for word in words:
        cand = (cur + " " + word) if cur else word
        if cur and text_width(cand, scale) > max_w:
            lines.append(cur)
            cur = word
        else:
            cur = cand
    if cur:
        lines.append(cur)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] += "..."
    return [_fit(line, scale, max_w) for line in lines]


def format_count(value):
    """Catalog counts arrive as strings ('', '12'); render compactly."""
    try:
        n = int(str(value).strip() or 0)
    except (TypeError, ValueError):
        n = 0
    if n >= 1000000:
        text = "%.1fM" % (n / 1000000.0)
    elif n >= 1000:
        text = "%.1fk" % (n / 1000.0)
    else:
        return str(n)
    return text.replace(".0", "")


def format_exact(value):
    """Exact, grouped count - the referral card's whole point is the number,
    so 1,284 clicks must not collapse to the same '1.3k' as 1,285."""
    try:
        n = int(str(value).strip() or 0)
    except (TypeError, ValueError):
        n = 0
    return "{:,}".format(n)


def format_size(size_bytes):
    try:
        n = int(size_bytes or 0)
    except (TypeError, ValueError):
        n = 0
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            if unit == "B":
                return "%d %s" % (n, unit)
            return ("%.1f %s" % (n, unit)).replace(".0 ", " ")
        n /= 1024.0
    return "0 B"


def _ts_seconds(value):
    """Catalog updatedAt may be epoch ms, epoch s, or an ISO string."""
    text = str(value or "").strip()
    if not text:
        return None
    try:
        n = int(float(text))
        if n > 10 ** 11:  # milliseconds
            return n // 1000
        if n > 10 ** 8:
            return n
        return None
    except ValueError:
        pass
    try:
        stamp = datetime.fromisoformat(text.replace("Z", "+00:00"))
        if stamp.tzinfo is None:
            stamp = stamp.replace(tzinfo=timezone.utc)
        return int(stamp.timestamp())
    except ValueError:
        return None


def format_age(value, now_s):
    ts = _ts_seconds(value)
    if ts is None:
        return "-"
    delta = max(0, int(now_s) - ts)
    if delta < 60:
        return "now"
    if delta < 3600:
        return "%dm ago" % (delta // 60)
    if delta < 86400:
        return "%dh ago" % (delta // 3600)
    if delta < 30 * 86400:
        return "%dd ago" % (delta // 86400)
    if delta < 365 * 86400:
        return "%dmo ago" % (delta // (30 * 86400))
    return "%dy ago" % (delta // (365 * 86400))


def render_repo_card(info, now_s):
    """Render the 1200x630 OpenGraph PNG for a repo.

    info keys (all optional except owner/repo): owner, repo, description,
    branch, host, stars, mirrors, issues, pulls, commits, branches,
    sizeBytes, updatedAt, followers.
    """
    owner = str(info.get("owner") or "")
    repo = str(info.get("repo") or "")
    canvas = _Canvas(CARD_W, CARD_H, _BG)

    # Card panel.
    margin = 28
    canvas.fill_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                     _CARD)
    canvas.frame_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                      2, _BORDER)

    pad = 72
    logo_x = CARD_W - pad - _LOGO_W
    canvas.blit_rgba(logo_x, margin + 40, _LOGO_W, _LOGO_H, _logo_rgba())

    # Title: owner/repo, shrunk (then ellipsized) to clear the logo.
    title = "%s/%s" % (owner, repo)
    title_max_w = logo_x - pad - 40
    scale = 6
    while scale > 3 and text_width(title, scale) > title_max_w:
        scale -= 1
    title = _fit(title, scale, title_max_w)
    canvas.text(pad, 96, title, scale, _FG)

    # Description under the title (two lines max).
    description = str(info.get("description") or "").strip()
    if not description:
        description = "A repository mirrored across the ForkMesh network."
    y = 96 + 7 * scale + 26
    for line in _wrap(description, 3, title_max_w, 2):
        canvas.text(pad, y, line, 3, _TEXT)
        y += 7 * 3 + 12

    # Divider.
    canvas.fill_rect(pad, 268, CARD_W - 2 * pad, 2, _BORDER)

    # Stats grid: 2 rows x 4 columns of value + label.
    cells = (
        ("STARS", format_count(info.get("stars"))),
        ("ISSUES", format_count(info.get("issues"))),
        ("PULL REQUESTS", format_count(info.get("pulls"))),
        ("MIRRORS", format_count(info.get("mirrors"))),
        ("COMMITS", format_count(info.get("commits"))),
        ("BRANCHES", format_count(info.get("branches"))),
        ("SIZE", format_size(info.get("sizeBytes"))),
        ("UPDATED", format_age(info.get("updatedAt"), now_s)),
    )
    grid_x = pad
    grid_w = CARD_W - 2 * pad
    col_w = grid_w // 4
    for index, (label, value) in enumerate(cells):
        cx = grid_x + (index % 4) * col_w
        cy = 306 + (index // 4) * 110
        value_scale = 5
        while value_scale > 2 and text_width(value, value_scale) > col_w - 24:
            value_scale -= 1
        canvas.text(cx, cy, value, value_scale, _FG)
        canvas.text(cx, cy + 7 * 5 + 12, label, 2, _MUTED)

    # Footer: canonical URL left, branch + followers right.
    footer_y = CARD_H - margin - 48
    host = str(info.get("host") or "forkmesh.com")
    url = _fit("%s/%s/%s" % (host, owner, repo), 3, grid_w - 360)
    canvas.text(pad, footer_y, url, 3, _ACCENT)
    right_bits = []
    branch = str(info.get("branch") or "").strip()
    if branch:
        right_bits.append(_fit(branch, 2, 200))
    try:
        followers = int(info.get("followers") or 0)
    except (TypeError, ValueError):
        followers = 0
    if followers > 0:
        right_bits.append("%s follower%s" % (
            format_count(followers), "" if followers == 1 else "s"))
    right = "  |  ".join(right_bits)
    if right:
        canvas.text(CARD_W - pad - text_width(right, 2), footer_y + 4,
                    right, 2, _MUTED)
    return canvas.png()


def render_referral_card(info, now_s):
    """Render the 1200x630 OpenGraph PNG for a /r/<name> share link.

    The counters are the live values read at request time, so a Mastodon (or
    Slack/Discord/Twitter) unfurl of the share link shows how far it has
    actually travelled instead of the generic signup-page preview.

    info keys: name (required), host, clicks, signups, lastTs.
    """
    name = str(info.get("name") or "")
    canvas = _Canvas(CARD_W, CARD_H, _BG)

    margin = 28
    canvas.fill_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                     _CARD)
    canvas.frame_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                      2, _BORDER)

    pad = 72
    logo_x = CARD_W - pad - _LOGO_W
    canvas.blit_rgba(logo_x, margin + 40, _LOGO_W, _LOGO_H, _logo_rgba())

    title = "@" + name
    title_max_w = logo_x - pad - 40
    scale = 6
    while scale > 3 and text_width(title, scale) > title_max_w:
        scale -= 1
    canvas.text(pad, 96, _fit(title, scale, title_max_w), scale, _FG)

    y = 96 + 7 * scale + 26
    for line in _wrap("invites you to ForkMesh - distributed Git hosting on a "
                      "mesh of desktop nodes.", 3, title_max_w, 2):
        canvas.text(pad, y, line, 3, _TEXT)
        y += 7 * 3 + 12

    canvas.fill_rect(pad, 268, CARD_W - 2 * pad, 2, _BORDER)

    # One wide row: the two counters this link has earned, plus how recently.
    cells = (
        ("CLICKS", format_exact(info.get("clicks"))),
        ("SIGNUPS", format_exact(info.get("signups"))),
        ("LAST ACTIVITY", format_age(info.get("lastTs"), now_s)),
    )
    grid_x = pad
    grid_w = CARD_W - 2 * pad
    col_w = grid_w // 3
    for index, (label, value) in enumerate(cells):
        cx = grid_x + index * col_w
        value_scale = 10
        while value_scale > 3 and text_width(value, value_scale) > col_w - 24:
            value_scale -= 1
        canvas.text(cx, 330, value, value_scale, _FG)
        canvas.text(cx, 330 + 7 * 10 + 16, label, 2, _MUTED)

    footer_y = CARD_H - margin - 48
    host = str(info.get("host") or "forkmesh.com")
    canvas.text(pad, footer_y,
                _fit("%s/r/%s" % (host, name), 3, grid_w - 360), 3, _ACCENT)
    right = "REFERRAL PROGRAM"
    canvas.text(CARD_W - pad - text_width(right, 2), footer_y + 4,
                right, 2, _MUTED)
    return canvas.png()


def render_hackathon_card(info):
    """Render the 1200x630 OpenGraph PNG for a hackathon share link.

    The counters are read live at request time, so an unfurl of an invite,
    rank or squad link shows the standing the invitation is actually built
    around rather than a generic campaign image. Same renderer, palette and
    cache rules as the repository and referral cards.

    info keys: kind, title, name, headline, cells (three label/value pairs),
    host, path.
    """
    canvas = _Canvas(CARD_W, CARD_H, _BG)

    margin = 28
    canvas.fill_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                     _CARD)
    canvas.frame_rect(margin, margin, CARD_W - 2 * margin, CARD_H - 2 * margin,
                      2, _BORDER)

    pad = 72
    logo_x = CARD_W - pad - _LOGO_W
    canvas.blit_rgba(logo_x, margin + 40, _LOGO_W, _LOGO_H, _logo_rgba())

    # Campaign name above the headline, so the card reads as an invitation
    # to a specific event rather than to the product in general.
    kicker = str(info.get("title") or "ForkMesh Hackathon").upper()
    max_w = logo_x - pad - 40
    canvas.text(pad, 74, _fit(kicker, 2, max_w), 2, _ACCENT)

    headline = str(info.get("headline") or "")
    scale = 6
    while scale > 3 and text_width(headline, scale) > max_w:
        scale -= 1
    canvas.text(pad, 112, _fit(headline, scale, max_w), scale, _FG)

    y = 112 + 7 * scale + 24
    for line in _wrap(
            "Build something that outlives its host. Join the ForkMesh "
            "Hackathon and ship it on a mesh of independent nodes.",
            3, max_w, 2):
        canvas.text(pad, y, line, 3, _TEXT)
        y += 7 * 3 + 12

    canvas.fill_rect(pad, 300, CARD_W - 2 * pad, 2, _BORDER)

    cells = tuple(info.get("cells") or ())[:3]
    grid_w = CARD_W - 2 * pad
    col_w = grid_w // max(1, len(cells) or 1)
    for index, (label, value) in enumerate(cells):
        text = value if isinstance(value, str) else format_exact(value)
        cx = pad + index * col_w
        # Leave a real gutter rather than the last legible size: three
        # five-digit figures at scale 10 touch, and two counters that touch
        # read as one number.
        value_scale = 10
        while value_scale > 3 and text_width(text, value_scale) > col_w - 64:
            value_scale -= 1
        canvas.text(cx, 356, text, value_scale, _FG)
        canvas.text(cx, 356 + 7 * 10 + 16, str(label), 2, _MUTED)

    footer_y = CARD_H - margin - 48
    host = str(info.get("host") or "forkmesh.com")
    path = str(info.get("path") or "/hackathon")
    canvas.text(pad, footer_y,
                _fit(host + path, 3, grid_w - 360), 3, _ACCENT)
    right = "FORKMESH HACKATHON"
    canvas.text(CARD_W - pad - text_width(right, 2), footer_y + 4,
                right, 2, _MUTED)
    return canvas.png()
