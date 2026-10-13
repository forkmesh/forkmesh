import asyncio
import base64
import json
import sqlite3
from pathlib import Path

import pytest

import org_office as o
import org_office_room as r

MIGRATIONS = [Path(__file__).resolve().parents[1] / "migrations" / name
              for name in ("0226_org_office.sql",
                           "0227_org_office_site_chat.sql")]
ORG_A = "a" * 64
ORG_B = "b" * 64


class Clock:
    def __init__(self):
        self.t = 1_800_000_000_000
        self.timers = []

    def __call__(self):
        return self.t

    def schedule(self, delay, callback):
        timer = {"at": self.t + delay, "fn": callback, "dead": False}
        self.timers.append(timer)

        class Handle:
            def cancel(self_inner):
                timer["dead"] = True

        return Handle()

    async def advance(self, ms):
        end = self.t + ms
        while True:
            due = sorted((t for t in self.timers
                          if not t["dead"] and t["at"] <= end),
                         key=lambda t: t["at"])
            if not due:
                break
            timer = due[0]
            timer["dead"] = True
            self.timers.remove(timer)
            self.t = max(self.t, timer["at"])
            result = timer["fn"]()
            if asyncio.iscoroutine(result):
                await result
        self.t = end


class Kv:
    def __init__(self):
        self.data = {}

    async def get(self, key):
        return json.loads(json.dumps(self.data.get(key)))

    async def put(self, key, value=None):
        items = key if isinstance(key, dict) else {key: value}
        for k, v in items.items():
            self.data[k] = json.loads(json.dumps(v))

    async def delete(self, keys):
        for k in keys:
            self.data.pop(k, None)

    async def list(self, prefix):
        return {k: v for k, v in self.data.items() if k.startswith(prefix)}


def make_store(db=None, kv=None):
    db = db or sqlite3.connect(":memory:")
    if not db.execute("SELECT name FROM sqlite_master WHERE name="
                      "'org_office_messages'").fetchone():
        db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
        for migration in MIGRATIONS:
            db.executescript(migration.read_text())
    db.row_factory = sqlite3.Row

    async def first(sql, *args):
        row = db.execute(sql, args).fetchone()
        db.commit()
        return dict(row) if row else None

    async def rows(sql, *args):
        return [dict(x) for x in db.execute(sql, args).fetchall()]

    async def run(sql, *args):
        db.execute(sql, args)
        db.commit()

    async def seal(record):
        return "sealed:" + base64.b64encode(json.dumps(record).encode()).decode()

    async def unseal(stored):
        if not str(stored).startswith("sealed:"):
            return None
        return json.loads(base64.b64decode(stored[7:]))

    store = r.SqlOfficeStore(first, rows, run, seal, unseal, kv or Kv())
    store.db = db
    return store


class Socket:
    def __init__(self):
        self.sent = []
        self.closed = None

    def send(self, text):
        if self.closed:
            raise RuntimeError("closed")
        self.sent.append(text)

    def close(self, code, reason):
        self.closed = (code, reason)

    def messages(self, kind=None):
        out = []
        for text in self.sent:
            if text == "pong":
                continue
            m = json.loads(text)
            if kind is None or m.get("t") == kind:
                out.append(m)
        return out

    def last(self, kind):
        found = self.messages(kind)
        return found[-1] if found else None


def profile(n, org=ORG_A, teams=(), role="member", name=None):
    return r.clean_profile({
        "org_bi": org, "org": "acme" if org == ORG_A else "globex",
        "account_bi": ("%02d" % n) * 32, "session_id": "s" * 30 + str(n),
        "name": name or "user%d" % n, "teams": list(teams), "role": role,
        "nonce": "nonce%04d" % n, "retention_ms": 7 * o.DAY,
    })


@pytest.fixture
def env():
    clock = Clock()
    store = make_store()
    allowed = {}

    async def recheck(p):
        return allowed.get(p["account_bi"], {"teams": p["teams"],
                                             "role": p["role"]})

    core = r.OfficeCore(ORG_A, store, recheck=recheck, clock=clock,
                        schedule=clock.schedule)
    return {"clock": clock, "store": store, "core": core, "allowed": allowed}


async def join(core, prof, **extra):
    ws = Socket()
    assert core.connect(ws, prof) is None
    await core.message(ws, json.dumps({"t": "join", "name": "ignored",
                                       **extra}))
    init = ws.last("init")
    assert init, ws.sent
    return ws, init


async def send(core, ws, **m):
    await core.message(ws, json.dumps(m))


def run(coro):
    return asyncio.run(coro)


def test_members_meet_and_movement_is_batched(env):
    async def go():
        core, clock = env["core"], env["clock"]
        a, init_a = await join(core, profile(1), x=30, y=20)
        b, init_b = await join(core, profile(2), x=31, y=20)
        assert init_a["you"][1] == "user1" and init_a["office"] == "acme"
        assert init_a["you"][3:5] == [30, 20]
        assert init_b["peers"][0][0] == init_a["you"][0]
        assert a.last("j")["p"][0] == init_b["you"][0]
        await send(core, a, t="m", x=31, y=21, d=0)
        await send(core, a, t="m", x=32, y=21, d=3)
        assert b.last("mv") is None
        await clock.advance(60)
        moves = b.messages("mv")
        assert len(moves) == 1 and moves[0]["u"] == [[init_a["you"][0], 32,
                                                      21, 3]]
        await send(core, a, t="m", x=40, y=21, d=0)
        assert a.last("you") == {"t": "you", "x": 32, "y": 21}
        assert "acct" not in json.dumps(init_b) and ("01" * 32) not in \
            json.dumps(init_b)
    run(go())


