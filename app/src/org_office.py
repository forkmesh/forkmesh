import json
import math
import random
import re
import time
import unicodedata

from org_office_park import (OFFICE_CAR, OFFICE_PARKING, PARK_FLOOR,
                             PARK_OBJ, PARK_W, PARK_X0, RIDES)

TILE = 32
OFFICE_W = 66
W = PARK_X0 + PARK_W
H = 44
OX = 6
OY = 7

ZONES = {
    ".": {"name": "Hallway", "private": False},
    "l": {"name": "Lobby", "private": False},
    "p": {"name": "Garden", "private": False},
    "c": {"name": "Coffee Huddle", "private": True},
    "a": {"name": "Engineering", "private": True},
    "b": {"name": "Design", "private": True},
    "k": {"name": "Library", "private": True},
    "m": {"name": "Media Room", "private": True},
    "s": {"name": "Lobby Couches", "private": True},
    "w": {"name": "Workspace", "private": False},
    "f": {"name": "Focus Lounge", "private": False},
    "h": {"name": "Campfire", "private": True},
    "q": {"name": "Agent Room", "private": False},
    "D": {"name": "Parking", "private": False},
    "Z": {"name": "Water Park", "private": False},
    "B": {"name": "Surf Lagoon", "private": False},
    "K": {"name": "Splash Harbor", "private": False},
    "A": {"name": "Shark Reef Tunnel", "private": False},
    "V": {"name": "Volcano Plaza", "private": False},
}
CHAIRS = {"v": 0, "^": 1, "<": 2, ">": 3,
          "n": 0, "u": 1, "d": 2, "c": 3, "o": 0,
          "S": 0, "s": 1, "j": 0, "h": 1}
COUCHES = frozenset(["S", "s", "j"])
WATER = "W"
WALKABLE_OBJ = frozenset(["", "r", WATER] + list(CHAIRS))
FIRE_ZONE = "h"
AGENT_ZONE = "q"
GATE = "x"
RIDE_GATE = "y"
CAR = "e"
COASTER_PLATFORM = (-6 + 7, 31 + 6, 39 + 6)
FIRE_SEATS = tuple((x + 6, y + 7) for x, y in (
    (44, -5), (46, -5), (47, -4), (47, -2),
    (46, -1), (44, -1), (43, -2), (43, -4)))
FIRE_REFRESH_MS = 600000
FIRE_RECENCY = ((3600000, 48), (5 * 3600000, 46), (86400000, 42),
                (3 * 86400000, 36), (5 * 86400000, 32),
                (10 * 86400000, 25), (30 * 86400000, 18),
                (60 * 86400000, 11), (120 * 86400000, 5))
FIRE_ACTIVE_TIERS = ((40 * 3600000, 22), (12 * 3600000, 17),
                     (4 * 3600000, 12), (3600000, 7), (600000, 3))
FIRE_STEP_TIERS = ((100000, 13), (25000, 10), (5000, 7), (500, 4), (1, 1))
FIRE_OFFICE_MS_TIERS = ((20 * 3600000, 13), (8 * 3600000, 10),
                        (2 * 3600000, 7), (1800000, 4), (60000, 1))
FIRE_OFFICE_VISIT_TIERS = ((20, 6), (5, 4), (1, 2))
FIRE_WINDOW_MS = 30 * 86400000
FIRE_BLAZE_SCORE = 70
FIRE_EMBER_SCORE = 40
FIRE_EMBER_DEPTH = 10

MAX_USERS = 100
MESSAGE_MAX_CHARS = 16384
FLUSH_MS = 50
AV_MAX = 10000
AV_CUSTOM = 1 << 24
AV_FIELDS = (6, 8, 12, 12, 8, 4, 8, 4)
AV_COMBOS = math.prod(AV_FIELDS)
EMOTES = frozenset(["👋", "❤️", "🎉", "🤣", "👏", "💃", "👍", "💯", "🔥"])
CHEER_EMOTES = frozenset(["👏", "🎉", "💃"])
CHEER_WINDOW_MS = 4000
CHEER_MIN = 3
CHEER_COOLDOWN_MS = 8000
NEAR = 3
FAR = 5
SIGNAL_SLACK = 2
NAME_MAX = 24
SAY_MAX = 300


def now_ms():
    return int(time.time() * 1000)


def js_round(value):
    return math.floor(value + 0.5)


def is_int(value):
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return True
    return isinstance(value, float) and math.isfinite(value) and value.is_integer()


def to_int32(value):
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, str):
        try:
            value = float(value.strip() or "0")
        except ValueError:
            return 0
    if not isinstance(value, (int, float)) or (
            isinstance(value, float) and not math.isfinite(value)):
        return 0
    number = int(value) & 0xFFFFFFFF
    return number - 0x100000000 if number & 0x80000000 else number


def to_number(value):
    if value is None:
        return math.nan
    if isinstance(value, bool):
        return float(value)
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return 0.0
        try:
            return float(text)
        except ValueError:
            return math.nan
    return math.nan


def finite(value):
    number = to_number(value)
    return number if math.isfinite(number) else None


def js_str(value):
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if value.is_integer():
            return str(int(value))
        return repr(value)
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return ",".join(js_str(item) for item in value)
    return "[object Object]"


