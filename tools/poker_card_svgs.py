#!/usr/bin/env python3
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = ROOT / "marketplace/packages/poker/content/desktop/cards"

W, H = 100, 140
RED = "#c8243a"
BLACK = "#16202c"
GOLD = "#d9b26a"
RANKS = ["2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"]
SUITS = {"S": BLACK, "H": RED, "D": RED, "C": BLACK}

SUIT_PATHS = {
    "H": ["M50 94 C40 84 4 62 4 32 C4 15 16 5 30 5 C40 5 46 11 50 19 "
          "C54 11 60 5 70 5 C84 5 96 15 96 32 C96 62 60 84 50 94 Z"],
    "D": ["M50 2 Q66 28 90 50 Q66 72 50 98 Q34 72 10 50 Q34 28 50 2 Z"],
    "S": ["M50 3 C62 22 96 40 96 64 C96 78 86 86 74 86 C64 86 57 81 53 74 "
          "C54 84 58 92 66 98 L34 98 C42 92 46 84 47 74 "
          "C43 81 36 86 26 86 C14 86 4 78 4 64 C4 40 38 22 50 3 Z"],
    "C": ["M29 27 A21 21 0 1 1 71 27 A21 21 0 1 1 29 27 Z",
          "M5 62 A21 21 0 1 1 47 62 A21 21 0 1 1 5 62 Z",
          "M53 62 A21 21 0 1 1 95 62 A21 21 0 1 1 53 62 Z",
          "M38 38 L62 38 L62 66 L38 66 Z",
          "M47 64 C47 82 43 91 34 98 L66 98 C57 91 53 82 53 64 Z"],
}

L, C, R = 35.0, 50.0, 65.0
T, B = 29.0, 111.0
Q1, Q2 = T + (B - T) / 3, T + 2 * (B - T) / 3
M = (T + B) / 2

PIPS = {
    "2": [(C, T), (C, B)],
    "3": [(C, T), (C, M), (C, B)],
    "4": [(L, T), (R, T), (L, B), (R, B)],
    "5": [(L, T), (R, T), (C, M), (L, B), (R, B)],
    "6": [(L, T), (R, T), (L, M), (R, M), (L, B), (R, B)],
    "7": [(L, T), (R, T), (C, (T + M) / 2), (L, M), (R, M), (L, B), (R, B)],
    "8": [(L, T), (R, T), (C, (T + M) / 2), (L, M), (R, M), (C, (M + B) / 2), (L, B), (R, B)],
    "9": [(L, T), (R, T), (L, Q1), (R, Q1), (C, M), (L, Q2), (R, Q2), (L, B), (R, B)],
    "10": [(L, T), (R, T), (C, (T + Q1) / 2), (L, Q1), (R, Q1), (L, Q2), (R, Q2),
           (C, (Q2 + B) / 2), (L, B), (R, B)],
}

COURT = ("J", "Q", "K")


def fmt(value):
    return f"{value:.2f}".rstrip("0").rstrip(".")


def suit(code, cx, cy, size, fill, flipped=False):
    scale = size / 100
    rotate = f" rotate(180 {fmt(cx)} {fmt(cy)})" if flipped else ""
    transform = f"{rotate.strip()} translate({fmt(cx - size / 2)} {fmt(cy - size / 2)}) scale({fmt(scale)})"
    paths = "".join(f'<path d="{d}"/>' for d in SUIT_PATHS[code])
    return f'<g fill="{fill}" transform="{transform.strip()}">{paths}</g>'


def face_base():
    return (
        '<defs><linearGradient id="face" x1="0" y1="0" x2="0" y2="1">'
        '<stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#eef0f3"/>'
        "</linearGradient></defs>"
        '<rect x="1.5" y="1.5" width="97" height="137" rx="8" fill="url(#face)" stroke="#b9c2cc" stroke-width="1.5"/>'
    )


def index(rank, code, ink):
    one = (
        f'<text x="12.5" y="24" text-anchor="middle" font-family="DejaVu Sans,Arial,sans-serif" '
        f'font-size="{17 if rank == "10" else 22}" font-weight="bold" fill="{ink}">{rank}</text>'
        + suit(code, 12.5, 36, 13, ink)
    )
    return one + f'<g transform="rotate(180 50 70)">{one}</g>'


def pip_card(rank, code, ink):
    size = 17 if rank in ("9", "10") else 19
    body = "".join(suit(code, x, y, size, ink, flipped=y > M + 0.1) for x, y in PIPS[rank])
    return face_base() + index(rank, code, ink) + body