def test_team_rooms_are_gated_for_moves_and_channels(env):
    async def go():
        core = env["core"]
        eng, init_eng = await join(core, profile(1, teams=["Platform"]),
                                   x=18, y=10)
        designer, init_design = await join(
            core, profile(2, teams=["design"]), x=18, y=10)
        assert o.zone_at(18, 10) == "a"
        assert init_eng["you"][3:5] == [18, 10]
        assert o.zone_at(*init_design["you"][3:5]) != "a"
        assert "chat" not in init_eng
        core.users[init_design["you"][0]].u.update(x=14, y=17)
        assert o.zone_at(14, 17) == "."
        await send(core, designer, t="m", x=14, y=16, d=1)
        assert o.zone_at(14, 16) == "a"
        assert designer.last("you") is not None
        assert designer.last("identity:locked")["room"] == "Engineering"
    run(go())


def test_owner_opens_every_room_and_knock_admits(env):
    async def go():
        core = env["core"]
        owner, init = await join(core, profile(1, role="owner"), x=18, y=10)
        assert set(init["identity"]["unlocked"]) == {"a", "b"}
        guest, g_init = await join(core, profile(2), x=14, y=17)
        await send(core, guest, t="meet-knock", z="a")
        assert guest.last("meet-knock-r")["ok"] is True
        knock = owner.last("meet-knock")
        await send(core, owner, t="meet-admit", id=knock["id"], z="a")
        assert guest.last("meet-admitted")["z"] == "a"
        await send(core, guest, t="m", x=14, y=16, d=1)
        assert core.users[g_init["you"][0]].u["y"] == 16
    run(go())


def test_signalling_and_nearby_messages_need_proximity(env):
    async def go():
        core = env["core"]
        a, ia = await join(core, profile(1), x=25, y=20)
        b, ib = await join(core, profile(2), x=27, y=20)
        c, ic = await join(core, profile(3), x=40, y=20)
        offer = {"sdp": {"type": "offer", "sdp": "v=0"}}
        await send(core, a, t="sig", to=ib["you"][0], d=offer)
        await send(core, a, t="sig", to=ic["you"][0], d=offer)
        assert b.last("sig")["d"] == offer
        assert not c.messages("sig")
        await send(core, a, t="sig", to=ic["you"][0], d={"bye": 1})
        assert c.last("sig")["d"] == {"bye": 1}
        await send(core, a, t="say", text="hello",
                   to=[ib["you"][0], ic["you"][0]])
        assert b.last("say")["text"] == "hello"
        assert not c.messages("say")
    run(go())


def test_frame_limits_close_abusive_sockets(env):
    async def go():
        core = env["core"]
        a, _ = await join(core, profile(1))
        await core.message(a, "x" * (o.MESSAGE_MAX_CHARS + 1))
        assert a.closed[0] == 1009
        b, ib = await join(core, profile(2))
        for _ in range(r.FRAME_CAP + 5):
            await send(core, b, t="emo", e="👋")
        assert b.closed == (1008, "rate limit")
        assert ib["you"][0] not in core.users
    run(go())


def test_payloads_are_validated(env):
    async def go():
        core = env["core"]
        a, ia = await join(core, profile(1), x=30, y=20,
                           status={"s": "focus", "emoji": "<b>"})
        assert "status" not in ia["you"][9]
        b, _ = await join(core, profile(2), x=31, y=20)
        await send(core, a, t="ex", k="status",
                   v={"s": "focus", "text": " heads  down "})
        assert b.last("ex")["v"] == {"s": "focus", "text": "heads down"}
        await send(core, a, t="ex", k="admin", v=True)
        await send(core, a, t="ex", k="sharing", v="yes")
        assert len(b.messages("ex")) == 1
        await send(core, a, t="chat", text="stored nowhere")
        assert not b.messages("chat")
        await send(core, a, t="wb.op", b=o.BOARDS[0],
                   op={"o": "add", "items": []})
        assert a.last("wb.nack")["err"] == "closed"
    run(go())


def test_whiteboard_edits_stay_in_the_room_and_persist(env):
    async def go():
        core, clock = env["core"], env["clock"]
        board = o.BOARDS[0]
        eng, _ = await join(core, profile(1, teams=["dev"]), x=18, y=10)
        hall, _ = await join(core, profile(2), x=30, y=20)
        await send(core, eng, t="wb.open", b=board)
        assert eng.last("wb.state")["edit"] is True
        await send(core, hall, t="wb.open", b=board)
        assert hall.last("wb.deny")
        item = {"id": "s1", "k": "t", "x": 5, "y": 5, "text": "plan"}
        await send(core, eng, t="wb.op", b=board, s=1,
                   op={"o": "add", "items": [item]})
        assert eng.last("wb.op")["op"]["items"][0]["text"] == "plan"
        await send(core, eng, t="wb.live", b=board, id="z", c="red",
                   pts=list(range(700)))
        await clock.advance(2000)
        assert env["store"].kv.data["wb:%s:s1" % board]["text"] == "plan"
        assert eng.last("wb.thumb")["b"] == board
        assert hall.last("wb.thumb") is None
    run(go())


