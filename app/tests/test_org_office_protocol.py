import json
import random
import shutil
import subprocess
from pathlib import Path

import pytest

import org_office as o

ROOT = Path(__file__).resolve().parents[2]
DUMP = ROOT / "office" / "tests" / "parity_dump.mjs"


class Seq:
    def __init__(self, seed):
        self.seed = float(seed)

    def random(self):
        self.seed = (self.seed * 1103515245.0 + 12345.0) % 2147483648.0
        return self.seed / 2147483648


@pytest.fixture(scope="module")
def js():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    out = subprocess.run([node, str(DUMP)], capture_output=True, text=True,
                         check=True, timeout=60)
    return json.loads(out.stdout)


def test_map_matches_the_client(js):
    assert (js["W"], js["H"]) == (o.W, o.H)
    assert js["floor"] == ["".join(row) for row in o.FLOOR]
    assert js["obj"] == ["".join(c or "_" for c in row) for row in o.OBJ]
    walk = ["".join("1" if o.is_walkable(x, y) else "0" for x in range(o.W))
            for y in range(o.H)]
    assert js["walk"] == walk
    assert js["zoneOut"] == [o.zone_at(-1, 0), o.zone_at(0, 99)]


def test_furniture_matches_the_client(js):
    assert js["desks"] == o.DESKS
    assert js["boards"] == o.BOARDS
    assert js["tables"] == [
        {"id": t["id"], "chairs": [list(c) for c in t["chairs"]]}
        for t in o.MEETING_TABLES]
    chair = o.MEETING_TABLES[0]["chairs"][0]
    assert js["seatTable"] == [o.table_at_seat(*chair), o.table_at_seat(0, 0)]
    assert js["pongTables"] == o.PONG_TABLES


def test_validators_match_the_client(js):
    statuses = [
        {"s": "focus", "emoji": "🎧", "text": "  deep   work\n now "},
        {"s": "away", "auto": True},
        {"s": "break", "emoji": "👍🏽x"},
        {"s": "focus", "emoji": "a"},
        {"s": "nope"},
        {"s": "meeting", "text": "x" * 90},
        {"s": "available", "emoji": "👨‍👩‍👧"},
        None,
        [],
    ]
    assert js["statuses"] == [o.clean_status(s) for s in statuses]
    assert js["workText"] == o.clean_work_text(" <b>x</b>\n  y ", 5)
    assert js["task"] == [
        o.clean_task({"id": "T-1", "title": " <hi> "}),
        o.clean_task({"id": "bad id", "title": "x"}),
        o.clean_task({"id": "x", "title": "  "}),
    ]
    first = o.DESKS[0]
    assert js["claim"] == o.claim_desk({}, "a:1", "Ann", first["x"], first["y"])
    assert js["relay"] == o.clean_relay([
        {"id": "sess-1", "st": "running", "at": 1000},
        {"id": "x", "st": "bogus", "at": 1},
        {"id": "y", "st": "failed", "at": 1.5},
        {"id": "z", "st": "failed", "at": 99999999999999},
    ], 5000)


def test_profile_validators_match_the_client(js):
    names = ["  Kaif   Ansari \n", "<b>x</b>", "x" * 40, None, "\u0000\u0007",
             "Zoë Ünïcode", 42]
    zones = ["Asia/Calcutta", " America/Argentina/Buenos_Aires ", "Etc/GMT+5",
             "UTC", "a/b/c/d", "Asia/Cal cutta", "<script>", "x" * 80, None,
             "9/x"]
    pics = ["0123456789abcdef", "0123456789ABCDEF", "short", None,
            1234567890123456]
    assert js["profile"] == {
        "names": [o.clean_display_name(n) for n in names],
        "zones": [o.clean_tz(z) for z in zones],
        "pics": [o.clean_pic(p) for p in pics],
    }
    assert o.clean_office_profile({"name": " Ann ", "tz": "bad zone",
                                   "x": 1}) == {"name": "Ann"}
    assert o.clean_office_profile(None) == {}