def ace_card(code, ink):
    size = 58 if code == "S" else 46
    ring = (
        f'<circle cx="50" cy="70" r="36" fill="none" stroke="{ink}" stroke-opacity="0.18" stroke-width="1.2"/>'
        if code != "S" else ""
    )
    return face_base() + index("A", code, ink) + ring + suit(code, 50, 70, size, ink)


def court_half(rank, code, ink):
    return (
        f'<text x="50" y="58" text-anchor="middle" font-family="DejaVu Serif,Georgia,serif" '
        f'font-size="34" font-weight="bold" fill="{ink}">{rank}</text>'
        + suit(code, 34.5, 33, 11, ink)
        + suit(code, 65.5, 33, 11, ink)
    )


def court_card(rank, code, ink):
    frame = (
        f'<defs><linearGradient id="panel" x1="0" y1="0" x2="1" y2="1">'
        f'<stop offset="0" stop-color="{GOLD}" stop-opacity="0.14"/>'
        f'<stop offset="0.5" stop-color="{GOLD}" stop-opacity="0.3"/>'
        f'<stop offset="1" stop-color="{GOLD}" stop-opacity="0.14"/>'
        "</linearGradient></defs>"
        '<rect x="24" y="18" width="52" height="104" rx="4" fill="url(#panel)" '
        f'stroke="{GOLD}" stroke-width="1.6"/>'
        f'<rect x="27" y="21" width="46" height="98" rx="2.5" fill="none" stroke="{ink}" '
        'stroke-opacity="0.35" stroke-width="0.8"/>'
        f'<line x1="30" y1="70" x2="70" y2="70" stroke="{GOLD}" stroke-width="1"/>'
        + suit(code, 50, 70, 9, ink)
    )
    half = court_half(rank, code, ink)
    return (
        face_base() + index(rank, code, ink) + frame
        + half + f'<g transform="rotate(180 50 70)">{half}</g>'
    )


def clipped(x0, y0, dx, dy, left, top, right, bottom):
    lo, hi = float("-inf"), float("inf")
    for start, delta, low, high in ((x0, dx, left, right), (y0, dy, top, bottom)):
        if delta == 0:
            if not low <= start <= high:
                return None
            continue
        a, b = (low - start) / delta, (high - start) / delta
        lo, hi = max(lo, min(a, b)), min(hi, max(a, b))
    if lo >= hi:
        return None
    return f"M{fmt(x0 + dx * lo)} {fmt(y0 + dy * lo)} L{fmt(x0 + dx * hi)} {fmt(y0 + dy * hi)}"


def card_back():
    lattice = []
    for offset in range(-120, 100, 8):
        for line in (clipped(offset, 10, 1, 1, 10, 10, 90, 130), clipped(offset + 120, 10, -1, 1, 10, 10, 90, 130)):
            if line:
                lattice.append(line)
    return (
        '<defs><linearGradient id="navy" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#173a63"/><stop offset="1" stop-color="#06162a"/>'
        "</linearGradient></defs>"
        '<rect x="1.5" y="1.5" width="97" height="137" rx="8" fill="#f7f4ec" stroke="#b9c2cc" stroke-width="1.5"/>'
        '<rect x="6" y="6" width="88" height="128" rx="6" fill="url(#navy)"/>'
        f'<path d="{" ".join(lattice)}" fill="none" stroke="#2c5a8c" stroke-opacity="0.55" stroke-width="0.8"/>'
        f'<rect x="10" y="10" width="80" height="120" rx="5" fill="none" stroke="{GOLD}" stroke-width="1.6"/>'
        f'<rect x="13.5" y="13.5" width="73" height="113" rx="3.5" fill="none" stroke="{GOLD}" '
        'stroke-opacity="0.5" stroke-width="0.8"/>'
        f'<path d="M50 40 L73 70 L50 100 L27 70 Z" fill="#0a2240" stroke="{GOLD}" stroke-width="2"/>'
        + suit("S", 50, 70, 22, GOLD)
    )


def svg(body):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">\n'
        f"{body}\n</svg>\n"
    )


def cards():
    out = {"card_back.svg": svg(card_back())}
    for rank in RANKS:
        for code, ink in SUITS.items():
            if rank == "A":
                body = ace_card(code, ink)
            elif rank in COURT:
                body = court_card(rank, code, ink)
            else:
                body = pip_card(rank, code, ink)
            out[f"{rank}_{code}.svg"] = svg(body)
    return out


def main():
    parser = argparse.ArgumentParser(description="Write the desktop poker card SVGs.")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--check", action="store_true", help="fail if the files on disk differ")
    args = parser.parse_args()
    stale = []
    for name, text in cards().items():
        path = args.out / name
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(name)
        else:
            path.write_text(text, encoding="utf-8")
    if stale:
        raise SystemExit("stale card SVGs: " + ", ".join(stale))


if __name__ == "__main__":
    main()