def test_revoked_members_are_disconnected(env):
    async def go():
        core, clock = env["core"], env["clock"]
        a, ia = await join(core, profile(1), x=30, y=20)
        b, _ = await join(core, profile(2), x=31, y=20)
        env["allowed"][("01" * 32)] = None
        await clock.advance(r.SWEEP_MS + 10)
        assert a.closed == (4003, "access revoked")
        assert ia["you"][0] not in core.users
        assert b.last("l")["id"] == ia["you"][0]
        assert b.closed is None
    run(go())


def test_a_second_device_replaces_the_first(env):
    async def go():
        core = env["core"]
        first, i1 = await join(core, profile(1))
        second_profile = profile(1)
        second_profile["nonce"] = "othernonce"
        second, i2 = await join(core, second_profile)
        assert first.closed == (4001, "replaced")
        assert list(core.users) == [i2["you"][0]]
    run(go())


def test_desks_scores_and_pong(env):
    async def go():
        core, clock, store = env["core"], env["clock"], env["store"]
        desk = o.DESKS[0]
        a, _ = await join(core, profile(1, teams=["dev"]), x=desk["x"],
                          y=desk["y"])
        await send(core, a, t="work-claim", x=desk["x"], y=desk["y"])
        assert store.kv.data["work-desks"]
        b, ib = await join(core, profile(2, teams=["dev"]))
        assert b.last("init")["workDesks"][0]["name"] == "user1"
        table = o.PONG_TABLES[0]
        core.users[a.last("init")["you"][0]].u.update(x=table["x0"] - 1,
                                                      y=table["y0"])
        core.users[ib["you"][0]].u.update(x=table["x1"] + 1, y=table["y0"])
        await send(core, a, t="play-pong-join")
        await send(core, b, t="play-pong-join")
        assert b.last("play-pong")["s"] == "play"
        await clock.advance(500)
        assert len(a.messages("play-pong")) > 5
        await send(core, a, t="play-arcade-score", score=10)
        assert a.last("play-arcade-result")["ok"] is False
    run(go())


def test_attendance_is_recorded(env):
    async def go():
        core, clock = env["core"], env["clock"]
        a, _ = await join(core, profile(1))
        clock.t += 60000
        await send(core, a, t="pr-att", tz=0)
        summary = a.last("pr-att")
        assert summary["me"]["today"] == 60000
        assert summary["today"][0]["name"] == "user1"
    run(go())


def test_claims_round_trip_and_reject_tampering():
    prof = profile(1, teams=["Dev"])
    encoded = r.encode_claim(prof)
    assert r.decode_claim(encoded) == prof
    assert r.decode_claim(encoded + "x") is None
    bad = dict(prof, org_bi="zz")
    assert r.decode_claim(r.encode_claim(bad)) is None
    assert r.clean_profile(dict(prof, name="\u0000", account="\u0000")) is None
    assert r.clean_profile(dict(prof, retention_ms=10 ** 12))[
        "retention_ms"] == r.CHAT_RETAIN_MAX_MS


def test_join_status_survives_and_null_clears_it(env):
    async def go():
        core = env["core"]
        a, ia = await join(core, profile(1), x=30, y=20,
                           status={"s": "break", "emoji": "☕"})
        assert ia["you"][9]["status"] == {"s": "break", "emoji": "☕"}
        b, ib = await join(core, profile(2), x=31, y=20)
        assert ib["peers"][0][9]["status"]["s"] == "break"
        await send(core, a, t="ex", k="status", v=None)
        assert b.last("ex") == {"t": "ex", "id": ia["you"][0], "k": "status",
                                "v": None}
        assert "status" not in core.users[ia["you"][0]].u["ex"]
    run(go())


def test_summon_is_delivered_answered_limited_and_refused_in_focus(env):
    async def go():
        core, clock = env["core"], env["clock"]
        a, ia = await join(core, profile(1), x=30, y=20)
        b, ib = await join(core, profile(2), x=31, y=20)
        await send(core, a, t="pr-summon", to=ib["you"][0])
        assert a.last("pr-summon-r")["ok"] is True
        assert b.last("pr-summon")["f"] == ia["you"][0]
        await send(core, b, t="pr-summon-a", to=ia["you"][0])
        assert a.last("pr-summon-a")["ok"] is True
        await send(core, a, t="pr-summon", to=ib["you"][0])
        assert a.last("pr-summon-r") == {
            "t": "pr-summon-r", "to": ib["you"][0], "name": "user2",
            "ok": False, "reason": "wait"}
        clock.t += r.SUMMON_COOLDOWN + 1
        await send(core, b, t="ex", k="status", v={"s": "focus"})
        await send(core, a, t="pr-summon", to=ib["you"][0])
        assert a.last("pr-summon-r")["reason"] == "focus"
        await send(core, a, t="pr-summon", to="nobody")
        assert a.last("pr-summon-r")["reason"] == "gone"
    run(go())