def _build_map():
    floor = [["g"] * W for _ in range(H)]
    obj = [[""] * W for _ in range(H)]

    def rect(x0, y0, x1, y1, char, layer):
        for y in range(y0 + OY, y1 + OY + 1):
            for x in range(x0 + OX, x1 + OX + 1):
                layer[y][x] = char

    def room(x0, y0, x1, y1, char, doors=()):
        rect(x0, y0, x1, y1, "#", floor)
        rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, char, floor)
        for x, y in doors:
            floor[y + OY][x + OX] = char

    def put(items):
        for char, x, y in items:
            obj[y + OY][x + OX] = char

    room(1, 1, 52, 24, ".", [(19, 1), (20, 1)])
    room(1, 1, 14, 9, "a", [(7, 9), (8, 9)])
    room(25, 1, 38, 9, "b", [(31, 9), (32, 9)])
    room(1, 15, 15, 24, "c", [(15, 20), (15, 21)])
    rect(2, 16, 14, 16, "#", floor)
    room(24, 15, 38, 24, "l", [(30, 15), (31, 15)])
    room(38, 1, 52, 9, "k", [(45, 9), (46, 9)])
    room(38, 15, 52, 24, "m", [(45, 15), (46, 15), (38, 21), (38, 22)])
    rect(33, 17, 37, 20, "s", floor)
    rect(31, -5, 40, 0, "p", floor)
    rect(41, -5, 49, 0, FIRE_ZONE, floor)
    room(-5, -6, 30, 0, AGENT_ZONE, [(30, -3), (30, -2), (3, 0), (4, 0),
                                     (19, 0), (20, 0)])
    floor[1 + OY][3 + OX] = "a"
    floor[1 + OY][4 + OX] = "a"
    x0, y0, x1, y1 = OFFICE_PARKING
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            floor[y][x] = "D"
    floor[10 + OY][52 + OX] = "."
    floor[11 + OY][52 + OX] = "."
    obj[OFFICE_CAR[1]][OFFICE_CAR[0]] = CAR
    for y in range(H):
        for x in range(PARK_W):
            floor[y][PARK_X0 + x] = PARK_FLOOR[y][x]
            char = PARK_OBJ[y][x]
            obj[y][PARK_X0 + x] = "" if char == "_" else char
    rect(31, -6, 40, -6, "p", floor)
    put([
        ("P", 2, 2), ("P", 13, 2), ("B", 6, 2), ("B", 7, 2), ("B", 8, 2),
        ("H", 11, 2), ("M", 4, 5), ("D", 5, 5), ("^", 4, 6),
        ("M", 9, 5), ("D", 10, 5), ("^", 9, 6),
    ])
    put([
        ("P", 26, 2), ("P", 37, 2), ("B", 30, 2), ("B", 31, 2), ("B", 32, 2),
        ("H", 27, 2), ("M", 28, 5), ("D", 29, 5), ("^", 28, 6),
        ("M", 33, 5), ("D", 34, 5), ("^", 33, 6),
    ])
    put([
        ("R", 2, 17), ("K", 3, 17), ("C", 4, 17), ("K", 5, 17), ("K", 6, 17),
        ("T", 8, 20), ("v", 8, 19), ("^", 8, 21), (">", 7, 20), ("<", 9, 20),
        ("S", 10, 17), ("S", 11, 17), ("S", 12, 17), ("P", 14, 17),
        ("P", 2, 23), ("P", 14, 23),
    ])
    put([
        ("D", 26, 17), ("M", 27, 17), ("D", 28, 17),
        ("T", 30, 19), ("T", 31, 19), ("T", 30, 20), ("T", 31, 20),
        ("v", 30, 18), ("v", 31, 18), ("^", 30, 21), ("^", 31, 21),
        ("P", 25, 16), ("P", 37, 16), ("P", 37, 23), ("S", 34, 23),
        ("S", 35, 23), ("S", 36, 23),
    ])
    rect(29, 22, 32, 23, "r", obj)
    put([
        ("N", 33, 17), ("n", 34, 17), ("n", 35, 17),
        ("c", 33, 18), ("F", 34, 18), ("F", 35, 18), ("d", 37, 18),
        ("c", 33, 19), ("u", 35, 19), ("o", 37, 20),
    ])
    put([
        ("P", 39, 2), ("B", 40, 2), ("B", 41, 2), ("B", 42, 2), ("B", 43, 2),
        ("L", 47, 2), ("B", 48, 2), ("B", 49, 2), ("B", 50, 2), ("P", 51, 2),
        ("v", 41, 4), (">", 40, 5), ("T", 41, 5), ("<", 42, 5), ("^", 41, 6),
        ("v", 49, 4), (">", 48, 5), ("T", 49, 5), ("<", 50, 5), ("^", 49, 6),
        ("P", 39, 8), ("u", 40, 8), ("u", 50, 8), ("P", 51, 8),
    ])
    put([
        ("P", 39, 16), ("V", 40, 16), ("V", 41, 16),
        ("X", 48, 16), ("Z", 49, 16), ("Z", 50, 16), ("Q", 51, 16),
        ("v", 44, 18), ("v", 45, 18), ("v", 46, 18),
        (">", 43, 19), ("T", 44, 19), ("T", 45, 19), ("T", 46, 19),
        ("<", 47, 19),
        (">", 43, 20), ("T", 44, 20), ("T", 45, 20), ("T", 46, 20),
        ("<", 47, 20),
        ("^", 44, 21), ("^", 45, 21), ("^", 46, 21),
        ("P", 39, 23), ("P", 51, 23),
    ])
    put([
        ("P", 2, 10), ("P", 37, 10), ("P", 2, 14), ("P", 37, 14),
        ("P", 51, 10), ("P", 51, 14),
        ("G", 18, 4), ("G", 19, 4), ("G", 20, 4), ("G", 21, 4),
        ("G", 18, 5), ("G", 19, 5), ("G", 20, 5), ("G", 21, 5),
        ("Q", 16, 2), ("A", 22, 2), ("A", 23, 2), ("P", 15, 2), ("P", 24, 2),
        ("P", 16, 23), ("P", 23, 23), ("A", 19, 23), ("A", 20, 23),
    ])
    put([
        ("W", 34, -4), ("W", 35, -4), ("W", 36, -4), ("W", 37, -4),
        ("W", 34, -3), ("W", 35, -3), ("W", 36, -3), ("W", 37, -3),
        ("v", 32, -4), ("v", 39, -4), (">", 32, -2), ("<", 39, -2),
        ("Y", 31, -5), ("Y", 40, -5), ("Y", 31, 0), ("Y", 40, 0),
        ("Y", 33, -5), ("Y", 38, -5),
    ])
    put([
        ("I", 45, -3),
        ("v", 44, -5), ("v", 46, -5), ("<", 47, -4), ("<", 47, -2),
        ("^", 46, -1), ("^", 44, -1), (">", 43, -2), (">", 43, -4),
        ("Y", 49, -5), ("Y", 49, 0), ("P", 41, 0),
        ("J", -4, -5), ("P", -4, -1), ("P", 29, -5),
        (GATE, 40, -6),
    ])
    room(1, 24, 38, 33, "w", [(17, 24), (18, 24), (21, 24), (22, 24)])
    rect(27, 25, 37, 32, "f", floor)
    for y in (25, 28, 31):
        columns = (3, 5, 8, 10, 13, 24) if y == 25 else \
            (3, 5, 8, 10, 13, 15, 18, 20, 23)
        for x in columns:
            put([("M", x, y), ("D", x + 1, y), ("^", x, y + 1)])
    put([
        ("P", 2, 25), ("P", 26, 25), ("P", 26, 32),
        ("B", 27, 25), ("B", 28, 25), ("P", 30, 25), ("P", 34, 25),
        ("B", 36, 25), ("B", 37, 25),
        ("S", 30, 26), ("S", 31, 26), ("S", 32, 26), ("S", 33, 26),
        ("S", 34, 26),
        ("j", 28, 27), ("j", 28, 28), ("j", 28, 29), ("j", 36, 27),
        ("j", 36, 28), ("j", 36, 29),
        ("t", 31, 28), ("t", 32, 28), ("t", 33, 28),
        ("s", 30, 30), ("s", 31, 30), ("s", 32, 30), ("s", 33, 30),
        ("s", 34, 30),
        ("P", 27, 32), ("P", 37, 32),
    ])
    trees = [
        ("E", 2, 4), ("O", 4, 9), ("E", 1, 13), ("U", 3, 17), ("E", 5, 21),
        ("O", 2, 25), ("E", 4, 30), ("U", 1, 34), ("E", 3, 37),
        ("O", 62, 4), ("E", 64, 8), ("U", 61, 13), ("E", 63, 18),
        ("O", 62, 23), ("E", 64, 27), ("E", 61, 31), ("O", 63, 36),
        ("E", 8, 3), ("U", 12, 5), ("E", 15, 2), ("O", 18, 5), ("E", 34, 4),
        ("U", 38, 2), ("O", 41, 5), ("E", 44, 3),
        ("O", 48, 4), ("E", 53, 3), ("U", 57, 5),
        ("E", 9, 35), ("O", 14, 36), ("E", 19, 34), ("U", 24, 36),
        ("E", 29, 35), ("O", 34, 36), ("E", 39, 34), ("U", 43, 36),
        ("O", 49, 36), ("E", 54, 34), ("U", 58, 36),
        ("O", 3, 41), ("E", 10, 43), ("U", 18, 42), ("E", 27, 43),
        ("O", 35, 42), ("E", 42, 43), ("U", 48, 41),
    ]
    for char, x, y in trees:
        if y < H and floor[y][x] == "g":
            obj[y][x] = char
    return floor, obj