def test_focus_sessions_match_the_client(js):
    cases = [
        {"mode": "focused", "mins": 60, "topic": "  ship <the> thing "},
        {"mode": "focused", "mins": 61},
        {"mode": "focused", "mins": "60"},
        {"mode": "focused"},
        {"mode": "pomodoro", "rounds": 2, "topic": "x" * 90},
        {"mode": "pomodoro", "rounds": 0},
        {"mode": "pomodoro", "rounds": 7},
        {"mode": "pomodoro", "rounds": 1.5},
        {"mode": "freeform", "mins": 15, "rounds": 3},
        {"mode": "nap"},
        None,
        [],
        {"mode": "freeform", "emoji": "🚀"},
        {"mode": "focused", "mins": 25, "emoji": "ab", "topic": "x"},
        {"mode": "freeform", "emoji": "👨‍💻 code"},
        {"mode": "freeform", "emoji": 7},
        {"mode": "freeform", "emoji": ""},
    ]
    assert o.clean_focus(cases[-5], True, 5) == {
        "mode": "freeform", "at": 5, "emoji": "🚀"}
    assert "emoji" not in o.clean_focus(cases[-4], True, 5)
    assert o.clean_focus(cases[-3], True, 5)["emoji"] == "👨‍💻"
    assert o.join_focus(o.clean_focus(cases[-5], True, 5), "h", True, 9)[
        "emoji"] == "🚀"
    host = o.clean_focus({"mode": "pomodoro", "rounds": 2,
                          "topic": "deep work"}, True, 1000)
    assert host == {"mode": "pomodoro", "at": 1000, "topic": "deep work",
                    "rounds": 2, "end": 1000 + 55 * 60000}
    assert js["focus"] == {
        "clean": [o.clean_focus(v, True, 5000) for v in cases],
        "standing": o.clean_focus(cases[0], False, 5000),
        "join": [
            o.join_focus(host, "host1", True, 2000),
            o.join_focus(host, "host1", False, 2000),
            o.join_focus(host, "host1", True, host["end"]),
            o.join_focus({**host, "host": "root9"}, "host1", True, 2000),
            o.join_focus(None, "host1", True, 2000),
            o.join_focus({"mode": "freeform", "at": 5}, "h", True, 10 ** 12),
        ],
    }
    assert js["couches"] == {
        "chars": sorted(o.COUCHES),
        "spots": [o.is_couch(x, y) for x, y in
                  ((36, 33), (36, 37), (34, 34), (17, 24), (15, 24), (-1, 0))],
    }
    assert js["couches"]["spots"] == [True, True, True, True, False, False]


def test_desk_decorations_match_the_client(js):
    cases = [
        {"desk": 1, "chair": 7, "slots": [1, 8, 11, 14]},
        {"slots": [0, 0, 0, 0]},
        {"desk": None, "chair": None, "slots": [3, 0, 0, 0]},
        {"desk": 6, "slots": [0, 0, 0, 0]},
        {"chair": 8, "slots": [0, 0, 0, 0]},
        {"slots": [9, 0, 0, 0]},
        {"slots": [0, 0, 1, 0]},
        {"slots": [0, 0, 0, 15]},
        {"slots": [0, 0, 0]},
        {"slots": [1.5, 0, 0, 0]},
        {"desk": "1", "slots": [0, 0, 0, 0]},
        {"slots": "0000"},
        None,
        [],
    ]
    first = o.clean_deco(cases[0])
    assert first == {"desk": 1, "chair": 7, "slots": [1, 8, 11, 14]}
    assert js["deco"] == {
        "desks": o.DECO_DESKS, "chairs": o.DECO_CHAIRS,
        "slots": list(o.DECO_SLOTS), "kinds": list(o.DECO_ITEMS),
        "clean": [o.clean_deco(v) for v in cases],
        "empty": [o.deco_empty(o.clean_deco(v)) for v in cases],
        "list": o.desk_list({
            "a:1": {"x": 1, "y": 2, "name": "Ann", "deco": first},
            "a:2": {"x": 3, "y": 4, "name": "Bo"}}),
    }


def test_avatar_codes_match_the_client(js):
    avatars = [0, 9999, 10000, o.AV_CUSTOM - 1, o.AV_CUSTOM,
               o.AV_CUSTOM + o.AV_COMBOS - 1, o.AV_CUSTOM + o.AV_COMBOS, -1,
               1.5, 7.0, "7", None, True]
    assert js["avatar"] == {
        "max": o.AV_MAX, "custom": o.AV_CUSTOM, "combos": o.AV_COMBOS,
        "fields": list(o.AV_FIELDS),
        "clean": [o.clean_avatar(v) for v in avatars],
    }
    assert o.clean_office_profile({"av": o.AV_CUSTOM + 5, "tz": "UTC"}) == {
        "tz": "UTC", "av": o.AV_CUSTOM + 5}
    assert o.clean_office_profile({"av": 10 ** 9}) == {}