def test_desk_claim_and_task_ping_reach_long_account_names(env):
    async def go():
        core, store = env["core"], env["store"]
        long_name = "a-very-long-account-name-over-24-chars"
        desk = o.DESKS[0]
        a, ia = await join(core, profile(1, teams=["dev"], name=long_name),
                           x=desk["x"], y=desk["y"])
        assert ia["you"][1] == long_name[:o.NAME_MAX]
        assert ia["you"][9]["fm"] == {"a": long_name, "n": long_name}
        b, _ = await join(core, profile(2), x=30, y=20)
        await send(core, a, t="work-claim", x=desk["x"], y=desk["y"])
        assert b.last("work-desks")["desks"] == [
            {"x": desk["x"], "y": desk["y"], "name": long_name[:24]}]
        assert store.kv.data["work-desks"]
        await send(core, b, t="work-claim", x=desk["x"], y=desk["y"])
        assert b.last("work-err")["reason"] == "taken"
        task = {"id": "T-1", "title": "Ship <it>"}
        await send(core, b, t="work-ping", to=[long_name], task=task,
                   what="completed")
        ping = a.last("work-ping")
        assert ping["task"] == {"id": "T-1", "title": "Ship it"}
        assert ping["what"] == "completed"
        assert b.last("work-pinged")["n"] == 1
        await send(core, b, t="work-ping", to=[long_name[:24]], task=task)
        assert b.last("work-pinged")["n"] == 0
        await send(core, a, t="work-release")
        assert b.last("work-desks")["desks"] == []
    run(go())


def test_long_names_survive_the_claim_round_trip():
    long_name = "a-very-long-account-name-over-24-chars"
    prof = profile(1, name=long_name)
    assert r.decode_claim(r.encode_claim(prof))["account"] == long_name


def test_gated_whiteboard_thumbnails_reach_only_people_who_may_enter(env):
    async def go():
        core, clock = env["core"], env["clock"]
        board = next(b for b in o.BOARDS
                     if o.zone_at(*map(int, b.split("."))) == "a")
        eng, ie = await join(core, profile(1, teams=["dev"]), x=18, y=10)
        other, io = await join(core, profile(2), x=30, y=20)
        assert board in ie["wb"]["thumbs"] and board not in io["wb"]["thumbs"]
        await send(core, eng, t="wb.open", b=board)
        await send(core, eng, t="wb.op", b=board, s=1, op={
            "o": "add", "items": [{"id": "n1", "k": "n", "x": 1, "y": 1,
                                   "text": "secret"}]})
        await clock.advance(2000)
        assert eng.last("wb.thumb")["b"] == board
        assert other.last("wb.thumb") is None
    run(go())


def test_a_full_office_says_so(env):
    async def go():
        core = env["core"]
        def member(n):
            return dict(profile(1), account_bi="%064x" % (n + 1),
                        nonce="nonce%05d" % n, name="m%d" % n, account="")
        for n in range(o.MAX_USERS):
            await join(core, r.clean_profile(member(n)))
        assert core.connect(Socket(), r.clean_profile(
            member(o.MAX_USERS))) == "office_full"
        assert r.FULL_REASON == "The office is full (100 people)."
    run(go())


def test_profile_names_and_time_zones_persist_and_cannot_impersonate(env):
    async def go():
        core, store = env["core"], env["store"]
        a, init_a = await join(core, profile(1), x=30, y=20)
        b, init_b = await join(core, profile(2), x=31, y=20)
        await send(core, a, t="prof-set", name="  Ann <Lee> ",
                   tz="Asia/Kolkata")
        assert a.last("prof-ok") == {"t": "prof-ok", "name": "Ann Lee",
                                     "custom": True, "tz": "Asia/Kolkata",
                                     "av": init_a["you"][2]}
        assert b.last("prof") == {"t": "prof", "id": init_a["you"][0],
                                  "name": "Ann Lee", "av": init_a["you"][2]}
        zones = [m for m in b.messages("ex") if m["k"] == "tz"]
        assert zones[-1]["v"] == "Asia/Kolkata"
        assert store.kv.data[o.profile_key("01" * 32)] == {
            "name": "Ann Lee", "tz": "Asia/Kolkata"}
        await send(core, b, t="prof-set", name="ann lee")
        assert b.last("prof-err") == {"t": "prof-err", "reason": "taken"}
        await send(core, b, t="prof-set", name="USER1")
        assert len(b.messages("prof-err")) == 2
        assert core.users[init_b["you"][0]].u["name"] == "user2"
        c, init_c = await join(core, profile(1), x=30, y=20)
        assert a.closed == (4001, "replaced")
        assert init_c["you"][1] == "Ann Lee"
        assert init_c["you"][9]["tz"] == "Asia/Kolkata"
        assert init_c["you"][9]["fm"] == {"a": "user1", "n": "user1"}
        await send(core, c, t="prof-set", name="", tz="not a zone")
        assert c.last("prof-ok") == {"t": "prof-ok", "name": "user1",
                                     "custom": False, "tz": "",
                                     "av": init_a["you"][2]}
        assert o.profile_key("01" * 32) not in store.kv.data
        assert "tz" not in core.users[init_c["you"][0]].u["ex"]
    run(go())


