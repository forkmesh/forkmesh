#!/usr/bin/env python3
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JS_OUT = ROOT / "office" / "public" / "park" / "layout.js"
PY_OUT = ROOT / "app" / "src" / "org_office_park.py"

PARK_X0 = 72
PARK_W = 100
PARK_H = 44

OFFICE_PARKING = (59, 14, 63, 19)
OFFICE_CAR = (61, 16)
OFFICE_ARRIVAL = (60, 18)

WALKABLE_OBJ = {"", "W", "h"}
FLUSH_BOWL = (2, 30, 7, 35)


def build():
    floor = [["g"] * PARK_W for _ in range(PARK_H)]
    obj = [[""] * PARK_W for _ in range(PARK_H)]

    def fl(x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                floor[y][x] = c

    def ob(x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                obj[y][x] = c

    fl(1, 1, 98, 42, "Z")

    fl(11, 1, 37, 16, "B")
    ob(12, 2, 36, 11, "W")
    for row in (13, 15):
        for x in range(13, 36):
            obj[row][x] = "m" if (x - 13) % 3 == 2 else "h"

    ob(33, 18, 37, 22, "W")

    fl(42, 11, 60, 27, "V")
    for y in range(9, 30):
        for x in range(40, 63):
            ring = y in (9, 10, 28, 29) or x in (40, 41, 61, 62)
            if ring:
                obj[y][x] = "~"
    for x0, y0 in ((50, 9), (50, 28), (40, 18), (61, 18)):
        ob(x0, y0, x0 + 1, y0 + 1, "")
    for y in range(13, 26):
        for x in range(44, 59):
            if ((x - 51) / 7.5) ** 2 + ((y - 19) / 6.5) ** 2 <= 1:
                obj[y][x] = "q"

    fl(64, 2, 84, 8, "A")
    ob(64, 2, 84, 3, "k")
    ob(64, 7, 84, 8, "k")

    ob(92, 2, 96, 4, "z")
    ob(92, 5, 95, 24, "=")
    ob(90, 25, 97, 28, "W")

    ob(66, 12, 72, 17, "q")
    ob(66, 18, 76, 23, "W")
    ob(77, 11, 83, 17, "b")

    ob(66, 35, 74, 39, "f")
    ob(80, 36, 86, 40, "W")

    fl(12, 26, 34, 41, "K")
    ob(20, 30, 23, 33, "i")
    ob(26, 35, 33, 40, "W")

    ob(46, 34, 50, 36, "w")
    for x0 in (39, 53, 58):
        ob(x0, 35, x0 + 2, 36, "a")
    for x in range(38, 63):
        obj[39][x] = "m" if (x - 38) % 3 == 2 else "h"

    ob(*FLUSH_BOWL, "l")
    ob(2, 37, 8, 41, "W")

    palms = [(x, 1) for x in range(2, 98, 4)] + [(x, 42) for x in range(2, 98, 4)]
    palms += [(10, 3), (10, 9), (38, 3), (38, 9), (10, 26), (35, 26), (10, 33),
              (35, 33), (63, 12), (63, 25), (87, 10), (87, 20), (78, 26), (88, 34),
              (64, 33), (44, 31), (58, 31), (24, 24), (30, 24), (2, 12), (2, 30)]
    for x, y in palms:
        if floor[y][x] in ("Z", "B", "K") and obj[y][x] == "":
            obj[y][x] = "p"

    obj[20][3] = "e"
    return floor, obj


def P(x, y):
    return [x + PARK_X0, y]


def rides():
    def rect(x0, y0, x1, y1):
        return [x0 + PARK_X0, y0, x1 + PARK_X0, y1]

    out = {
        "drive-park": {"gate": list(OFFICE_CAR), "board": list(OFFICE_PARKING),
                       "exit": P(6, 21)},
        "drive-office": {"gate": P(3, 20), "board": rect(2, 18, 6, 23),
                         "exit": list(OFFICE_ARRIVAL)},
        "lazy": {"gate": P(42, 20), "board": rect(42, 21, 43, 22), "exit": None},
    }
    summit = {"gate": P(56, 26), "board": rect(46, 26, 56, 27)}
    out["drop"] = dict(summit, exit=P(35, 20))
    out["funnel"] = dict(summit, exit=P(75, 20))
    out["blaster"] = dict(summit, exit=P(83, 38))
    for lane in range(4):
        out["race%d" % (lane + 1)] = {"gate": P(90, 4), "board": rect(88, 3, 91, 6),
                                      "exit": P(92 + lane, 26)}
    out["cliff"] = {"gate": P(74, 13), "board": rect(73, 12, 75, 16), "exit": P(71, 20)}
    out["flush"] = {"gate": P(9, 31), "board": rect(8, 28, 9, 33), "exit": P(5, 39)}
    out["surf"] = {"gate": P(76, 36), "board": rect(75, 35, 77, 39), "exit": P(76, 38)}
    return out


def check(floor, obj, ride_map):
    def walkable_park(x, y):
        px = x - PARK_X0
        if not (0 <= px < PARK_W and 0 <= y < PARK_H):
            return None
        return floor[y][px] not in ("g", "#") and obj[y][px] in WALKABLE_OBJ

    for rid, ride in ride_map.items():
        gx, gy = ride["gate"]
        if gx >= PARK_X0:
            assert obj[gy][gx - PARK_X0] in ("", "y", "e"), (rid, "gate", obj[gy][gx - PARK_X0])
        x0, y0, x1, y1 = ride["board"]
        free = [(x, y) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)
                if (x, y) != (gx, gy) and walkable_park(x, y) is not False]
        assert free, (rid, "board")
        if ride["exit"] and ride["exit"][0] >= PARK_X0:
            assert walkable_park(*ride["exit"]), (rid, "exit")


def gates(obj, ride_map):
    for ride in ride_map.values():
        gx, gy = ride["gate"]
        if gx >= PARK_X0 and obj[gy][gx - PARK_X0] == "":
            obj[gy][gx - PARK_X0] = "y"


def emit():
    floor, obj = build()
    ride_map = rides()
    gates(obj, ride_map)
    check(floor, obj, ride_map)
    frows = ["".join(r) for r in floor]
    orows = ["".join(c or "_" for c in r) for r in obj]
    js = ["export const PARK_X0 = %d;" % PARK_X0,
          "export const PARK_W = %d;" % PARK_W,
          "export const PARK_H = %d;" % PARK_H,
          "export const OFFICE_PARKING = %s;" % json.dumps(list(OFFICE_PARKING)),
          "export const OFFICE_CAR = %s;" % json.dumps(list(OFFICE_CAR)),
          "export const PARK_FLOOR = ["]
    js += ["  %s," % json.dumps(r) for r in frows]
    js += ["];", "export const PARK_OBJ = ["]
    js += ["  %s," % json.dumps(r) for r in orows]
    js += ["];", "export const RIDES = {"]
    for rid, ride in ride_map.items():
        js.append("  %s: %s," % (json.dumps(rid), json.dumps(ride, separators=(", ", ": "))))
    js += ["};", ""]
    py = ["PARK_X0 = %d" % PARK_X0, "PARK_W = %d" % PARK_W, "PARK_H = %d" % PARK_H,
          "OFFICE_PARKING = %r" % (OFFICE_PARKING,), "OFFICE_CAR = %r" % (OFFICE_CAR,),
          "PARK_FLOOR = ("]
    py += ["    %r," % r for r in frows]
    py += [")", "PARK_OBJ = ("]
    py += ["    %r," % r for r in orows]
    py += [")", "RIDES = {"]
    for rid, ride in ride_map.items():
        exit_ = tuple(ride["exit"]) if ride["exit"] else None
        py.append("    %r: {\"gate\": %r, \"board\": %r, \"exit\": %r}," % (
            rid, tuple(ride["gate"]), tuple(ride["board"]), exit_))
    py += ["}", ""]
    return "\n".join(js), "\n".join(py)


def main(argv):
    js, py = emit()
    if "--check" in argv:
        stale = [p for p, text in ((JS_OUT, js), (PY_OUT, py))
                 if not p.exists() or p.read_text(encoding="utf-8") != text]
        for p in stale:
            print("stale: %s" % p.relative_to(ROOT))
        return 1 if stale else 0
    JS_OUT.parent.mkdir(parents=True, exist_ok=True)
    JS_OUT.write_text(js, encoding="utf-8")
    PY_OUT.write_text(py, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