def test_whiteboard_ops_match_the_client(js):
    items = [
        {"id": "a1", "k": "s", "c": "#AABBCC",
         "pts": [1.4, 2.6, 3, 5000, -900, 12, 7, 8], "w": 99, "hl": 1},
        {"id": "b2", "k": "t", "c": "red", "x": 10.5, "y": 20,
         "text": " hi\u0001 there ", "sz": 5},
        {"id": "c3", "k": "n", "x": 1700, "y": -4, "text": "note"},
        {"id": "d4", "k": "r", "x": 1, "y": 2, "x2": 3000, "y2": 4, "w": 0},
        {"id": "bad id", "k": "s", "pts": [1, 2, 3]},
        {"id": "e5", "k": "a", "x": 1, "y": 2},
    ]
    assert js["items"] == [o.sanitize_item(i) for i in items]
    board = o.new_board()
    ops = [
        {"o": "add", "items": items},
        {"o": "mv", "ids": ["a1", "b2", "zz"], "dx": 12.5, "dy": -3},
        {"o": "upd", "id": "b2", "text": "changed"},
        {"o": "upd", "id": "b2", "text": "   "},
        {"o": "del", "ids": ["c3"]},
        {"o": "nope"},
    ]
    results = []
    for op in ops:
        r = o.apply_op(board, json.loads(json.dumps(op)), "user-123456789")
        results.append({"ok": r["ok"], "err": r["err"], "op": r["op"],
                        "put": [i["id"] for i in r["put"]], "del": r["del"]})
    assert json.loads(json.dumps(results)) == js["opResults"]
    assert board["bytes"] == js["boardBytes"]
    assert o.thumb_of(board) == js["thumb"]
    assert [len(c) for c in o.chunk_items(o.board_items(board), 200)] == \
        js["chunks"]


def test_play_rules_match_the_client(js):
    assert js["plausible"] == [[
        o.plausible_score(4, 1000), o.plausible_score(5, 1000),
        o.plausible_score(1.5, 1000), o.plausible_score(1, -1)]]
    inserted = o.insert_score(
        [{"k": "a", "name": "A", "score": 5, "ts": 1},
         {"k": "b", "name": "B", "score": 3, "ts": 2}],
        {"k": "c", "name": "C", "score": 9, "ts": 3})
    assert js["insert"] == inserted
    table = o.PONG_TABLES[0]
    assert js["pongSide"] == [o.pong_side(table, 22, 11),
                              o.pong_side(table, 29, 12),
                              o.pong_side(table, 25, 11)]
    assert js["nearTile"] == [o.near_tile(22, 10, "Q", 2),
                              o.near_tile(1, 1, "Q", 2),
                              o.near_tile(0, 0, "E", 3)]
    assert js["dayKey"] == o.day_key(1759190400000)


def test_pong_physics_match_the_client(js):
    rng = Seq(42)
    game = o.new_pong(o.PONG_TABLES[0])
    o.start_pong(game, 1000, rng)
    o.set_pad(game, "L", 0.2)
    o.set_pad(game, "R", "x")
    trace = []
    for i in range(400):
        if game["status"] != "play":
            break
        now = 2300 + i * o.TICK_MS
        point = o.step_pong(game, o.TICK_MS / 1000, now, rng)
        if i % 25 == 0 or point:
            trace.append({"i": i, "pt": point, "p": o.pack_pong(game, now)})
    assert len(trace) == len(js["pongTrace"])
    for mine, theirs in zip(trace, js["pongTrace"]):
        assert mine["i"] == theirs["i"] and mine["pt"] == theirs["pt"]
        for key in ("s", "sc", "w", "wait", "id"):
            assert mine["p"][key] == theirs["p"][key]
        for key in ("b", "p"):
            assert mine["p"][key] == pytest.approx(theirs["p"][key], abs=2e-3)


def test_attendance_windows_start_weeks_on_monday():
    monday = 1790553600000
    now = monday + 2 * o.DAY + 10 * 3600000
    assert o.windows(now, 0) == {"day": monday + 2 * o.DAY, "week": monday}
    ist = o.windows(monday + 2 * o.DAY + 20 * 3600000, -330)
    assert ist["day"] == monday + 3 * o.DAY - 330 * 60000