FLOOR, OBJ = _build_map()


def in_bounds(x, y):
    return 0 <= x < W and 0 <= y < H


def obj_at(x, y):
    return OBJ[y][x] if in_bounds(x, y) else None


def is_walkable(x, y):
    if not (is_int(x) and is_int(y)):
        return False
    x, y = int(x), int(y)
    if not in_bounds(x, y):
        return False
    tile = FLOOR[y][x]
    return tile not in ("g", "#") and OBJ[y][x] in WALKABLE_OBJ


def zone_at(x, y):
    if not (is_int(x) and is_int(y)):
        return "g"
    x, y = int(x), int(y)
    return FLOOR[y][x] if in_bounds(x, y) else "g"


def chair_dir(x, y):
    if not in_bounds(x, y):
        return None
    return CHAIRS.get(OBJ[y][x])


def is_couch(x, y):
    return in_bounds(x, y) and OBJ[y][x] in COUCHES


def is_private(zone):
    return bool(ZONES.get(zone, {}).get("private"))


SPAWNS = [
    (x, y)
    for y in range(22 + OY, 23 + OY + 1)
    for x in range(27 + OX, 34 + OX + 1)
    if is_walkable(x, y)
]


def random_spawn(rng=random):
    x, y = SPAWNS[int(rng.random() * len(SPAWNS))]
    return {"x": x, "y": y}


GATED_ZONES = {"a": "Engineering", "b": "Design"}
ZONE_TEAM_ALIASES = {
    "a": (
        "engineering", "engineers", "development", "developers", "dev",
        "platform", "frontend", "backend", "fullstack", "mobile",
        "infrastructure", "infra", "devops", "site-reliability", "sre",
        "security", "data", "qa",
    ),
    "b": (
        "design", "designers", "product", "product-design", "ux", "ui",
        "ux-design", "ui-design", "user-research", "research", "creative",
    ),
}
ALL_ROOMS_ROLES = ("owner", "admin")
ROLES = ("owner", "admin", "maintainer", "member", "auditor")


def team_slug(value):
    text = js_str(value).strip().lower()
    text = re.sub(r"[^a-z0-9]+", "-", text)
    text = re.sub(r"-+", "-", text).strip("-")
    return text[:64]


def clean_teams(teams):
    if not isinstance(teams, (list, tuple)):
        return []
    return sorted({slug for slug in (team_slug(t) for t in teams) if slug})[:50]


def clean_role(value):
    role = js_str(value).lower()
    return role if role in ROLES else ""


def _fnv(text, seed):
    value = seed & 0xFFFFFFFF
    for unit in _utf16_units(text):
        value ^= unit
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def _utf16_units(text):
    for char in text:
        code = ord(char)
        if code > 0xFFFF:
            code -= 0x10000
            yield 0xD800 + (code >> 10)
            yield 0xDC00 + (code & 0x3FF)
        else:
            yield code


def avatar_for(identity):
    return _fnv(js_str(identity), 2166136261) % AV_MAX


FOCUS_MODES = ("focused", "pomodoro", "freeform")
FOCUS_MINUTES = (15, 25, 30, 45, 60, 90, 120)
FOCUS_ROUNDS_MAX = 6
FOCUS_WORK_MS = 25 * 60000
FOCUS_BREAK_MS = 5 * 60000
FOCUS_TOPIC_MAX = 60


def clean_focus(value, seated, now):
    if not isinstance(value, dict) or not seated:
        return None
    mode = js_str(value.get("mode"))
    if mode not in FOCUS_MODES:
        return None
    out = {"mode": mode, "at": now}
    topic = clean_work_text(value.get("topic"), FOCUS_TOPIC_MAX)
    if topic:
        out["topic"] = topic
    emoji = clean_emoji(value.get("emoji")) if value.get("emoji") is not None \
        else ""
    if emoji:
        out["emoji"] = emoji
    if mode == "focused":
        mins = value.get("mins")
        if not is_int(mins) or int(mins) not in FOCUS_MINUTES:
            return None
        out["mins"] = int(mins)
        out["end"] = now + int(mins) * 60000
    elif mode == "pomodoro":
        rounds = value.get("rounds")
        if not is_int(rounds) or not 1 <= int(rounds) <= FOCUS_ROUNDS_MAX:
            return None
        out["rounds"] = int(rounds)
        out["end"] = now + int(rounds) * (FOCUS_WORK_MS + FOCUS_BREAK_MS) \
            - FOCUS_BREAK_MS
    return out


def join_focus(session, host_id, seated, now):
    if not isinstance(session, dict) or not seated or not host_id or \
            session.get("mode") not in FOCUS_MODES:
        return None
    if "end" in session and not session["end"] > now:
        return None
    out = {"mode": session["mode"], "at": session.get("at"),
           "host": session.get("host") or host_id}
    for key in ("topic", "emoji", "mins", "rounds", "end"):
        if key in session:
            out[key] = session[key]
    return out


def cheer_mark(marks, uid, now, present):
    fresh = {key: at for key, at in marks.items()
             if key in present and now - at < CHEER_WINDOW_MS}
    fresh[uid] = now
    return fresh


def clean_avatar(value):
    if not is_int(value):
        return None
    number = int(value)
    if 0 <= number < AV_MAX or \
            AV_CUSTOM <= number < AV_CUSTOM + AV_COMBOS:
        return number
    return None


def unlocked_zones(teams, role):
    if role in ALL_ROOMS_ROLES:
        return list(GATED_ZONES)
    have = set(clean_teams(teams))
    return [
        zone for zone in GATED_ZONES
        if any(alias in have for alias in ZONE_TEAM_ALIASES[zone])
    ]


def may_enter(profile, zone, admitted=None):
    if zone not in GATED_ZONES:
        return True
    if admitted and zone in admitted:
        return True
    return zone in unlocked_zones(
        profile.get("teams") or [], profile.get("role") or "")



def _points(value, tiers):
    total = value if is_int(value) and value > 0 else 0
    for minimum, points in tiers:
        if total >= minimum:
            return points
    return 0


def _count(row, key):
    value = (row or {}).get(key)
    return int(value) if is_int(value) and value > 0 else 0


def fire_score(row, now):
    last = max(_count(row, "world_at"), _count(row, "contrib_at"),
               _count(row, "office_at"))
    age = now - last if last else None
    recency = 0
    if age is not None:
        for limit, points in FIRE_RECENCY:
            if age < limit:
                recency = points
                break
    contributions = sum(_count(row, k) for k in
                        ("issues", "pulls", "commits", "discussions"))
    depth = (_points(_count(row, "active_ms"), FIRE_ACTIVE_TIERS)
             + _points(_count(row, "steps"), FIRE_STEP_TIERS)
             + _points(_count(row, "office_ms"), FIRE_OFFICE_MS_TIERS)
             + _points(_count(row, "office_visits"), FIRE_OFFICE_VISIT_TIERS)
             + min(14, int(math.log2(contributions + 1) * 3.5)))
    score = max(0, min(100, recency + depth))
    tier = None
    if age is not None and age < 86400000 and score >= FIRE_BLAZE_SCORE:
        tier = "blaze"
    elif age is not None and age < 5 * 86400000 and \
            score >= FIRE_EMBER_SCORE and depth >= FIRE_EMBER_DEPTH:
        tier = "ember"
    return {"score": score, "tier": tier, "depth": depth}