def test_a_renamed_member_keeps_the_desk_plate_in_step(env):
    async def go():
        core = env["core"]
        a, _ = await join(core, profile(1), x=30, y=20)
        desk = o.DESKS[0]
        await send(core, a, t="work-claim", x=desk["x"], y=desk["y"])
        await send(core, a, t="prof-set", name="Ann")
        assert a.last("work-desks")["desks"][0]["name"] == "Ann"
    run(go())


def test_account_pictures_reach_the_room_and_follow_rechecks(env):
    async def go():
        core, clock = env["core"], env["clock"]
        raw = {"org_bi": ORG_A, "org": "acme", "account_bi": "07" * 32,
               "session_id": "s" * 31, "name": "pat", "teams": [],
               "role": "member", "nonce": "nonce0007",
               "pic": "0123456789abcdef"}
        assert r.clean_profile({**raw, "pic": "<img>"})["pic"] == ""
        a, init_a = await join(core, r.clean_profile(raw), x=30, y=20)
        b, _ = await join(core, profile(2), x=31, y=20)
        assert init_a["you"][9]["pic"] == "0123456789abcdef"
        assert r.decode_claim(r.encode_claim(r.clean_profile(raw)))["pic"] \
            == "0123456789abcdef"
        env["allowed"]["07" * 32] = {"teams": [], "role": "member",
                                     "pic": "f" * 16}
        await clock.advance(r.RECHECK_MS + 1)
        await send(core, a, t="st", mic=False, cam=False, away=False)
        pics = [m for m in b.messages("ex") if m["k"] == "pic"]
        assert pics[-1]["v"] == "f" * 16
        env["allowed"]["07" * 32] = {"teams": [], "role": "member", "pic": ""}
        await clock.advance(r.RECHECK_MS + 1)
        await send(core, a, t="st", mic=False, cam=False, away=False)
        pics = [m for m in b.messages("ex") if m["k"] == "pic"]
        assert pics[-1]["v"] is None
    run(go())


def test_avatars_are_validated_stored_and_restored(env):
    async def go():
        core, store = env["core"], env["store"]
        a, init_a = await join(core, profile(1), x=30, y=20)
        b, _ = await join(core, profile(2), x=31, y=20)
        default = init_a["you"][2]
        assert default == o.avatar_for("01" * 32) < o.AV_MAX
        custom = o.AV_CUSTOM + 4242
        await send(core, a, t="prof-set", av=custom)
        assert a.last("prof-ok")["av"] == custom
        assert b.last("prof") == {"t": "prof", "id": init_a["you"][0],
                                  "name": "user1", "av": custom}
        assert store.kv.data[o.profile_key("01" * 32)] == {"av": custom}
        for bad in (o.AV_CUSTOM + o.AV_COMBOS, -1, 12345, 1.5, "7", [1]):
            await clock_free(core, a)
            await send(core, a, t="prof-set", av=bad)
            assert a.last("prof-err") == {"t": "prof-err",
                                          "reason": "avatar"}
        assert core.users[init_a["you"][0]].u["av"] == custom
        c, init_c = await join(core, profile(1), x=30, y=20)
        assert init_c["you"][2] == custom
        await send(core, c, t="prof-set", av=None)
        assert c.last("prof-ok")["av"] == default
        assert o.profile_key("01" * 32) not in store.kv.data
    run(go())


async def clock_free(core, ws):
    core.entry_for(ws).buckets.pop("spend", None)


def test_applause_breaks_out_when_three_people_in_a_room_celebrate(env):
    async def go():
        core, clock = env["core"], env["clock"]
        people = []
        for n in range(1, 5):
            ws, init = await join(core, profile(n), x=29 + n, y=20)
            people.append((ws, init["you"][0]))
        far, far_init = await join(core, profile(9), x=30, y=20)
        core.users[far_init["you"][0]].u.update(x=40, y=27)
        zone = o.zone_at(30, 20)
        assert o.zone_at(40, 27) != zone
        (a, ida), (b, idb), (c, idc), (d, idd) = people
        await send(core, a, t="emo", e="👏")
        await send(core, b, t="emo", e="🎉")
        assert a.last("cheer") is None
        await send(core, a, t="emo", e="👏")
        assert a.last("cheer") is None
        await send(core, c, t="emo", e="💃")
        cheer = d.last("cheer")
        assert cheer == {"t": "cheer", "z": zone, "n": 3,
                         "ids": sorted([ida, idb, idc])}
        assert a.last("cheer") == cheer and far.last("cheer") is None
        await clock.advance(1500)
        for ws in (a, b, c):
            await send(core, ws, t="emo", e="👏")
        assert len(d.messages("cheer")) == 1
        await clock.advance(o.CHEER_COOLDOWN_MS)
        for ws in (a, b, d):
            await send(core, ws, t="emo", e="👏")
        assert len(d.messages("cheer")) == 2
        await clock.advance(o.CHEER_COOLDOWN_MS)
        await send(core, a, t="emo", e="👏")
        await send(core, b, t="emo", e="👏")
        await clock.advance(o.CHEER_WINDOW_MS)
        await send(core, c, t="emo", e="👏")
        assert len(d.messages("cheer")) == 2
        await send(core, a, t="emo", e="👍")
        await send(core, b, t="emo", e="👍")
        assert len(d.messages("cheer")) == 2
        assert o.cheer_mark({"x": 0, "y": 3900, "gone": 3950}, "z", 4000,
                            {"x", "y", "z"}) == {"y": 3900, "z": 4000}
    run(go())