def test_attendance_merges_blips_and_ranks_people():
    assert o.merge_visits([[0, 100, False], [150, 300, False],
                           [10 ** 6, 2 * 10 ** 6, True]], 90) == \
        [[0, 300, False], [10 ** 6, 2 * 10 ** 6, True]]
    monday = 1790553600000
    hour = 3600000
    now = monday + 2 * o.DAY + 12 * hour
    rows = [
        {"who": "a:1", "name": "Ana", "started": monday + 9 * hour,
         "ended": monday + 17 * hour},
        {"who": "a:1", "name": "Ana B", "started": now - 3 * hour,
         "ended": now - 2 * hour},
        {"who": "a:2", "name": "Bo", "started": now - hour, "ended": None},
        {"who": "a:3", "name": "Old", "started": monday - 3 * o.DAY,
         "ended": monday - 3 * o.DAY + hour},
    ]
    summary = o.summarize(rows, now, 0, "a:1")
    assert [(p["name"], p["ms"]) for p in summary["today"]] == \
        [("Ana B", hour), ("Bo", hour)]
    assert [(p["name"], p["ms"]) for p in summary["thisWeek"]] == \
        [("Ana B", 9 * hour), ("Bo", hour)]
    assert summary["me"] == {"today": hour, "week": 9 * hour}
    bo = next(p for p in summary["inToday"] if p["name"] == "Bo")
    assert bo["online"] and bo["visits"] == [[now - hour, None]]
    assert summary["inToday"][0]["me"] is True


def test_team_gates_and_signalling_range():
    assert o.unlocked_zones(["Platform"], "member") == ["a"]
    assert o.unlocked_zones([], "owner") == ["a", "b"]
    assert o.unlocked_zones(["marketing"], "member") == []
    profile = {"teams": ["design"], "role": "member"}
    assert o.may_enter(profile, "b") and not o.may_enter(profile, "a")
    assert o.may_enter(profile, "a", {"a"})
    assert o.may_enter(profile, "c") and o.may_enter(profile, ".")
    hall = {"x": 25, "y": 20, "ex": {}}
    assert o.zone_at(25, 20) == "."
    assert o.may_connect(hall, {"x": 30, "y": 20, "ex": {}})
    assert not o.may_connect(hall, {"x": 34, "y": 20, "ex": {}})
    engineering = {"x": 10, "y": 12, "ex": {}}
    assert o.zone_at(10, 12) == "a"
    assert not o.may_connect(engineering, {"x": 10, "y": 16, "ex": {}})
    assert o.may_connect(engineering, {"x": 18, "y": 9, "ex": {}})
    table = o.MEETING_TABLES[0]
    a = {"x": 0, "y": 0, "ex": {"meeting": {"table": table["id"]}}}
    b = {"x": 50, "y": 30, "ex": {"meeting": {"table": table["id"]}}}
    assert o.may_connect(a, b)


def test_spawn_is_walkable_and_statuses_are_bounded():
    rng = random.Random(3)
    for _ in range(50):
        spot = o.random_spawn(rng)
        assert o.is_walkable(spot["x"], spot["y"])
    assert o.clean_status({"s": "focus", "emoji": "<"}) is None
    assert o.clean_status({"s": "focus", "text": "<b>x</b>"})["text"] == \
        "<b>x</b>"


def test_library_media_room_and_lobby_couches():
    assert [o.ZONES[z]["name"] for z in "kms"] == [
        "Library", "Media Room", "Lobby Couches"]
    assert all(o.is_private(z) for z in "kms")
    assert not any(z in o.GATED_ZONES for z in "kms")
    profile = {"teams": [], "role": "member"}
    assert all(o.may_enter(profile, z) for z in "kms")
    assert [o.zone_at(*p) for p in ((40, 24), (43, 27), (38, 24))] == \
        ["s", "s", "l"]
    assert o.chair_dir(40, 24) == 0 and o.chair_dir(43, 27) == 0
    assert o.is_walkable(40, 24) and not o.is_walkable(40, 25)
    couch = {"x": 40, "y": 24, "ex": {}}
    assert o.may_connect(couch, {"x": 43, "y": 27, "ex": {}})
    assert not o.may_connect(couch, {"x": 38, "y": 24, "ex": {}})
    assert o.table_at_seat(40, 24) is None
    assert o.zone_at(44, 28) == "m" and o.is_walkable(44, 28)
    assert o.zone_at(51, 16) == "k" and o.is_walkable(51, 16)
    chairs = {t["id"]: len(t["chairs"]) for t in o.MEETING_TABLES}
    assert chairs["m-50-26"] == 10
    assert chairs["k-47-12"] == chairs["k-55-12"] == 4
    rng = random.Random(7)
    assert {o.zone_at(**o.random_spawn(rng)) for _ in range(60)} == {"l"}