def fire_ring(rows, now, seats=len(FIRE_SEATS)):
    ranked = []
    for row in rows or []:
        name = str((row or {}).get("name") or "")
        bi = str((row or {}).get("bi") or "")
        if not name or not bi:
            continue
        result = fire_score(row, now)
        if result["tier"] is None:
            continue
        ranked.append((0 if result["tier"] == "blaze" else 1,
                       -result["score"], -_count(row, "office_ms"),
                       -_count(row, "active_ms"), name.lower(), bi,
                       name, result))
    ranked.sort()
    return [{"bi": item[5], "name": item[6], "score": item[7]["score"],
             "tier": item[7]["tier"], "rank": index + 1}
            for index, item in enumerate(ranked[:seats])]


def roster_ok(record):
    if not isinstance(record, dict):
        return False
    kind = str(record.get("kind") or "").strip().lower()
    if kind not in ("user", "node"):
        kind = "user" if record.get("pass_hash") else "node"
    return kind == "user" and record.get("status") == "active" and \
        not record.get("profile_private")


def fire_public(top):
    return [{"n": t["name"], "r": t["rank"], "t": t["tier"],
             "s": t["score"]} for t in top]

STATUSES = ("available", "focus", "meeting", "away", "break")
STATUS_TEXT_MAX = 60
_CONTROL_RE = re.compile(r"[\u0000-\u001f\u007f]")
_EMOJI_FORBIDDEN_RE = re.compile(r"[<>&\"'`]")


def _extends_cluster(char):
    code = ord(char)
    return (
        code == 0x200D
        or 0xFE00 <= code <= 0xFE0F
        or 0x1F3FB <= code <= 0x1F3FF
        or 0xE0020 <= code <= 0xE007F
        or code == 0x20E3
        or unicodedata.category(char) in ("Mn", "Me", "Mc")
    )


def graphemes(text):
    out = []
    index = 0
    size = len(text)
    while index < size:
        start = index
        first = text[index]
        index += 1
        if 0x1F1E6 <= ord(first) <= 0x1F1FF and index < size and \
                0x1F1E6 <= ord(text[index]) <= 0x1F1FF:
            index += 1
        while index < size:
            char = text[index]
            if ord(char) == 0x200D and index + 1 < size:
                index += 2
                continue
            if _extends_cluster(char):
                index += 1
                continue
            break
        out.append(text[start:index])
    return out


def _utf16_len(text):
    return sum(1 for _ in _utf16_units(text))


def clean_emoji(value):
    emoji = _CONTROL_RE.sub("", js_str(value)).strip()
    if not emoji:
        return ""
    first = graphemes(emoji)[0]
    if _utf16_len(first) > 16 or _EMOJI_FORBIDDEN_RE.search(first) or any(
            unicodedata.category(ch)[0] in ("L", "N") for ch in first):
        return None
    return first


def clean_status(value):
    if not isinstance(value, dict):
        return None
    state = js_str(value.get("s") or "")
    if state not in STATUSES:
        return None
    out = {"s": state}
    emoji = clean_emoji(value.get("emoji"))
    if emoji is None:
        return None
    if emoji:
        out["emoji"] = emoji
    text = _CONTROL_RE.sub(" ", js_str(value.get("text")))
    text = re.sub(r"\s+", " ", text).strip()
    if text:
        out["text"] = "".join(graphemes(text)[:STATUS_TEXT_MAX])
    if value.get("auto") is True and state == "away":
        out["auto"] = True
    return out


DAY = 86400000
VISIT_KEEP_MS = 14 * DAY
MERGE_GAP = 90000
BOARD_SIZE = 10