def test_focus_sessions_need_a_couch_notify_and_end_on_standing_up(env):
    async def go():
        core, clock = env["core"], env["clock"]
        assert o.is_couch(36, 33) and o.is_couch(37, 33) and \
            not o.is_couch(36, 34)
        a, init_a = await join(core, profile(1), x=36, y=34)
        b, init_b = await join(core, profile(2), x=37, y=34)
        c, _ = await join(core, profile(3), x=30, y=20)
        d, _ = await join(core, profile(4), x=31, y=20,
                          status={"s": "focus"})
        ida, idb = init_a["you"][0], init_b["you"][0]
        assert isinstance(init_a["now"], int)
        await send(core, a, t="focus-start", mode="focused", mins=60)
        assert a.last("focus-err") == {"t": "focus-err", "reason": "seat"}
        await send(core, a, t="m", x=36, y=33, d=0)
        await send(core, a, t="focus-start", mode="focused", mins=7)
        assert a.last("focus-err") == {"t": "focus-err", "reason": "bad"}
        start = clock()
        await send(core, a, t="focus-start", mode="focused", mins=60,
                   topic=" ship <it> ", notify=True)
        session = {"mode": "focused", "at": start, "topic": "ship it",
                   "mins": 60, "end": start + 3600000}
        assert core.users[ida].u["ex"]["focus"] == session
        assert c.last("focus-open") == {"t": "focus-open", "id": ida,
                                        "name": "user1", "focus": session}
        assert b.last("focus-open") is not None
        assert d.last("focus-open") is None and a.last("focus-open") is None
        await send(core, b, t="focus-start", join=ida)
        assert b.last("focus-err") == {"t": "focus-err", "reason": "seat"}
        await send(core, b, t="m", x=37, y=33, d=0)
        await clock.advance(5000)
        await send(core, b, t="focus-start", join=ida)
        assert core.users[idb].u["ex"]["focus"] == {**session, "host": ida}
        assert len(c.messages("focus-open")) == 1
        await send(core, b, t="focus-start", join="nobody")
        assert b.last("focus-err") == {"t": "focus-err", "reason": "gone"}
        await send(core, a, t="focus-end")
        await send(core, a, t="focus-start", mode="freeform", notify=True)
        assert len(c.messages("focus-open")) == 1
        assert core.users[ida].u["ex"]["focus"] == {"mode": "freeform",
                                                    "at": clock()}
        await send(core, a, t="m", x=36, y=34, d=0)
        assert "focus" not in core.users[ida].u["ex"]
        await send(core, b, t="m", x=38, y=33, d=0)
        assert core.users[idb].u["ex"]["focus"]["host"] == ida
        await send(core, b, t="focus-end")
        assert "focus" not in core.users[idb].u["ex"]
        again = profile(1)
        again["nonce"] = "othernonce"
        a2, _ = await join(core, again, x=36, y=33)
        await send(core, a2, t="focus-start", mode="freeform", notify=True)
        assert len(c.messages("focus-open")) == 1
        await clock.advance(r.FOCUS_NOTE_MS)
        await send(core, a2, t="focus-end")
        await send(core, a2, t="focus-start", mode="freeform", notify=True)
        assert len(c.messages("focus-open")) == 2
    run(go())


