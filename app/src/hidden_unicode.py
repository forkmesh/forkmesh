from __future__ import annotations

from typing import Any

POLICY = "forkmesh-hidden-unicode-v1"

_DROP_RANGES = (
    (0x0000, 0x0008),
    (0x000B, 0x000C),
    (0x000E, 0x001F),
    (0x007F, 0x009F),
    (0x00AD, 0x00AD),
    (0x034F, 0x034F),
    (0x115F, 0x1160),
    (0x17B4, 0x17B5),
    (0x180B, 0x180F),
    (0x200B, 0x200B),
    (0x202A, 0x202E),
    (0x2060, 0x206F),
    (0x3164, 0x3164),
    (0xD800, 0xDFFF),
    (0xFDD0, 0xFDEF),
    (0xFE00, 0xFE0D),
    (0xFEFF, 0xFEFF),
    (0xFFA0, 0xFFA0),
    (0xFFF9, 0xFFFB),
    (0x1BCA0, 0x1BCA3),
    (0x1D173, 0x1D17A),
    (0xE0000, 0xE007F),
    (0xE0100, 0xE01EF),
)

_EMOJI_RANGES = (
    (0x00A9, 0x00A9),
    (0x00AE, 0x00AE),
    (0x203C, 0x203C),
    (0x2049, 0x2049),
    (0x2122, 0x2122),
    (0x2139, 0x2139),
    (0x2190, 0x2BFF),
    (0x3030, 0x3030),
    (0x303D, 0x303D),
    (0x3297, 0x3297),
    (0x3299, 0x3299),
    (0x1F000, 0x1FAFF),
)

_JOINING_RANGES = (
    (0x0600, 0x08FF),
    (0x0900, 0x0DFF),
    (0x0F00, 0x109F),
    (0x1780, 0x18AF),
    (0xFB50, 0xFDFF),
    (0xFE70, 0xFEFC),
)

_RTL_RANGES = (
    (0x0590, 0x08FF),
    (0xFB1D, 0xFDFF),
    (0xFE70, 0xFEFC),
    (0x10800, 0x10FFF),
    (0x1E800, 0x1EFFF),
)

_ZWNJ = 0x200C
_ZWJ = 0x200D
_DIRECTION_MARKS = (0x061C, 0x200E, 0x200F)
_PRESENTATION_SELECTORS = (0xFE0E, 0xFE0F)
_BLACK_FLAG = 0x1F3F4
_CANCEL_TAG = 0xE007F
_WHITESPACE = (0x09, 0x0A, 0x0D, 0x20)


def _within(cp: int, ranges: tuple[tuple[int, int], ...]) -> bool:
    for low, high in ranges:
        if low <= cp <= high:
            return True
    return False


def _noncharacter(cp: int) -> bool:
    return (cp & 0xFFFE) == 0xFFFE


def _flag_tag_length(cps: list[int], start: int) -> int:
    index = start
    while index < len(cps) and index - start < 8:
        cp = cps[index]
        if cp == _CANCEL_TAG:
            run = index - start
            return run + 1 if 2 <= run <= 7 else 0
        if not (0xE0030 <= cp <= 0xE0039 or 0xE0061 <= cp <= 0xE007A):
            return 0
        index += 1
    return 0


def clean(text: str) -> str:
    if not isinstance(text, str) or not text:
        return text
    if text.isascii() and text.isprintable():
        return text
    cps = [ord(ch) for ch in text]
    out: list[int] = []
    index = 0
    total = len(cps)
    while index < total:
        cp = cps[index]
        prev = out[-1] if out else -1
        following = cps[index + 1] if index + 1 < total else -1
        index += 1
        if cp in (0x2028, 0x2029):
            out.append(0x0A)
            continue
        if cp in _PRESENTATION_SELECTORS:
            if prev >= 0 and prev not in _WHITESPACE and prev not in _PRESENTATION_SELECTORS and prev not in (_ZWJ, _ZWNJ):
                out.append(cp)
            continue
        if cp == _ZWJ:
            emoji = (prev == 0xFE0F or _within(prev, _EMOJI_RANGES)) and _within(following, _EMOJI_RANGES)
            joining = _within(prev, _JOINING_RANGES) and _within(following, _JOINING_RANGES)
            if emoji or joining:
                out.append(cp)
            continue
        if cp == _ZWNJ:
            if _within(prev, _JOINING_RANGES) and _within(following, _JOINING_RANGES):
                out.append(cp)
            continue
        if cp in _DIRECTION_MARKS:
            if _within(prev, _RTL_RANGES) or _within(following, _RTL_RANGES):
                out.append(cp)
            continue
        if cp == _BLACK_FLAG:
            out.append(cp)
            run = _flag_tag_length(cps, index)
            out.extend(cps[index:index + run])
            index += run
            continue
        if _within(cp, _DROP_RANGES) or _noncharacter(cp):
            continue
        out.append(cp)
    if out == cps:
        return text
    return "".join(chr(cp) for cp in out)


def has_hidden(text: str) -> bool:
    return isinstance(text, str) and clean(text) != text


_RAW_KEY_PARTS = (
    "password",
    "passphrase",
    "passcode",
    "secret",
    "token",
    "signature",
    "ciphertext",
    "privatekey",
    "sealed",
)
_RAW_KEYS = frozenset(("sig", "nonce", "salt", "otp", "pin", "mac", "hmac", "totp"))


def raw_key(key: Any) -> bool:
    if not isinstance(key, str):
        return False
    folded = key.lower().replace("_", "").replace("-", "")
    return folded in _RAW_KEYS or any(part in folded for part in _RAW_KEY_PARTS)


def clean_value(value: Any, *, depth: int = 0) -> Any:
    if depth > 64:
        return value
    if isinstance(value, str):
        return clean(value)
    if isinstance(value, list):
        return [clean_value(item, depth=depth + 1) for item in value]
    if isinstance(value, dict):
        return {
            (clean(key) if isinstance(key, str) else key):
                (item if raw_key(key) else clean_value(item, depth=depth + 1))
            for key, item in value.items()
        }
    return value


def clean_pairs(pairs: Any) -> list:
    return [
        (clean(key) if isinstance(key, str) else key, item if raw_key(key) else clean_value(item))
        for key, item in pairs
    ]


def clean_query(parsed: dict) -> dict:
    return {
        (clean(key) if isinstance(key, str) else key): (values if raw_key(key) else clean_value(values))
        for key, values in parsed.items()
    }