def windows(now, tz):
    tz = finite(tz)
    offset = js_round(tz) * 60000 if tz is not None and abs(tz) <= 14 * 60 else 0
    local = now - offset
    day_local = (local // DAY) * DAY
    weekday = (time.gmtime(day_local / 1000).tm_wday) % 7
    day = day_local + offset
    return {"day": day, "week": day - weekday * DAY}


def _overlap(a, b, lo, hi):
    return max(0, min(b, hi) - max(a, lo))


def merge_visits(visits, gap=MERGE_GAP):
    out = []
    for visit in sorted(visits, key=lambda v: v[0]):
        if out and visit[0] - out[-1][1] <= gap:
            out[-1][1] = max(out[-1][1], visit[1])
            out[-1][2] = out[-1][2] or visit[2]
        else:
            out.append([visit[0], visit[1], visit[2]])
    return out


def summarize(rows, now, tz, me_key):
    bounds = windows(now, tz)
    day, week = bounds["day"], bounds["week"]
    people = {}
    for row in rows:
        is_open = row.get("ended") is None
        end = now if is_open else row["ended"]
        if end < week:
            continue
        person = people.get(row["who"])
        if person is None:
            person = people[row["who"]] = {
                "who": row["who"], "name": row["name"], "last": 0,
                "today": 0, "week": 0, "visits": [], "online": False,
            }
        if row["started"] >= person["last"]:
            person["name"] = row["name"]
            person["last"] = row["started"]
        person["week"] += _overlap(row["started"], end, week, now)
        person["today"] += _overlap(row["started"], end, day, now)
        if is_open:
            person["online"] = True
        if end > day:
            person["visits"].append([max(row["started"], day), end, is_open])
    everyone = list(people.values())

    def board(key):
        ranked = sorted(
            (p for p in everyone if p[key] > 0),
            key=lambda p: (-p[key], p["name"].lower()))
        return [
            {"name": p["name"], "ms": p[key], "online": p["online"],
             "me": p["who"] == me_key}
            for p in ranked[:BOARD_SIZE]
        ]

    in_today = []
    for person in everyone:
        if not person["visits"]:
            continue
        merged = merge_visits(person["visits"])
        in_today.append({
            "name": person["name"], "first": merged[0][0],
            "ms": person["today"], "online": person["online"],
            "me": person["who"] == me_key,
            "visits": [[a, None] if o else [a, b] for a, b, o in merged[-5:]],
        })
    in_today.sort(key=lambda p: p["first"])
    mine = people.get(me_key) or {}
    return {
        "day": day, "week": week, "now": now, "inToday": in_today,
        "today": board("today"), "thisWeek": board("week"),
        "me": {"today": mine.get("today", 0), "week": mine.get("week", 0)},
    }


DESK_TOPS = frozenset(["M", "D"])


def find_desks(obj=OBJ):
    out = []
    for y in range(1, len(obj)):
        for x in range(len(obj[y])):
            if obj[y][x] != "^":
                continue
            top = obj[y - 1][x]
            if top in DESK_TOPS:
                out.append({"x": x, "y": y, "mx": x, "my": y - 1,
                            "monitor": top == "M"})
    return out


DESKS = find_desks()


def owner_key(account_bi):
    return "a:" + js_str(account_bi)[:80] if account_bi else ""


def clean_work_text(value, limit):
    text = re.sub(r"[\u0000-\u001f\u007f<>]", " ", js_str(value))
    text = re.sub(r"\s+", " ", text).strip()
    return text[:limit]


def clean_task(value):
    if not isinstance(value, dict):
        return None
    task_id = js_str(value.get("id"))
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", task_id):
        return None
    title = clean_work_text(value.get("title"), 160)
    if not title:
        return None
    return {"id": task_id, "title": title}


def claim_desk(desks, key, name, x, y, valid=None):
    valid = DESKS if valid is None else valid
    if not key:
        return {"error": "no_owner"}
    if not any(d["x"] == x and d["y"] == y for d in valid):
        return {"error": "not_a_desk"}
    for other, desk in desks.items():
        if other != key and desk["x"] == x and desk["y"] == y:
            return {"error": "taken", "by": desk["name"]}
    updated = dict(desks)
    updated[key] = {"x": x, "y": y,
                    "name": clean_work_text(name, 24) or "Someone"}
    return {"desks": updated, "prev": desks.get(key)}


def release_desk(desks, key):
    if key not in desks:
        return {"desks": desks, "prev": None}
    updated = dict(desks)
    prev = updated.pop(key)
    return {"desks": updated, "prev": prev}


def desk_list(desks):
    out = []
    for desk in desks.values():
        item = {"x": desk["x"], "y": desk["y"], "name": desk["name"]}
        if desk.get("deco"):
            item["deco"] = desk["deco"]
        out.append(item)
    return out


DECO_DESKS = 6
DECO_CHAIRS = 8
DECO_SLOTS = ("top", "top", "screen", "floor")
DECO_ITEMS = (None, "top", "top", "top", "top", "top", "top", "top", "top",
              "screen", "screen", "screen", "floor", "floor", "floor")


def clean_deco(value):
    if not isinstance(value, dict):
        return None
    desk = value.get("desk")
    chair = value.get("chair")
    desk = 0 if desk is None else desk
    chair = 0 if chair is None else chair
    if not is_int(desk) or not 0 <= int(desk) < DECO_DESKS:
        return None
    if not is_int(chair) or not 0 <= int(chair) < DECO_CHAIRS:
        return None
    slots = value.get("slots")
    if not isinstance(slots, list) or len(slots) != len(DECO_SLOTS):
        return None
    out = []
    for kind, item in zip(DECO_SLOTS, slots):
        if not is_int(item):
            return None
        item = int(item)
        if item != 0 and not (0 < item < len(DECO_ITEMS)
                              and DECO_ITEMS[item] == kind):
            return None
        out.append(item)
    return {"desk": int(desk), "chair": int(chair), "slots": out}


def deco_empty(deco):
    return not deco or (deco["desk"] == 0 and deco["chair"] == 0
                        and not any(deco["slots"]))


PROFILE_NAME_MAX = 24
PROFILE_TZ_MAX = 64
_TZ_RE = re.compile(r"[A-Za-z][A-Za-z0-9_+-]*(?:/[A-Za-z0-9_+-]+){0,2}")
_PIC_RE = re.compile(r"[0-9a-f]{16}")


def clean_display_name(value):
    return clean_work_text(value, PROFILE_NAME_MAX)


def clean_tz(value):
    text = js_str(value).strip()
    if len(text) > PROFILE_TZ_MAX or not _TZ_RE.fullmatch(text):
        return ""
    return text


def clean_pic(value):
    text = js_str(value)
    return text if _PIC_RE.fullmatch(text) else ""


def clean_office_profile(value):
    out = {}
    if not isinstance(value, dict):
        return out
    name = clean_display_name(value.get("name"))
    if name:
        out["name"] = name
    tz = clean_tz(value.get("tz"))
    if tz:
        out["tz"] = tz
    avatar = clean_avatar(value.get("av"))
    if avatar is not None:
        out["av"] = avatar
    return out


def profile_key(account_bi):
    return "prof:" + owner_key(account_bi)


def name_fold(value):
    return js_str(value).strip().casefold()


AGENT_STATES = frozenset(
    ["queued", "running", "waiting", "success", "failed", "stopped"])
RELAY_MAX = 24
_AGENT_ID_RE = re.compile(r"[A-Za-z0-9._:-]{1,64}")


def clean_relay(items, now=None):
    now = now_ms() if now is None else now
    if not isinstance(items, list):
        return []
    out = []
    for item in items[:RELAY_MAX]:
        if not isinstance(item, dict):
            continue
        ident = js_str(item.get("id"))
        state = js_str(item.get("st"))
        at = item.get("at")
        if (not _AGENT_ID_RE.fullmatch(ident) or state not in AGENT_STATES
                or not is_int(at) or at <= 0 or at > now + 60000):
            continue
        out.append({"id": ident, "st": state, "at": int(at)})
    return out


KNOCK_GAP_MS = 15000
KNOCK_TTL_MS = 120000


def _find_meeting_tables():
    seen = set()
    out = []
    for y in range(H):
        for x in range(W):
            if OBJ[y][x] != "T" or (x, y) in seen:
                continue
            tiles = []
            queue = [(x, y)]
            seen.add((x, y))
            while queue:
                cx, cy = queue.pop()
                tiles.append((cx, cy))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = cx + dx, cy + dy
                    if not in_bounds(nx, ny) or (nx, ny) in seen or \
                            OBJ[ny][nx] != "T":
                        continue
                    seen.add((nx, ny))
                    queue.append((nx, ny))
            xs = [t[0] for t in tiles]
            ys = [t[1] for t in tiles]
            x0, y0 = min(xs), min(ys)
            chairs = []
            chair_keys = set()
            for tx, ty in tiles:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = tx + dx, ty + dy
                    if chair_dir(nx, ny) is None or (nx, ny) in chair_keys:
                        continue
                    chair_keys.add((nx, ny))
                    chairs.append((nx, ny))
            if not chairs:
                continue
            zone = FLOOR[y0][x0]
            out.append({
                "id": "%s-%d-%d" % (zone, x0, y0),
                "zone": zone,
                "chairs": chairs,
            })
    return out


MEETING_TABLES = _find_meeting_tables()
MEETING_TABLE_IDS = frozenset(t["id"] for t in MEETING_TABLES)
SEAT_TABLE = {
    chair: table["id"] for table in MEETING_TABLES for chair in table["chairs"]
}


def table_at_seat(x, y):
    if not (is_int(x) and is_int(y)):
        return None
    return SEAT_TABLE.get((int(x), int(y)))


def clean_meeting(value, x, y, now=None):
    if not isinstance(value, dict):
        return None
    table = js_str(value.get("table") or "")
    if table not in MEETING_TABLE_IDS or table_at_seat(x, y) != table:
        return None
    return {"table": table, "at": now_ms() if now is None else now}


def knock_allowed(last, now, gap=KNOCK_GAP_MS):
    return not last or now - last >= gap


BW = 1600
BH = 1000
WB_MAX_ITEMS = 4000
WB_MAX_BYTES = 1500000
WB_MAX_PTS = 2700
WB_MAX_TEXT = 280
WB_MAX_BATCH = 60
WB_MAX_IDS = 600
STICKY_W = 180
STICKY_H = 130
WB_KINDS = frozenset(["s", "t", "n", "r", "e", "a"])
WB_COLORS = ["#1d2030", "#e5484d", "#f59e0b", "#22a35a", "#3b45e0",
             "#8e4ec6", "#ffffff"]
_WB_ID_RE = re.compile(r"[A-Za-z0-9_-]{1,32}")
_HEX_RE = re.compile(r"#[0-9a-fA-F]{6}")


def _clamp(value, lo, hi):
    return min(hi, max(lo, value))


def _num(value, lo, hi):
    number = to_number(value)
    if not math.isfinite(number):
        return None
    return int(_clamp(js_round(number), lo, hi))


def _wb_text(value):
    text = re.sub(r"[\u0000-\u0009\u000b-\u001f\u007f]", "", js_str(value))
    return text[:WB_MAX_TEXT]


def board_id(x, y):
    return "%d.%d" % (x, y)


def parse_board_id(value):
    match = re.fullmatch(r"(\d{1,3})\.(\d{1,3})", js_str(value))
    return {"x": int(match.group(1)), "y": int(match.group(2))} if match else None


def find_boards(obj=OBJ):
    return [board_id(x, y) for y in range(len(obj)) for x in range(len(obj[y]))
            if obj[y][x] == "H"]


BOARDS = find_boards()


def sanitize_item(item):
    if not isinstance(item, dict) or item.get("k") not in WB_KINDS or \
            not _WB_ID_RE.fullmatch(js_str(item.get("id"))):
        return None
    color = item.get("c")
    color = color.lower() if isinstance(color, str) and \
        _HEX_RE.fullmatch(color) else WB_COLORS[0]
    out = {"id": js_str(item.get("id")), "k": item["k"], "c": color}
    if item["k"] == "s":
        pts = item.get("pts")
        if not isinstance(pts, list) or len(pts) < 3 or len(pts) > WB_MAX_PTS:
            return None
        clean = []
        for i in range(0, len(pts) - 2, 3):
            x = _num(pts[i], -50, BW + 50)
            y = _num(pts[i + 1], -50, BH + 50)
            p = _num(pts[i + 2], 0, 9)
            if x is None or y is None or p is None:
                return None
            clean.extend((x, y, p))
        if not clean:
            return None
        width = _num(item.get("w"), 1, 60)
        out["w"] = 4 if width is None else width
        out["pts"] = clean
        if item.get("hl"):
            out["hl"] = 1
        return out
    x = _num(item.get("x"), 0, BW)
    y = _num(item.get("y"), 0, BH)
    if x is None or y is None:
        return None
    out["x"] = x
    out["y"] = y
    if item["k"] == "t":
        text = _wb_text(item.get("text")).strip()
        if not text:
            return None
        out["text"] = text
        size = _num(item.get("sz"), 10, 96)
        out["sz"] = 24 if size is None else size
        return out
    if item["k"] == "n":
        out["text"] = _wb_text(item.get("text"))
        return out
    x2 = _num(item.get("x2"), 0, BW)
    y2 = _num(item.get("y2"), 0, BH)
    if x2 is None or y2 is None:
        return None
    out["x2"] = x2
    out["y2"] = y2
    width = _num(item.get("w"), 1, 40)
    out["w"] = 4 if width is None else width
    return out


def item_bytes(item):
    return len(json.dumps(item, separators=(",", ":"), ensure_ascii=False))


def new_board():
    return {"items": {}, "z": 0, "bytes": 0}


def load_board(items):
    board = new_board()
    for item in sorted((i for i in items if isinstance(i, dict) and i.get("id")),
                       key=lambda i: i.get("z") or 0):
        board["items"][item["id"]] = item
        board["bytes"] += item_bytes(item)
        board["z"] = max(board["z"], item.get("z") or 0)
    return board


def board_items(board):
    return list(board["items"].values())


def move_item(item, dx, dy):
    if item["k"] == "s":
        pts = item["pts"]
        for i in range(0, len(pts), 3):
            pts[i] = _clamp(pts[i] + dx, -50, BW + 50)
            pts[i + 1] = _clamp(pts[i + 1] + dy, -50, BH + 50)
        return
    item["x"] = _clamp(item["x"] + dx, 0, BW)
    item["y"] = _clamp(item["y"] + dy, 0, BH)
    if "x2" in item:
        item["x2"] = _clamp(item["x2"] + dx, 0, BW)
        item["y2"] = _clamp(item["y2"] + dy, 0, BH)


def apply_op(board, op, by=None):
    res = {"ok": False, "put": [], "del": [], "op": None, "err": None}
    if not isinstance(op, dict):
        res["err"] = "bad"
        return res
    kind = op.get("o")
    if kind == "add":
        items = op.get("items")
        if not isinstance(items, list) or not items or len(items) > WB_MAX_BATCH:
            res["err"] = "bad"
            return res
        added = []
        for raw in items:
            item = sanitize_item(raw)
            if not item or item["id"] in board["items"]:
                continue
            size = item_bytes(item) + 24
            if len(board["items"]) >= WB_MAX_ITEMS or \
                    board["bytes"] + size > WB_MAX_BYTES:
                res["err"] = "full"
                break
            board["z"] += 1
            item["z"] = board["z"]
            if by:
                item["by"] = js_str(by)[:12]
            board["items"][item["id"]] = item
            board["bytes"] += item_bytes(item)
            added.append(item)
        if not added:
            return res
        res["put"] = added
        res["op"] = {"o": "add", "items": added}
    elif kind == "del":
        ids = op.get("ids")
        if not isinstance(ids, list) or len(ids) > WB_MAX_IDS:
            res["err"] = "bad"
            return res
        gone = []
        for ident in ids:
            item = board["items"].pop(js_str(ident), None)
            if not item:
                continue
            board["bytes"] -= item_bytes(item)
            gone.append(item["id"])
        if not gone:
            return res
        res["del"] = gone
        res["op"] = {"o": "del", "ids": gone}
    elif kind == "mv":
        dx = _num(op.get("dx"), -BW, BW)
        dy = _num(op.get("dy"), -BH, BH)
        ids = op.get("ids")
        if not isinstance(ids, list) or len(ids) > WB_MAX_IDS or dx is None \
                or dy is None or (not dx and not dy):
            res["err"] = "bad"
            return res
        moved = []
        for ident in ids:
            item = board["items"].get(js_str(ident))
            if not item:
                continue
            before = item_bytes(item)
            move_item(item, dx, dy)
            board["bytes"] += item_bytes(item) - before
            moved.append(item["id"])
            res["put"].append(item)
        if not moved:
            return res
        res["op"] = {"o": "mv", "ids": moved, "dx": dx, "dy": dy}
    elif kind == "upd":
        item = board["items"].get(js_str(op.get("id")))
        if not item or item["k"] not in ("t", "n"):
            return res
        text = _wb_text(op.get("text"))
        if item["k"] == "t" and not text.strip():
            return res
        before = item_bytes(item)
        item["text"] = text.strip() if item["k"] == "t" else text
        board["bytes"] += item_bytes(item) - before
        res["put"] = [item]
        res["op"] = {"o": "upd", "id": item["id"], "text": item["text"]}
    elif kind == "clear":
        res["del"] = list(board["items"])
        board["items"].clear()
        board["bytes"] = 0
        res["op"] = {"o": "clear"}
    else:
        res["err"] = "bad"
        return res
    res["ok"] = True
    return res


def _text_width(item):
    return max(20, len(item["text"]) * item["sz"] * 0.56)


def item_bounds(item):
    if item["k"] == "s":
        pts = item["pts"]
        xs = pts[0::3]
        ys = pts[1::3]
        pad = (item["w"] * 3 if item.get("hl") else item["w"]) / 2 + 2
        return (min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad)
    if item["k"] == "t":
        return (item["x"], item["y"], item["x"] + _text_width(item),
                item["y"] + item["sz"] * 1.25)
    if item["k"] == "n":
        return (item["x"], item["y"], item["x"] + STICKY_W,
                item["y"] + STICKY_H)
    pad = item["w"] / 2 + 4
    return (min(item["x"], item["x2"]) - pad, min(item["y"], item["y2"]) - pad,
            max(item["x"], item["x2"]) + pad, max(item["y"], item["y2"]) + pad)


def _seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = _clamp(((px - ax) * dx + (py - ay) * dy) / length, 0, 1) if length else 0
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def simplify(pts, eps=1.2):
    count = len(pts) // 3
    if count <= 2:
        return list(pts)
    keep = [0] * count
    keep[0] = keep[count - 1] = 1
    stack = [(0, count - 1)]
    while stack:
        a, b = stack.pop()
        best, best_index = -1, -1
        for i in range(a + 1, b):
            d = _seg_dist(pts[i * 3], pts[i * 3 + 1], pts[a * 3],
                          pts[a * 3 + 1], pts[b * 3], pts[b * 3 + 1]) + \
                abs(pts[i * 3 + 2] - pts[a * 3 + 2]) * 0.4
            if d > best:
                best, best_index = d, i
        if best > eps:
            keep[best_index] = 1
            stack.append((a, best_index))
            stack.append((best_index, b))
    out = []
    for i in range(count):
        if keep[i]:
            out.extend(pts[i * 3:i * 3 + 3])
    return out


def thumb_of(board, limit=160):
    items = board_items(board)[-limit:]

    def q(value, size):
        return int(_clamp(js_round(value / size * 63), 0, 63))

    out = []
    for item in items:
        if item["k"] == "s":
            pts = simplify(item["pts"], 12)
            flat = []
            step = max(1, math.ceil(len(pts) / 3 / 12))
            for i in range(0, len(pts), 3 * step):
                flat.extend((q(pts[i], BW), q(pts[i + 1], BH)))
            flat.extend((q(pts[-3], BW), q(pts[-2], BH)))
            out.append(["s", item["c"], flat, 1 if item.get("hl") else 0])
        elif item["k"] == "t":
            x0, y0, x1, y1 = item_bounds(item)
            out.append(["t", item["c"],
                        [q(x0, BW), q(y0, BH), q(x1, BW), q(y1, BH)]])
        elif item["k"] == "n":
            out.append(["n", item["c"], [
                q(item["x"], BW), q(item["y"], BH),
                q(item["x"] + STICKY_W, BW), q(item["y"] + STICKY_H, BH)]])
        else:
            out.append([item["k"], item["c"], [
                q(item["x"], BW), q(item["y"], BH),
                q(item["x2"], BW), q(item["y2"], BH)]])
    return out


def chunk_items(items, max_chars=200000):
    chunks = []
    current = []
    size = 0
    for item in items:
        length = item_bytes(item)
        if current and size + length > max_chars:
            chunks.append(current)
            current = []
            size = 0
        current.append(item)
        size += length
    if current or not chunks:
        chunks.append(current)
    return chunks


def bucket(state, key, cap, rate, cost=1, now=None):
    now = now_ms() if now is None else now
    entry = state.get(key)
    if entry is None:
        entry = state[key] = {"t": float(cap), "at": now}
    entry["t"] = min(cap, entry["t"] + (now - entry["at"]) / 1000 * rate)
    entry["at"] = now
    if entry["t"] < cost:
        return False
    entry["t"] -= cost
    return True


CHAT_ROOMS = (
    {"ch": "general", "zone": None},
    {"ch": "engineering", "zone": "a"},
    {"ch": "design", "zone": "b"},
    {"ch": "coffee", "zone": "c"},
    {"ch": "lobby", "zone": "l"},
    {"ch": "garden", "zone": "p"},
)
CHAT_ROOM_IDS = frozenset(r["ch"] for r in CHAT_ROOMS)
CHAT_ROOM_ZONE = {r["ch"]: r["zone"] for r in CHAT_ROOMS}


def alias(key):
    text = js_str(key)
    return "%08x%08x" % (_fnv(text, 2166136261), _fnv(text, 3735928559))


TICK_MS = 33
USE_RADIUS = 2
ARCADE_MAX_PER_SEC = 3
ARCADE_MAX_SCORE = 5000
ARCADE_MAX_MS = 30 * 60 * 1000
SCORES_KEEP = 10
COOLER_RADIUS = 3
COURT_W = 2
COURT_H = 1
PAD_H = 0.3
PAD_X = 0.08
BALL_R = 0.035
PAD_SPEED = 2.6
SERVE_SPEED = 0.9
MAX_SPEED = 2.4
WIN_SCORE = 5
SERVE_DELAY_MS = 1200

ICEBREAKERS = (
    "What is a small thing that made your week better?",
    "Which tool could you not work without?",
    "What was your first job?",
    "If you could learn one skill overnight, what would it be?",
    "What is the best meal you have had this year?",
    "Which app on your phone do you open most?",
    "What is a hobby you picked up recently?",
    "Mountains or beach, and why?",
    "What song is stuck in your head right now?",
    "What is the most useful thing you learned from a teammate?",
    "Which book or article changed how you work?",
    "What would you build if you had a free week?",
    "Tea, coffee, or something else entirely?",
    "What is the best piece of advice you have been given?",
    "Where would you work from if you could pick anywhere?",
    "What was your favourite game growing up?",
    "What is one thing on your desk right now that has a story?",
    "Early bird or night owl?",
    "What is a bug you will never forget?",
    "Which city would you like to spend a month in?",
    "What is a small habit that helps you focus?",
    "What did you want to be when you were ten?",
    "What is the last thing you fixed around the house?",
    "Which emoji do you use far too often?",
    "What is a show or film you recommend to everyone?",
    "Keyboard shortcuts you swear by?",
    "What is something you are looking forward to this month?",
    "If our team had a mascot, what should it be?",
)


def plausible_score(score, ms):
    if not is_int(score) or score < 0 or score > ARCADE_MAX_SCORE:
        return False
    if not isinstance(ms, (int, float)) or ms < 0 or ms > ARCADE_MAX_MS:
        return False
    return score <= math.floor(ms / 1000 * ARCADE_MAX_PER_SEC) + 1


def insert_score(scores, entry, keep=SCORES_KEEP):
    if not entry["score"] > 0:
        return {"list": scores, "rank": 0, "top": False}
    prev_best = scores[0]["score"] if scores else -1
    mine = next((s for s in scores if s["k"] == entry["k"]), None)
    if mine and mine["score"] >= entry["score"]:
        return {"list": scores, "rank": 0, "top": False}
    updated = [s for s in scores if s["k"] != entry["k"]] + [entry]
    updated.sort(key=lambda s: (-s["score"], s["ts"]))
    updated = updated[:keep]
    rank = next((i + 1 for i, s in enumerate(updated) if s is entry), 0)
    return {"list": updated, "rank": rank,
            "top": rank == 1 and entry["score"] > prev_best}


def find_play_tables(char="G"):
    seen = set()
    out = []
    for y in range(H):
        for x in range(W):
            if OBJ[y][x] != char or (x, y) in seen:
                continue
            x1, y1 = x, y
            while x1 + 1 < W and OBJ[y][x1 + 1] == char:
                x1 += 1
            while y1 + 1 < H and OBJ[y1 + 1][x] == char:
                y1 += 1
            for yy in range(y, y1 + 1):
                for xx in range(x, x1 + 1):
                    seen.add((xx, yy))
            out.append({"id": "%d,%d" % (x, y), "x0": x, "y0": y,
                        "x1": x1, "y1": y1})
    return out


PONG_TABLES = find_play_tables("G")


def pong_side(table, x, y):
    if y < table["y0"] - 1 or y > table["y1"] + 1:
        return None
    if x in (table["x0"] - 1, table["x0"] - 2):
        return "L"
    if x in (table["x1"] + 1, table["x1"] + 2):
        return "R"
    return None


def near_table(table, x, y, radius):
    return (table["x0"] - radius <= x <= table["x1"] + radius
            and table["y0"] - radius <= y <= table["y1"] + radius)


def near_tile(x, y, char, radius):
    best = None
    best_distance = math.inf
    for yy in range(y - radius, y + radius + 1):
        for xx in range(x - radius, x + radius + 1):
            if obj_at(xx, yy) != char:
                continue
            distance = abs(xx - x) + abs(yy - y)
            if distance <= radius and distance < best_distance:
                best_distance = distance
                best = {"x": xx, "y": yy}
    return best


def pick_question(prev, rng=random):
    question = prev
    while question == prev:
        question = ICEBREAKERS[int(rng.random() * len(ICEBREAKERS))
                               % len(ICEBREAKERS)]
    return question


def day_key(ts):
    return time.strftime("%Y-%m-%d", time.gmtime(ts / 1000))


def new_pong(table):
    return {
        "table": table,
        "players": {"L": None, "R": None},
        "names": {"L": "", "R": ""},
        "pad": {"L": COURT_H / 2, "R": COURT_H / 2},
        "target": {"L": COURT_H / 2, "R": COURT_H / 2},
        "ball": {"x": COURT_W / 2, "y": COURT_H / 2, "vx": 0.0, "vy": 0.0},
        "score": {"L": 0, "R": 0},
        "status": "wait",
        "serveAt": 0,
        "winner": None,
    }


def serve(game, toward, now, rng=random):
    angle = (rng.random() - 0.5) * (math.pi / 3)
    direction = -1 if toward == "L" else 1
    game["ball"] = {
        "x": COURT_W / 2, "y": COURT_H / 2,
        "vx": direction * SERVE_SPEED * math.cos(angle),
        "vy": SERVE_SPEED * math.sin(angle),
    }
    game["serveAt"] = now + SERVE_DELAY_MS


def start_pong(game, now, rng=random):
    game["score"] = {"L": 0, "R": 0}
    game["pad"] = {"L": COURT_H / 2, "R": COURT_H / 2}
    game["target"] = {"L": COURT_H / 2, "R": COURT_H / 2}
    game["status"] = "play"
    game["winner"] = None
    serve(game, "L" if rng.random() < 0.5 else "R", now, rng)


def set_pad(game, side, value):
    number = finite(value)
    if number is None or side not in ("L", "R"):
        return
    game["target"][side] = _clamp(number, PAD_H / 2, COURT_H - PAD_H / 2)


def _bounce(game, side):
    ball = game["ball"]
    offset = _clamp((ball["y"] - game["pad"][side]) / (PAD_H / 2), -1, 1)
    speed = min(MAX_SPEED, math.hypot(ball["vx"], ball["vy"]) * 1.07)
    angle = offset * (math.pi * 55 / 180)
    direction = 1 if side == "L" else -1
    ball["vx"] = direction * speed * math.cos(angle)
    ball["vy"] = speed * math.sin(angle)


def step_pong(game, dt, now, rng=random):
    if game["status"] != "play":
        return None
    limit = PAD_SPEED * dt
    for side in ("L", "R"):
        game["pad"][side] += _clamp(
            game["target"][side] - game["pad"][side], -limit, limit)
    if now < game["serveAt"]:
        return None
    ball = game["ball"]
    steps = max(1, math.ceil(dt / 0.008))
    h = dt / steps
    for _ in range(steps):
        px = ball["x"]
        ball["x"] += ball["vx"] * h
        ball["y"] += ball["vy"] * h
        if ball["y"] < BALL_R:
            ball["y"] = 2 * BALL_R - ball["y"]
            ball["vy"] = abs(ball["vy"])
        if ball["y"] > COURT_H - BALL_R:
            ball["y"] = 2 * (COURT_H - BALL_R) - ball["y"]
            ball["vy"] = -abs(ball["vy"])
        left = PAD_X + BALL_R
        right = COURT_W - PAD_X - BALL_R
        if ball["vx"] < 0 and px >= left and ball["x"] < left and \
                abs(ball["y"] - game["pad"]["L"]) <= PAD_H / 2 + BALL_R:
            ball["x"] = 2 * left - ball["x"]
            _bounce(game, "L")
        elif ball["vx"] > 0 and px <= right and ball["x"] > right and \
                abs(ball["y"] - game["pad"]["R"]) <= PAD_H / 2 + BALL_R:
            ball["x"] = 2 * right - ball["x"]
            _bounce(game, "R")
        if ball["x"] < 0 or ball["x"] > COURT_W:
            scorer = "R" if ball["x"] < 0 else "L"
            game["score"][scorer] += 1
            if game["score"][scorer] >= WIN_SCORE:
                game["status"] = "over"
                game["winner"] = scorer
                game["ball"] = {"x": COURT_W / 2, "y": COURT_H / 2,
                                "vx": 0.0, "vy": 0.0}
            else:
                serve(game, "R" if scorer == "L" else "L", now, rng)
            return scorer
    return None


def pack_pong(game, now=None):
    now = now_ms() if now is None else now

    def r(value):
        return js_round(value * 1000) / 1000

    return {
        "id": game["table"]["id"],
        "s": game["status"],
        "b": [r(game["ball"]["x"]), r(game["ball"]["y"])],
        "p": [r(game["pad"]["L"]), r(game["pad"]["R"])],
        "sc": [game["score"]["L"], game["score"]["R"]],
        "ids": [game["players"]["L"], game["players"]["R"]],
        "n": [game["names"]["L"], game["names"]["R"]],
        "w": game["winner"],
        "wait": 1 if game["status"] == "play" and game["serveAt"] > now else 0,
    }


def may_connect(a, b):
    za = zone_at(a["x"], a["y"])
    zb = zone_at(b["x"], b["y"])
    ta = ((a.get("ex") or {}).get("meeting") or {}).get("table")
    tb = ((b.get("ex") or {}).get("meeting") or {}).get("table")
    if ta and ta == tb:
        return True
    if is_private(za) or is_private(zb):
        return za == zb
    return max(abs(a["x"] - b["x"]), abs(a["y"] - b["y"])) <= FAR + SIGNAL_SLACK


def chat_channel_zone(channel):
    return CHAT_ROOM_ZONE.get(channel)