def test_desk_decorations_are_validated_shared_kept_and_moved(env):
    async def go():
        core, store = env["core"], env["store"]
        assert len(o.DESKS) == 28
        a, _ = await join(core, profile(1), x=30, y=20)
        b, _ = await join(core, profile(2), x=31, y=20)
        deco = {"desk": 2, "chair": 3, "slots": [1, 5, 10, 14]}
        await send(core, a, t="work-deco", deco=deco)
        assert a.last("work-err") == {"t": "work-err", "reason": "no_desk"}
        first, second = o.DESKS[4], o.DESKS[5]
        await send(core, a, t="work-claim", x=first["x"], y=first["y"])
        await send(core, a, t="work-deco",
                   deco={"slots": [9, 0, 0, 0]})
        assert a.last("work-err") == {"t": "work-err", "reason": "deco"}
        await send(core, a, t="work-deco", deco=deco)
        assert a.last("work-deco") == {"t": "work-deco", "ok": True}
        assert b.last("work-desks")["desks"] == [
            {"x": first["x"], "y": first["y"], "name": "user1",
             "deco": deco}]
        assert store.kv.data["work-desks"]["a:" + "01" * 32]["deco"] == deco
        core.entry_for(a).buckets.pop("spend", None)
        seen = len(b.messages("work-desks"))
        await send(core, a, t="work-deco", deco=deco)
        assert len(b.messages("work-desks")) == seen
        assert a.last("work-deco") == {"t": "work-deco", "ok": True}
        core.entry_for(a).buckets.pop("spend", None)
        await send(core, a, t="work-claim", x=second["x"], y=second["y"])
        assert b.last("work-desks")["desks"] == [
            {"x": second["x"], "y": second["y"], "name": "user1",
             "deco": deco}]
        await send(core, a, t="prof-set", name="Ann")
        assert b.last("work-desks")["desks"][0] == {
            "x": second["x"], "y": second["y"], "name": "Ann", "deco": deco}
        fresh = r.OfficeCore(ORG_A, r.SqlOfficeStore(
            store.first, store.rows, store.run, store.seal, store.unseal,
            store.kv), clock=env["clock"], schedule=env["clock"].schedule)
        store.kv.data["work-desks"]["a:" + "01" * 32]["deco"] = {
            "desk": 99, "slots": []}
        await fresh.load()
        assert "deco" not in fresh.desks["a:" + "01" * 32]
        core.entry_for(a).buckets.pop("spend", None)
        await send(core, a, t="work-deco",
                   deco={"desk": 0, "chair": 0, "slots": [0, 0, 0, 0]})
        assert "deco" not in b.last("work-desks")["desks"][0]
    run(go())


def fire_tables(store, now, members):
    db = store.db
    db.executescript(
        "CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT,"
        " name TEXT, created_at INTEGER);"
        "CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT);"
        "CREATE TABLE world_user_activity (account_bi TEXT PRIMARY KEY,"
        " total_active_ms INTEGER, total_steps INTEGER,"
        " last_touch_at INTEGER);"
        "CREATE TABLE contributor_activity (author_bi TEXT PRIMARY KEY,"
        " name TEXT, issues INTEGER, pulls INTEGER, commits INTEGER,"
        " discussions INTEGER, total INTEGER, last_ts INTEGER);")
    for n, spec in members.items():
        bi = ("%02d" % n) * 32
        record = {"kind": "user", "status": "active",
                  "profile_private": bool(spec.get("private"))}
        db.execute("INSERT INTO org_members VALUES (?,?,?,?,0)",
                   (spec.get("org", ORG_A), bi, spec.get("role", "member"),
                    "user%d" % n))
        db.execute("INSERT INTO users VALUES (?,?)", (
            bi, "sealed:" + base64.b64encode(
                json.dumps(record).encode()).decode()))
        if "world" in spec:
            active, steps, ago = spec["world"]
            db.execute("INSERT INTO world_user_activity VALUES (?,?,?,?)",
                       (bi, active, steps, now - ago))
        if "contrib" in spec:
            total, ago = spec["contrib"]
            db.execute("INSERT INTO contributor_activity VALUES "
                       "(?,?,?,0,0,0,?,?)", (bi, "x", total, total, now - ago))
    db.commit()


def test_fire_score_follows_the_world_rings_without_avatar_or_readiness():
    now = 10 ** 13
    hour, day = 3600000, 86400000
    blaze = o.fire_score({"world_at": now - hour, "active_ms": 50 * hour,
                          "steps": 30000, "issues": 20}, now)
    assert blaze == {"score": 46 + 22 + 10 + 14, "tier": "blaze",
                     "depth": 46}
    idle = o.fire_score({"world_at": now - 2 * day}, now)
    assert idle["tier"] is None and idle["score"] == 36
    ember = o.fire_score({"office_at": now - 2 * day,
                          "office_ms": 3 * hour, "office_visits": 6}, now)
    assert ember == {"score": 36 + 7 + 4, "tier": "ember", "depth": 11}
    old = o.fire_score({"world_at": now - 6 * day, "active_ms": 50 * hour,
                        "steps": 10 ** 6, "issues": 10 ** 4}, now)
    assert old["tier"] is None
    assert o.fire_score({"world_at": True, "steps": "9"}, now) == {
        "score": 0, "tier": None, "depth": 0}
    rows = [
        {"bi": "b", "name": "bo", "office_at": now - 2 * day,
         "office_ms": 3 * hour, "office_visits": 6},
        {"bi": "a", "name": "ann", "world_at": now - hour,
         "active_ms": 50 * hour, "steps": 30000, "issues": 20},
        {"bi": "c", "name": "cy", "world_at": now - 2 * day},
        {"bi": "", "name": "nobody", "world_at": now - hour},
    ]
    ring = o.fire_ring(rows, now)
    assert [(t["name"], t["rank"], t["tier"]) for t in ring] == [
        ("ann", 1, "blaze"), ("bo", 2, "ember")]
    assert o.fire_public(ring)[0] == {"n": "ann", "r": 1, "t": "blaze",
                                      "s": 92}
    assert len(o.fire_ring(rows * 10, now, seats=3)) == 3


