"""What stands on this instance's World hexagon, for a neighbouring World.

ForkMesh's World draws this instance as one hexagon beside its town and, when
it can, reads this description to draw the hexagon as it is laid out here:
ground, plaza, sign, houses and the bug terrarium. It is plain data in the
neighbour-scene v1 shape (world/public/world/world-neighbour-scene.js on the
hub); positions are relative to the hexagon centre, in the World's own axes.
"""
import json
import math
from urllib.parse import quote, urlencode

VERSION = 1
PATH = "/api/world/neighbour-scene"
MAX_BUGS = 32
CACHE_SECONDS = 600
# The territory's centre and plaza (world-territory.js TERRITORY_PLAZA and
# territory-geometry.js TERRITORY_CENTER): the plaza sits 85 in front.
PLAZA = (0.0, 85.0)
PLAZA_RADIUS = 26
SIGN = (0.0, -22.0)
LAMPS = ((-20, -14), (20, -14), (-20, 14), (20, 14))
STYLES = {
    "security": {"ground": "#3a4550", "plaza": "#46525d", "lamp": "#9ff5e1",
                 "pattern": "circuit", "text": "> secure by design_"},
    "meadow": {"ground": "#8fbf72", "plaza": "#d8d0bf", "lamp": "#fff3c4",
               "pattern": "plain", "text": "Welcome to our World"},
}


def _round(value):
    return round(float(value), 1)


def _facing(x, z):
    return round(math.atan2(-x, -z), 4) if (x or z) else 0.0


def _placed(kind, x, z, **extra):
    item = {"type": kind, "x": _round(PLAZA[0] + x), "z": _round(PLAZA[1] + z)}
    item.update(extra)
    return item


def raw_url(origin, bugs, path):
    owner, _, name = str(bugs.get("repo") or "").partition("/")
    query = urlencode({"path": path, "ref": bugs.get("ref") or "main"})
    return "%s/api/repo/%s/%s/raw?%s" % (
        origin, quote(owner, safe=""), quote(name, safe=""), query)


def bug_entries(origin, bugs, listing, limit=MAX_BUGS):
    folder = str(bugs.get("path") or "").rsplit("/", 1)[0]
    items = listing if isinstance(listing, list) else (
        listing.get("bugs") if isinstance(listing, dict) else [])
    out = []
    for item in items or []:
        if len(out) >= limit:
            break
        if not isinstance(item, dict):
            continue
        domain = str(item.get("domain") or "").strip().lower()[:40]
        if not domain:
            continue
        favicon = str(item.get("favicon") or "")
        icon = ""
        if favicon and ":" not in favicon and not favicon.startswith("/") \
                and ".." not in favicon:
            icon = raw_url(origin, bugs,
                           "%s/%s" % (folder, favicon) if folder else favicon)
        url = str(item.get("url") or "")
        out.append({"label": domain, "icon": icon,
                    "url": url if url.startswith("https://") else ""})
    return out


def build(name, world, origin, accent="", listing=None):
    world = world if isinstance(world, dict) else {}
    style = STYLES.get(world.get("style")) or STYLES["meadow"]
    accent = str(world.get("accent") or accent or "#3b82f6")
    objects = [
        {"type": "plaza", "x": PLAZA[0], "z": PLAZA[1],
         "radius": PLAZA_RADIUS, "color": style["plaza"]},
        _placed("sign", *SIGN, title=str(name or "")[:40],
                text=style["text"]),
    ]
    objects += [_placed("lamp", x, z, color=style["lamp"]) for x, z in LAMPS]
    for item in world.get("objects") or []:
        if not isinstance(item, dict) or item.get("type") != "house":
            continue
        x, z = (list(item.get("position") or []) + [0, 0])[:2]
        objects.append(_placed(
            "house", x, z, facing=_facing(x, z),
            label=str(item.get("label") or "")[:40],
            url=str(item.get("url") or ""),
            color=str(item.get("color") or accent)))
    bugs = world.get("bugs") if isinstance(world.get("bugs"), dict) else None
    if bugs:
        x, z = (list(bugs.get("position") or [70, 40]) + [0, 0])[:2]
        limit = min(MAX_BUGS, int(bugs.get("limit") or MAX_BUGS))
        objects.append(_placed(
            "terrarium", x, z, facing=_facing(x, z),
            label=str(bugs.get("title") or "Bug terrarium")[:40],
            bugs=bug_entries(origin, bugs, listing, limit)))
    return {
        "v": VERSION,
        "name": str(name or "")[:40],
        "ground": {"color": style["ground"], "accent": accent,
                   "pattern": style["pattern"]},
        "objects": objects,
    }


def dumps(scene):
    return json.dumps(scene, separators=(",", ":"))