def test_campfire_admits_only_top_members_and_publishes_the_ring(env):
    async def go():
        core, store, clock = env["core"], env["store"], env["clock"]
        now = clock()
        hour = 3600000
        busy = (50 * hour, 30000, hour)
        fire_tables(store, now, {
            1: {"world": busy, "contrib": (20, hour)},
            2: {"world": (20 * hour, 6000, 2 * hour)},
            3: {"world": (0, 0, 200 * 86400000)},
            4: {"world": busy, "private": True},
            5: {"world": busy, "org": ORG_B},
        })
        a, init_a = await join(core, profile(1), x=30, y=20)
        c, init_c = await join(core, profile(3), x=31, y=20)
        assert init_a["fire"] == [
            {"n": "user1", "r": 1, "t": "blaze", "s": 92},
            {"n": "user2", "r": 2, "t": "blaze", "s": 70}]
        assert core.is_top(core.entry_for(a))
        assert not core.is_top(core.entry_for(c))
        sx, sy = o.FIRE_SEATS[0]
        edge = (o.OX + 40, o.OY - 3)
        assert o.zone_at(*edge) == "p"
        for ws in (a, c):
            u = core.entry_for(ws).u
            u["x"], u["y"] = edge
        await send(core, c, t="m", x=edge[0] + 1, y=edge[1], d=3)
        assert c.last("you") == {"t": "you", "x": edge[0], "y": edge[1]}
        assert c.last("fire-locked") == {"t": "fire-locked", "org": "acme"}
        await send(core, a, t="m", x=edge[0] + 1, y=edge[1], d=3)
        assert core.users[init_a["you"][0]].u["x"] == edge[0] + 1
        c2_profile = profile(3)
        c2_profile["nonce"] = "othernonce"
        c2, init_c2 = await join(core, c2_profile, x=sx, y=sy)
        assert o.zone_at(init_c2["you"][3], init_c2["you"][4]) != o.FIRE_ZONE
        pond = (o.OX + 34, o.OY - 4)
        assert o.obj_at(*pond) == o.WATER and o.is_walkable(*pond)
        store.db.execute(
            "UPDATE world_user_activity SET last_touch_at=? WHERE "
            "account_bi=?", (now - 3 * hour, "03" * 32))
        store.db.execute(
            "UPDATE world_user_activity SET total_active_ms=?,"
            "total_steps=? WHERE account_bi=?",
            (50 * hour, 30000, "03" * 32))
        store.db.commit()
        await clock.advance(o.FIRE_REFRESH_MS)
        assert [t["n"] for t in c2.last("fire")["top"]] == [
            "user1", "user3", "user2"]
    run(go())


def test_rollercoaster_boards_only_from_the_platform(env):
    async def go():
        core = env["core"]
        y, x0, x1 = o.COASTER_PLATFORM
        a, ia = await join(core, profile(1), x=x0, y=y)
        b, _ = await join(core, profile(2), x=x1, y=y)
        await send(core, a, t="ex", k="ride", v={"go": 1})
        ride = b.last("ex")
        assert ride["k"] == "ride" and ride["v"] == {"id": "coaster", "at": core.now()}
        c, _ = await join(core, profile(3), x=30, y=20)
        await send(core, c, t="ex", k="ride", v={"go": 1})
        assert all(m["k"] != "ride" or m["id"] == ia["you"][0]
                   for m in b.messages("ex"))
        await send(core, a, t="ex", k="ride", v="go")
        assert len([m for m in b.messages("ex") if m["k"] == "ride"]) == 1
    run(go())


def test_rides_board_from_their_area_and_carry_riders_to_the_exit(env):
    async def go():
        core = env["core"]
        x0, y0, x1, y1 = o.RIDES["drive-park"]["board"]
        a, ia = await join(core, profile(1), x=x0, y=y0)
        b, _ = await join(core, profile(2), x=30, y=20)
        uid = ia["you"][0]
        await send(core, a, t="ex", k="ride", v={"id": "drive-park"})
        exit_x, exit_y = o.RIDES["drive-park"]["exit"]
        assert a.last("you") == {"t": "you", "x": exit_x, "y": exit_y}
        assert (core.users[uid].u["x"], core.users[uid].u["y"]) == (exit_x, exit_y)
        ride = b.last("ex")
        assert ride["k"] == "ride" and ride["v"] == {"id": "drive-park", "at": core.now()}
        await send(core, a, t="ex", k="ride", v={"id": "drive-park"})
        assert len([m for m in b.messages("ex") if m["k"] == "ride"]) == 1
        await send(core, a, t="ex", k="ride", v={"id": "nope"})
        await send(core, a, t="ex", k="ride", v={"id": "drop"})
        assert len([m for m in b.messages("ex") if m["k"] == "ride"]) == 1
        bx0, by0, _, _ = o.RIDES["lazy"]["board"]
        core.users[uid].u["x"], core.users[uid].u["y"] = bx0, by0
        await send(core, a, t="ex", k="ride", v={"id": "lazy"})
        assert (core.users[uid].u["x"], core.users[uid].u["y"]) == (bx0, by0)
        assert b.last("ex")["v"]["id"] == "lazy"
    run(go())
