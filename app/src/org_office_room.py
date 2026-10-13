import asyncio
import base64
import json
import random
import re
import secrets

import org_office as o


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


ACCOUNT_NAME_MAX = 63
FULL_REASON = "The office is full (%d people)." % o.MAX_USERS
CLAIM_HEADER = "x-forkmesh-org-office-claim"
SOCKET_TAG = "org-office"
ROOM_PATH_RE = re.compile(r"^/api/office/org/([0-9a-f]{64})/v1/(ws|revoke)$")
BI_RE = re.compile(r"[0-9a-f]{64}")
SESSION_RE = re.compile(r"[A-Za-z0-9_-]{24,64}")
NONCE_RE = re.compile(r"[A-Za-z0-9_-]{8,32}")
ORG_NAME_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?")
RECHECK_MS = 30000
SWEEP_MS = 30000
CHAT_RETAIN_MAX_MS = 7 * o.DAY
CHAT_RETAIN_MIN_MS = 3600000
CHAT_MAX_PER_CHANNEL = 500
PRUNE_EVERY_MS = 3600000
SUMMON_COOLDOWN = 20000
SUMMON_TTL = 60000
SUMMON_KEEP = 8
PING_WHAT = frozenset(["started", "stopped", "completed", "updated", "commented"])
TYPING_MS = 1500
LOCK_NOTICE_MS = 1500
THUMB_MS = 1500
DRAW_IDLE_MS = 15000
WB_PERSIST_MS = 1000
WB_PUT_CHUNK = 100
OVER_KEEP_MS = 4000
AUDIENCE = 8
LEAVE_RANGE = 2
COOLER_COOLDOWN_MS = 6000
FOCUS_NOTE_MS = 60000
COFFEE_REPEAT_MS = 5 * 60 * 1000
FRAME_CAP = 120
FRAME_RATE = 60


def dumps(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def clean_profile(raw):
    if not isinstance(raw, dict):
        return None
    org_bi = str(raw.get("org_bi") or "")
    account_bi = str(raw.get("account_bi") or "")
    session_id = str(raw.get("session_id") or "")
    org = str(raw.get("org") or "")
    nonce = str(raw.get("nonce") or "")
    if not (BI_RE.fullmatch(org_bi) and BI_RE.fullmatch(account_bi)
            and SESSION_RE.fullmatch(session_id)
            and ORG_NAME_RE.fullmatch(org) and NONCE_RE.fullmatch(nonce)):
        return None
    full = re.sub(r"[\u0000-\u001f\u007f]", "",
                  str(raw.get("account") or raw.get("name") or ""))
    full = full.strip()[:ACCOUNT_NAME_MAX]
    name = full[:o.NAME_MAX]
    if not name:
        return None
    try:
        retention = int(raw.get("retention_ms") or CHAT_RETAIN_MAX_MS)
    except (TypeError, ValueError):
        retention = CHAT_RETAIN_MAX_MS
    return {
        "org_bi": org_bi,
        "org": org,
        "account_bi": account_bi,
        "session_id": session_id,
        "name": name,
        "account": full.lower(),
        "teams": o.clean_teams(raw.get("teams")),
        "role": o.clean_role(raw.get("role")) or "member",
        "nonce": nonce,
        "retention_ms": max(CHAT_RETAIN_MIN_MS, min(CHAT_RETAIN_MAX_MS,
                                                    retention)),
        "pic": o.clean_pic(raw.get("pic")),
    }


def encode_claim(profile):
    raw = dumps(profile).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def decode_claim(value):
    text = str(value or "")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,4096}", text):
        return None
    try:
        padded = text + "=" * ((4 - len(text) % 4) % 4)
        return clean_profile(json.loads(base64.urlsafe_b64decode(padded)))
    except Exception:
        return None


class SqlOfficeStore:
    def __init__(self, first, rows, run, seal, unseal, kv):
        self.first = first
        self.rows = rows
        self.run = run
        self.seal = seal
        self.unseal = unseal
        self.kv = kv

    async def visit_start(self, org_bi, member_bi, name, now):
        row = await self.first(
            "INSERT INTO org_office_visits (org_bi,member_bi,data,started_at,"
            "ended_at,seen_at) VALUES (?,?,?,?,NULL,?) RETURNING id",
            org_bi, member_bi, await self.seal({"name": name}), now, now)
        return int((row or {}).get("id") or 0)

    async def visit_end(self, org_bi, visit_id, now):
        await self.run(
            "UPDATE org_office_visits SET ended_at=?,seen_at=? WHERE org_bi=? "
            "AND id=? AND ended_at IS NULL", now, now, org_bi, visit_id)

    async def visits_seen(self, org_bi, visit_ids, now):
        ids = [int(v) for v in visit_ids if isinstance(v, int)][:200]
        if not ids:
            return
        await self.run(
            "UPDATE org_office_visits SET seen_at=? WHERE org_bi=? AND "
            "ended_at IS NULL AND id IN (" + ",".join("?" * len(ids)) + ")",
            now, org_bi, *ids)

    async def visits_close_stale(self, org_bi, live_ids):
        ids = [int(v) for v in live_ids if isinstance(v, int)][:200]
        if ids:
            await self.run(
                "UPDATE org_office_visits SET ended_at=seen_at WHERE org_bi=? "
                "AND ended_at IS NULL AND id NOT IN ("
                + ",".join("?" * len(ids)) + ")", org_bi, *ids)
        else:
            await self.run(
                "UPDATE org_office_visits SET ended_at=seen_at WHERE org_bi=? "
                "AND ended_at IS NULL", org_bi)

    async def visit_rows(self, org_bi, since, now):
        rows = await self.rows(
            "SELECT member_bi,data,started_at,ended_at FROM org_office_visits "
            "WHERE org_bi=? AND started_at<? AND started_at>=? AND "
            "(ended_at IS NULL OR ended_at>=?) ORDER BY started_at LIMIT 5000",
            org_bi, now + 1, since - 7 * o.DAY, since)
        out = []
        for row in rows:
            record = await self.unseal(row.get("data")) or {}
            name = record.get("name") if isinstance(record, dict) else ""
            out.append({"who": "a:" + row["member_bi"],
                        "name": str(name or "Someone"),
                        "started": int(row["started_at"]),
                        "ended": None if row.get("ended_at") is None
                        else int(row["ended_at"])})
        return out

    async def fire_rows(self, org_bi, now):
        return await self.rows(
            "SELECT om.member_bi AS bi,om.name AS name,"
            "a.total_active_ms AS active_ms,a.total_steps AS steps,"
            "a.last_touch_at AS world_at,c.issues AS issues,c.pulls AS pulls,"
            "c.commits AS commits,c.discussions AS discussions,"
            "c.last_ts AS contrib_at,v.office_ms AS office_ms,"
            "v.office_visits AS office_visits,v.office_at AS office_at "
            "FROM org_members om "
            "LEFT JOIN world_user_activity a ON a.account_bi=om.member_bi "
            "LEFT JOIN contributor_activity c ON c.author_bi=om.member_bi "
            "LEFT JOIN (SELECT member_bi,COUNT(*) AS office_visits,"
            "SUM(MAX(0,COALESCE(ended_at,seen_at)-started_at)) AS office_ms,"
            "MAX(seen_at) AS office_at FROM org_office_visits WHERE org_bi=? "
            "AND started_at>=? GROUP BY member_bi) v "
            "ON v.member_bi=om.member_bi WHERE om.org_bi=? AND om.role IN "
            "('owner','admin','maintainer','member','auditor') LIMIT 1000",
            org_bi, now - o.FIRE_WINDOW_MS, org_bi)

    async def roster_ok(self, account_bi):
        row = await self.first("SELECT data FROM users WHERE user_bi=?",
                               account_bi)
        if not row:
            return False
        return o.roster_ok(await self.unseal(row.get("data")))

    async def prune(self, org_bi, now):
        await self.run(
            "DELETE FROM org_office_visits WHERE org_bi=? AND "
            "COALESCE(ended_at,seen_at)<?", org_bi, now - o.VISIT_KEEP_MS)


class _Timer:
    def __init__(self, handle):
        self.handle = handle

    def cancel(self):
        try:
            self.handle.cancel()
        except Exception:
            pass


def asyncio_schedule(delay_ms, callback):
    loop = asyncio.get_running_loop()

    def fire():
        result = callback()
        if asyncio.iscoroutine(result):
            loop.create_task(result)

    return _Timer(loop.call_later(max(0, delay_ms) / 1000, fire))


class Entry:
    __slots__ = ("ws", "u", "profile", "buckets", "checked_at",
                 "knock_at", "summons", "arcade", "wb_draw_at",
                 "lock_at", "closed", "prof")

    def __init__(self, ws, profile, now):
        self.ws = ws
        self.u = None
        self.profile = profile
        self.buckets = {}
        self.checked_at = now
        self.knock_at = {}
        self.summons = {}
        self.arcade = None
        self.wb_draw_at = 0
        self.lock_at = 0
        self.closed = False
        self.prof = {}


class OfficeCore:
    def __init__(self, org_bi, store, recheck=None, clock=None,
                 schedule=None, rng=None):
        self.org_bi = org_bi
        self.org = ""
        self.store = store
        self.recheck = recheck
        self.clock = clock or o.now_ms
        self.schedule = schedule or asyncio_schedule
        self.rng = rng or random.SystemRandom()
        self.entries = {}
        self.users = {}
        self.dirty = set()
        self.flush_timer = None
        self.sweep_timer = None
        self.loaded = False
        self.desks = {}
        self.admitted = {}
        self.knocks = {}
        self.boards = {}
        self.wb_dirty = {}
        self.wb_timer = None
        self.thumb_timers = {}
        self.games = {}
        self.pong_timer = None
        self.scores = []
        self.coffee = {"day": o.day_key(self.clock()), "counts": {},
                       "last": {}}
        self.cooler_at = {}
        self.top = []
        self.top_bis = frozenset()
        self.fire_at = None
        self.focus_note_at = {}
        self.cooler_last = None
        self.pruned_at = 0
        self.cheers = {}
        self.cheer_at = {}

    async def load(self):
        if self.loaded:
            return
        self.loaded = True
        stored = await self.store.kv.get("work-desks") or {}
        valid = {(d["x"], d["y"]) for d in o.DESKS}
        self.desks = {
            k: d for k, d in stored.items()
            if isinstance(d, dict) and (d.get("x"), d.get("y")) in valid
        } if isinstance(stored, dict) else {}
        for key, desk in list(self.desks.items()):
            deco = o.clean_deco(desk.get("deco"))
            kept = {k: v for k, v in desk.items() if k != "deco"}
            if not o.deco_empty(deco):
                kept["deco"] = deco
            self.desks[key] = kept
        scores = await self.store.kv.get("play:scores")
        self.scores = scores if isinstance(scores, list) else []
        coffee = await self.store.kv.get("play:coffee")
        if isinstance(coffee, dict) and isinstance(coffee.get("counts"), dict):
            coffee.setdefault("last", {})
            self.coffee = coffee
        for board in o.BOARDS:
            await self._board(board)
        try:
            await self.store.visits_close_stale(self.org_bi, [])
        except Exception:
            pass

    def now(self):
        return self.clock()

    def send(self, entry, message):
        if entry.closed:
            return
        try:
            entry.ws.send(message if isinstance(message, str)
                          else dumps(message))
        except Exception:
            entry.closed = True

    def broadcast(self, message, except_entry=None, only=None):
        text = message if isinstance(message, str) else dumps(message)
        for entry in list(self.users.values()):
            if entry is except_entry:
                continue
            if only is not None and not only(entry):
                continue
            self.send(entry, text)

    def close(self, entry, code, reason):
        entry.closed = True
        try:
            entry.ws.close(code, reason)
        except Exception:
            pass

    def connect(self, ws, profile):
        if len(self.users) >= o.MAX_USERS:
            return "office_full"
        self.org = profile["org"]
        entry = Entry(ws, profile, self.now())
        self.entries[id(ws)] = entry
        self._arm_sweep()
        return None

    def entry_for(self, ws):
        return self.entries.get(id(ws))

    def _arm_sweep(self):
        if self.sweep_timer is None and self.recheck is not None:
            self.sweep_timer = self.schedule(SWEEP_MS, self._sweep)

    async def _sweep(self):
        self.sweep_timer = None
        if not self.entries:
            return
        now = self.now()
        for entry in list(self.entries.values()):
            if now - entry.checked_at >= RECHECK_MS:
                await self._revalidate(entry, now)
        if self.users:
            await self.refresh_fire(now)
        if self.entries:
            self._arm_sweep()

    async def _revalidate(self, entry, now):
        entry.checked_at = now
        if self.recheck is None:
            return True
        try:
            fresh = await self.recheck(entry.profile)
        except Exception:
            fresh = None
        if not fresh:
            if entry.u:
                self.send(entry, {"t": "denied",
                                  "reason": "Your access to this office "
                                            "ended."})
            await self.leave(entry)
            self.close(entry, 4003, "access revoked")
            return False
        entry.profile["teams"] = o.clean_teams(fresh.get("teams"))
        entry.profile["role"] = o.clean_role(fresh.get("role")) or "member"
        if "pic" in fresh:
            entry.profile["pic"] = o.clean_pic(fresh.get("pic"))
        if entry.u:
            teams = entry.profile["teams"]
            if entry.u["ex"].get("teams") != teams:
                self.set_extra(entry.u, "teams", teams or None)
            self.set_extra(entry.u, "pic", entry.profile.get("pic") or None)
        return True

    def spend(self, entry):
        return o.bucket(entry.buckets, "spend", 5, 1, 1, self.now())

    async def message(self, ws, raw):
        entry = self.entry_for(ws)
        if entry is None or entry.closed:
            return
        if not isinstance(raw, str):
            self.close(entry, 1003, "text frames only")
            return
        if len(raw) > o.MESSAGE_MAX_CHARS:
            self.close(entry, 1009, "message too large")
            await self.leave(entry)
            return
        now = self.now()
        if not o.bucket(entry.buckets, "frames", FRAME_CAP, FRAME_RATE, 1,
                        now):
            self.close(entry, 1008, "rate limit")
            await self.leave(entry)
            return
        if raw == "ping":
            self.send(entry, "pong")
            return
        if now - entry.checked_at >= RECHECK_MS:
            if not await self._revalidate(entry, now):
                return
        try:
            m = json.loads(raw)
        except Exception:
            return
        if not isinstance(m, dict):
            return
        kind = m.get("t")
        if kind == "join":
            if entry.u is None:
                await self.join(entry, m)
            return
        if entry.u is None:
            return
        handler = HANDLERS.get(kind)
        if handler is not None:
            await handler(self, entry, m)

    def pack(self, u):
        return [u["id"], u["name"], u["av"], u["x"], u["y"], u["d"],
                1 if u["mic"] else 0, 1 if u["cam"] else 0,
                1 if u["away"] else 0, u["ex"]]

    def _new_id(self):
        for _ in range(8):
            candidate = secrets.token_hex(4)
            if candidate not in self.users:
                return candidate
        return secrets.token_hex(8)

    def unlocked(self, entry):
        return o.unlocked_zones(entry.profile["teams"], entry.profile["role"])

    def may_enter(self, entry, zone):
        return o.may_enter(entry.profile, zone,
                           self.admitted.get(entry.u["id"]) if entry.u
                           else None)

    def can_enter(self, entry, x, y, probe):
        if not o.is_walkable(x, y):
            return False
        zone = o.zone_at(x, y)
        u = entry.u
        if zone in o.GATED_ZONES and o.zone_at(u["x"], u["y"]) != zone \
                and not self.may_enter(entry, zone):
            if not probe:
                self._lock_notice(entry, zone)
            return False
        if zone == o.FIRE_ZONE and o.zone_at(u["x"], u["y"]) != zone \
                and not self.is_top(entry):
            if not probe:
                self._fire_notice(entry)
            return False
        return True

    def is_top(self, entry):
        return entry.profile["account_bi"] in self.top_bis

    async def refresh_fire(self, now, force=False):
        if not force and self.fire_at is not None and \
                now - self.fire_at < o.FIRE_REFRESH_MS:
            return
        self.fire_at = now
        try:
            rows = await self.store.fire_rows(self.org_bi, now)
            top = []
            checked = 0
            for item in o.fire_ring(rows, now, seats=len(rows or [])):
                if len(top) >= len(o.FIRE_SEATS) or \
                        checked >= 3 * len(o.FIRE_SEATS):
                    break
                checked += 1
                if await self.store.roster_ok(item["bi"]):
                    top.append({**item, "rank": len(top) + 1})
        except Exception:
            return
        if top == self.top:
            return
        self.top = top
        self.top_bis = frozenset(t["bi"] for t in top)
        self.broadcast({"t": "fire", "top": o.fire_public(top)})

    def _fire_notice(self, entry):
        now = self.now()
        if now - entry.lock_at < LOCK_NOTICE_MS:
            return
        entry.lock_at = now
        self.send(entry, {"t": "fire-locked", "org": self.org})

    def _lock_notice(self, entry, zone):
        now = self.now()
        if now - entry.lock_at < LOCK_NOTICE_MS:
            return
        entry.lock_at = now
        self.send(entry, {"t": "identity:locked", "z": zone,
                          "room": o.GATED_ZONES[zone], "org": self.org,
                          "guest": False})

    async def join(self, entry, m):
        await self.load()
        await self.refresh_fire(self.now())
        profile = entry.profile
        for other in list(self.users.values()):
            if other is not entry and \
                    other.profile["account_bi"] == profile["account_bi"]:
                await self.leave(other)
                self.close(other, 4001, "replaced")
        if len(self.users) >= o.MAX_USERS:
            self.send(entry, {"t": "denied", "reason": FULL_REASON})
            self.close(entry, 4003, "full")
            return
        x, y = m.get("x"), m.get("y")
        if o.is_walkable(x, y):
            pos = {"x": int(x), "y": int(y)}
        else:
            pos = o.random_spawn(self.rng)
        try:
            stored = await self.store.kv.get(
                o.profile_key(profile["account_bi"]))
        except Exception:
            stored = None
        entry.prof = o.clean_office_profile(stored)
        u = {
            "id": self._new_id(),
            "name": entry.prof.get("name") or profile["name"],
            "av": entry.prof.get("av", o.avatar_for(profile["account_bi"])),
            "x": pos["x"], "y": pos["y"], "d": 0,
            "mic": False, "cam": False, "away": False,
            "ex": {"org": profile["org"]},
        }
        entry.u = u
        zone = o.zone_at(u["x"], u["y"])
        if (zone in o.GATED_ZONES and not o.may_enter(profile, zone)) or \
                (zone == o.FIRE_ZONE and not self.is_top(entry)):
            spot = o.random_spawn(self.rng)
            u["x"], u["y"] = spot["x"], spot["y"]
        if profile["teams"]:
            u["ex"]["teams"] = profile["teams"]
        status = o.clean_status(m.get("status"))
        if status:
            u["ex"]["status"] = status
        if entry.prof.get("tz"):
            u["ex"]["tz"] = entry.prof["tz"]
        if profile.get("pic"):
            u["ex"]["pic"] = profile["pic"]
        key = o.owner_key(profile["account_bi"])
        desk = self.desks.get(key)
        if desk:
            u["ex"]["desk"] = {"x": desk["x"], "y": desk["y"]}
            await self._sync_desk_name(u, key)
        u["ex"]["fm"] = {"a": profile["account"], "n": profile["account"]}
        peers = [self.pack(e.u) for e in self.users.values()]
        self.users[u["id"]] = entry
        init = {"t": "init", "you": self.pack(u), "peers": peers,
                "office": self.org, "now": self.now()}
        init["identity"] = {
            "org": self.org, "signedIn": True, "rooms": o.GATED_ZONES,
            "unlocked": self.unlocked(entry),
        }
        init["fire"] = o.fire_public(self.top)
        init["workDesks"] = o.desk_list(self.desks)
        init["workDeskSpots"] = [[d["x"], d["y"]] for d in o.DESKS]
        thumbs = {}
        for board in o.BOARDS:
            if self._wb_can_view(entry, board):
                thumbs[board] = o.thumb_of(await self._board(board))
        init["wb"] = {"boards": list(o.BOARDS), "thumbs": thumbs,
                      "size": [o.BW, o.BH]}
        init["play"] = {"scores": self.scores,
                        "pong": [o.pack_pong(g, self.now())
                                 for g in self.games.values()]}
        self.send(entry, init)
        self.broadcast({"t": "j", "p": self.pack(u)}, except_entry=entry)
        try:
            u["pv"] = await self.store.visit_start(
                self.org_bi, profile["account_bi"], u["name"], self.now())
            await self._visits_seen()
        except Exception:
            u["pv"] = None

    async def _sync_desk_name(self, u, key):
        desk = self.desks.get(key)
        name = o.clean_work_text(u["name"], 24)
        if not desk or desk.get("name") == name:
            return
        self.desks = {**self.desks, key: {**desk, "name": name}}
        await self.store.kv.put("work-desks", self.desks)
        self.broadcast({"t": "work-desks", "desks": o.desk_list(self.desks)})

    def name_taken(self, entry, name):
        wanted = o.name_fold(name)
        for other in self.users.values():
            if other is entry:
                continue
            if wanted in (o.name_fold(other.u["name"]),
                          o.name_fold(other.profile["name"]),
                          other.profile["account"]):
                return True
        return False

    async def _visits_seen(self):
        ids = [e.u.get("pv") for e in self.users.values() if e.u]
        await self.store.visits_seen(self.org_bi, ids, self.now())

    async def leave(self, entry):
        self.entries.pop(id(entry.ws), None)
        u = entry.u
        if u is None or self.users.get(u["id"]) is not entry:
            return
        del self.users[u["id"]]
        self.dirty.discard(u["id"])
        self.broadcast({"t": "l", "id": u["id"]})
        self.admitted.pop(u["id"], None)
        for key, knock in list(self.knocks.items()):
            if knock["id"] != u["id"]:
                continue
            del self.knocks[key]
            done = dumps({"t": "meet-knock-end", "id": u["id"],
                          "z": knock["z"], "ok": False, "gone": True,
                          "name": u["name"]})
            for other in self._inside(knock["z"]):
                self.send(other, done)
        game = self._game_of(u["id"])
        if game:
            self._drop(game, u["id"], "left")
        now = self.now()
        try:
            if isinstance(u.get("pv"), int):
                await self.store.visit_end(self.org_bi, u["pv"], now)
            await self._visits_seen()
        except Exception:
            pass
        await self._maybe_prune(now)

    async def _maybe_prune(self, now):
        if now - self.pruned_at < PRUNE_EVERY_MS:
            return
        self.pruned_at = now
        try:
            await self.store.prune(self.org_bi, now)
        except Exception:
            pass

    def set_extra(self, u, key, value):
        ex = u["ex"]
        if value is None:
            if key not in ex:
                return
            del ex[key]
        else:
            if ex.get(key) == value:
                return
            ex[key] = value
        self.broadcast({"t": "ex", "id": u["id"], "k": key, "v": value})

    def schedule_flush(self):
        if self.flush_timer is None:
            self.flush_timer = self.schedule(o.FLUSH_MS, self.flush)

    def flush(self):
        self.flush_timer = None
        if not self.dirty:
            return
        moves = []
        for uid in self.dirty:
            entry = self.users.get(uid)
            if entry:
                u = entry.u
                moves.append([uid, u["x"], u["y"], u["d"]])
        self.dirty.clear()
        if moves:
            self.broadcast({"t": "mv", "u": moves})

    def cheer(self, entry):
        u = entry.u
        zone = o.zone_at(u["x"], u["y"])
        now = self.now()
        inside = self._inside(zone)
        marks = o.cheer_mark(self.cheers.get(zone) or {}, u["id"], now,
                             {e.u["id"] for e in inside})
        self.cheers[zone] = marks
        if len(marks) < o.CHEER_MIN or \
                now - self.cheer_at.get(zone, -o.CHEER_COOLDOWN_MS) < \
                o.CHEER_COOLDOWN_MS:
            return
        self.cheer_at[zone] = now
        self.cheers[zone] = {}
        payload = dumps({"t": "cheer", "z": zone, "n": len(marks),
                         "ids": sorted(marks)})
        for other in inside:
            self.send(other, payload)

    def _inside(self, zone, except_id=None):
        return [e for e in self.users.values()
                if e.u["id"] != except_id
                and o.zone_at(e.u["x"], e.u["y"]) == zone]

    async def _board(self, board):
        record = self.boards.get(board)
        if record is None:
            items = await self.store.kv.list("wb:%s:" % board)
            record = o.load_board(list(items.values()))
            self.boards[board] = record
        return record

    def _game_of(self, uid):
        for game in self.games.values():
            if uid in (game["players"]["L"], game["players"]["R"]):
                return game
        return None

    def _side_of(self, game, uid):
        if game["players"]["L"] == uid:
            return "L"
        if game["players"]["R"] == uid:
            return "R"
        return None

    def _audience(self, game):
        return [e for e in self.users.values()
                if e.u["id"] in (game["players"]["L"], game["players"]["R"])
                or o.near_table(game["table"], e.u["x"], e.u["y"], AUDIENCE)]

    def _push_pong(self, game, extra=None):
        payload = {"t": "play-pong", **o.pack_pong(game, self.now())}
        if extra:
            payload.update(extra)
        text = dumps(payload)
        for entry in self._audience(game):
            self.send(entry, text)

    def _away_from_end(self, game, u, side):
        table = game["table"]
        x = table["x0"] - 1 if side == "L" else table["x1"] + 1
        return (abs(u["x"] - x) > LEAVE_RANGE
                or u["y"] < table["y0"] - LEAVE_RANGE
                or u["y"] > table["y1"] + LEAVE_RANGE)

    def _finish(self, game, winner, reason=None):
        game["status"] = "over"
        game["winner"] = winner
        self._push_pong(game, {"why": reason} if reason else None)

        def clear():
            if self.games.get(game["table"]["id"]) is game and \
                    game["status"] == "over":
                del self.games[game["table"]["id"]]
                self._push_pong({**game, "status": "gone"})

        self.schedule(OVER_KEEP_MS, clear)

    def _drop(self, game, uid, reason):
        side = self._side_of(game, uid)
        if not side:
            return
        if game["status"] == "play":
            self._finish(game, "R" if side == "L" else "L", reason)
            return
        game["players"][side] = None
        game["names"][side] = ""
        if not game["players"]["L"] and not game["players"]["R"]:
            self.games.pop(game["table"]["id"], None)
            self._push_pong({**game, "status": "gone"})
        else:
            self._push_pong(game)

    def _tick(self):
        self.pong_timer = None
        now = self.now()
        live = 0
        for game in list(self.games.values()):
            if game["status"] != "play":
                continue
            gone = None
            for side in ("L", "R"):
                entry = self.users.get(game["players"][side])
                if not entry or self._away_from_end(game, entry.u, side):
                    gone = side
            if gone:
                self._finish(game, "R" if gone == "L" else "L", "left")
                continue
            point = o.step_pong(game, o.TICK_MS / 1000, now, self.rng)
            if game["status"] == "over":
                self._finish(game, game["winner"])
                continue
            live += 1
            self._push_pong(game, {"pt": point} if point else None)
        if live:
            self.pong_timer = self.schedule(o.TICK_MS, self._tick)

    def _start_ticks(self):
        if self.pong_timer is None:
            self.pong_timer = self.schedule(o.TICK_MS, self._tick)

    def _schedule_wb_persist(self):
        if self.wb_timer is None:
            self.wb_timer = self.schedule(WB_PERSIST_MS, self._persist_boards)

    async def _persist_boards(self):
        self.wb_timer = None
        pending = self.wb_dirty
        self.wb_dirty = {}
        puts = {}
        deletes = []
        for board, ids in pending.items():
            record = self.boards.get(board)
            for item_id in ids:
                key = "wb:%s:%s" % (board, item_id)
                item = record["items"].get(item_id) if record else None
                if item is None:
                    deletes.append(key)
                else:
                    puts[key] = item
        keys = list(puts)
        for index in range(0, len(keys), WB_PUT_CHUNK):
            chunk = {k: puts[k] for k in keys[index:index + WB_PUT_CHUNK]}
            try:
                await self.store.kv.put(chunk)
            except Exception:
                pass
        for index in range(0, len(deletes), WB_PUT_CHUNK):
            try:
                await self.store.kv.delete(deletes[index:index + WB_PUT_CHUNK])
            except Exception:
                pass

    def _schedule_thumb(self, board):
        if board in self.thumb_timers:
            return

        def send_thumb():
            self.thumb_timers.pop(board, None)
            record = self.boards.get(board)
            if record is not None:
                self.broadcast({"t": "wb.thumb", "b": board,
                                "th": o.thumb_of(record)},
                               only=lambda e: self._wb_can_view(e, board))

        self.thumb_timers[board] = self.schedule(THUMB_MS, send_thumb)

    def _wb_spot(self, board):
        point = o.parse_board_id(board)
        for dx, dy in ((0, 1), (-1, 0), (1, 0), (0, -1)):
            if o.is_walkable(point["x"] + dx, point["y"] + dy):
                return point["x"] + dx, point["y"] + dy
        return point["x"], point["y"] + 1

    def _wb_can_edit(self, u, board):
        point = o.parse_board_id(board)
        return bool(point) and o.zone_at(u["x"], u["y"]) == \
            o.zone_at(point["x"], point["y"])

    def _wb_can_view(self, entry, board):
        if self._wb_can_edit(entry.u, board):
            return True
        x, y = self._wb_spot(board)
        return self.can_enter(entry, x, y, True)

    def _wb_viewers(self, board):
        return [e for e in self.users.values()
                if (e.u["ex"].get("wb") or {}).get("b") == board]

    def _wb_relay(self, board, message, except_entry=None):
        text = dumps(message)
        for entry in self._wb_viewers(board):
            if entry is not except_entry:
                self.send(entry, text)

    def _wb_drawing(self, entry, board, on):
        current = entry.u["ex"].get("wb")
        if not current or current.get("b") != board:
            return
        if on:
            entry.wb_draw_at = self.now()
        if bool(current.get("d")) == on:
            return
        self.set_extra(entry.u, "wb", {"b": board, "d": 1 if on else 0})


async def _h_move(core, entry, m):
    u = entry.u
    x, y, d = o.to_int32(m.get("x")), o.to_int32(m.get("y")), \
        o.to_int32(m.get("d")) & 3
    ok = o.bucket(entry.buckets, "move", 20, 12, 1, core.now())
    if not ok or not core.can_enter(entry, x, y, False) or \
            abs(x - u["x"]) + abs(y - u["y"]) > 2:
        core.send(entry, {"t": "you", "x": u["x"], "y": u["y"]})
        return
    moved = (x, y) != (u["x"], u["y"])
    u["x"], u["y"], u["d"] = x, y, d
    core.dirty.add(u["id"])
    core.schedule_flush()
    if moved:
        table = (u["ex"].get("meeting") or {}).get("table")
        if table and o.table_at_seat(x, y) != table:
            core.set_extra(u, "meeting", None)
        if u["ex"].get("focus") and not o.is_couch(x, y):
            core.set_extra(u, "focus", None)


async def _h_signal(core, entry, m):
    if not o.bucket(entry.buckets, "sig", 200, 50, 1, core.now()):
        return
    target = core.users.get(o.js_str(m.get("to")))
    data = m.get("d")
    if target is None or target is entry or not isinstance(data, dict):
        return
    bye = set(data) == {"bye"}
    if not bye and not o.may_connect(entry.u, target.u):
        return
    core.send(target, {"t": "sig", "f": entry.u["id"], "d": data})


async def _h_say(core, entry, m):
    if not core.spend(entry) or not isinstance(m.get("to"), list):
        return
    text = o.js_str(m.get("text")).strip()[:o.SAY_MAX]
    if not text:
        return
    payload = dumps({"t": "say", "id": entry.u["id"], "text": text})
    for target_id in m["to"][:50]:
        target = core.users.get(o.js_str(target_id))
        if target and target is not entry and \
                o.may_connect(entry.u, target.u):
            core.send(target, payload)


async def _h_status(core, entry, m):
    if not o.bucket(entry.buckets, "st", 10, 2, 1, core.now()):
        return
    u = entry.u
    u["mic"], u["cam"], u["away"] = bool(m.get("mic")), bool(m.get("cam")), \
        bool(m.get("away"))
    core.broadcast({"t": "st", "id": u["id"], "mic": u["mic"],
                    "cam": u["cam"], "away": u["away"]}, except_entry=entry)


async def _h_emote(core, entry, m):
    if m.get("e") in o.EMOTES and core.spend(entry):
        core.broadcast({"t": "emo", "id": entry.u["id"], "e": m["e"]},
                       except_entry=entry)
        if m["e"] in o.CHEER_EMOTES:
            core.cheer(entry)


async def _h_wave(core, entry, m):
    target = core.users.get(o.js_str(m.get("to")))
    if target and target is not entry and core.spend(entry):
        core.send(target, {"t": "wave", "f": entry.u["id"],
                           "name": entry.u["name"]})


def _extra_status(core, entry, value):
    return o.clean_status(value)


def _extra_task(core, entry, value):
    return o.clean_task(value)


def _extra_meeting(core, entry, value):
    return o.clean_meeting(value, entry.u["x"], entry.u["y"], core.now())


def _extra_flag(core, entry, value):
    return True if value is True else None


def _extra_ride(core, entry, value):
    u = entry.u
    if not isinstance(value, dict):
        return None
    rid = o.js_str(value.get("id")) if "id" in value else "coaster"
    if rid == "coaster":
        y, x0, x1 = o.COASTER_PLATFORM
        if u["y"] != y or not x0 <= u["x"] <= x1:
            return None
        return {"id": rid, "at": core.now()}
    ride = o.RIDES.get(rid)
    if ride is None:
        return None
    x0, y0, x1, y1 = ride["board"]
    if not (x0 <= u["x"] <= x1 and y0 <= u["y"] <= y1):
        return None
    exit_ = ride["exit"]
    if exit_ and (u["x"], u["y"]) != exit_:
        u["x"], u["y"] = exit_
        core.send(entry, {"t": "you", "x": u["x"], "y": u["y"]})
        core.dirty.add(u["id"])
        core.schedule_flush()
        for key in ("meeting", "focus"):
            core.set_extra(u, key, None)
    return {"id": rid, "at": core.now()}


EXTRAS = {
    "ride": _extra_ride,
    "status": _extra_status,
    "task": _extra_task,
    "meeting": _extra_meeting,
    "sharing": _extra_flag,
    "coffee": _extra_flag,
}


async def _h_extra(core, entry, m):
    key = o.js_str(m.get("k"))
    check = EXTRAS.get(key)
    if check is None or not o.bucket(entry.buckets, "ex", 10, 2, 1,
                                     core.now()):
        return
    value = m.get("v")
    if value is None:
        core.set_extra(entry.u, key, None)
        return
    clean = check(core, entry, value)
    if clean is not None:
        core.set_extra(entry.u, key, clean)


async def _h_attendance(core, entry, m):
    if not core.spend(entry):
        return
    now = core.now()
    tz = m.get("tz")
    week = o.windows(now, tz)["week"]
    try:
        await core._visits_seen()
        rows = await core.store.visit_rows(core.org_bi, week, now)
    except Exception:
        rows = []
    summary = o.summarize(rows, now, tz,
                          "a:" + entry.profile["account_bi"])
    core.send(entry, {"t": "pr-att", **summary})


def _trim_inbox(box, now):
    ordered = sorted(box.items(), key=lambda kv: -abs(kv[1]))
    return {k: v for k, v in ordered[:SUMMON_KEEP]
            if now - abs(v) < SUMMON_TTL}


def _inbox(entry, now):
    return _trim_inbox(entry.summons, now)


async def _h_summon(core, entry, m):
    target = core.users.get(o.js_str(m.get("to")))

    def reply(ok, reason=None):
        message = {"t": "pr-summon-r", "to": target.u["id"] if target else None,
                   "name": target.u["name"] if target else "", "ok": ok}
        if reason:
            message["reason"] = reason
        core.send(entry, message)

    if not target or target is entry:
        reply(False, "gone")
        return
    now = core.now()
    box = _inbox(target, now)
    previous = box.get(entry.u["id"])
    if previous and now - abs(previous) < SUMMON_COOLDOWN:
        reply(False, "wait")
        return
    if not core.spend(entry):
        reply(False, "wait")
        return
    if (target.u["ex"].get("status") or {}).get("s") == "focus":
        reply(False, "focus")
        return
    box[entry.u["id"]] = now
    target.summons = _trim_inbox(box, now)
    core.send(target, {"t": "pr-summon", "f": entry.u["id"],
                       "name": entry.u["name"]})
    reply(True)


async def _h_summon_answer(core, entry, m):
    source = core.users.get(o.js_str(m.get("to")))
    now = core.now()
    box = _inbox(entry, now)
    at = box.get(o.js_str(m.get("to")))
    if not source or not at or at < 0:
        return
    box[source.u["id"]] = -at
    entry.summons = box
    core.send(source, {"t": "pr-summon-a", "f": entry.u["id"],
                       "name": entry.u["name"], "ok": not m.get("no")})


async def _h_desk_claim(core, entry, m):
    if not core.spend(entry):
        return
    x, y = o.to_int32(m.get("x")), o.to_int32(m.get("y"))
    result = o.claim_desk(core.desks, o.owner_key(entry.profile["account_bi"]),
                          entry.u["name"], x, y)
    if result.get("error"):
        core.send(entry, {"t": "work-err", "reason": result["error"],
                          "by": result.get("by")})
        return
    core.desks = result["desks"]
    previous = result.get("prev")
    if previous and previous.get("deco"):
        key = o.owner_key(entry.profile["account_bi"])
        core.desks[key] = {**core.desks[key], "deco": previous["deco"]}
    await core.store.kv.put("work-desks", core.desks)
    core.set_extra(entry.u, "desk", {"x": x, "y": y})
    core.broadcast({"t": "work-desks", "desks": o.desk_list(core.desks)})


async def _h_desk_release(core, entry, m):
    if not core.spend(entry):
        return
    result = o.release_desk(core.desks,
                            o.owner_key(entry.profile["account_bi"]))
    if not result["prev"]:
        return
    core.desks = result["desks"]
    await core.store.kv.put("work-desks", core.desks)
    core.set_extra(entry.u, "desk", None)
    core.broadcast({"t": "work-desks", "desks": o.desk_list(core.desks)})


async def _h_focus_start(core, entry, m):
    now = core.now()
    if not o.bucket(entry.buckets, "ex", 10, 2, 1, now):
        return
    u = entry.u
    seated = o.is_couch(u["x"], u["y"])
    joining = m.get("join") is not None
    if joining:
        host = core.users.get(o.js_str(m.get("join")))
        if host is entry:
            host = None
        clean = o.join_focus(host.u["ex"].get("focus") if host else None,
                             host.u["id"] if host else "", seated, now)
    else:
        clean = o.clean_focus(m, seated, now)
    if clean is None:
        reason = "seat" if not seated else "gone" if joining else "bad"
        core.send(entry, {"t": "focus-err", "reason": reason})
        return
    core.set_extra(u, "focus", clean)
    key = entry.profile["account_bi"]
    if joining or m.get("notify") is not True or \
            now - core.focus_note_at.get(key, 0) < FOCUS_NOTE_MS:
        return
    core.focus_note_at = {k: t for k, t in core.focus_note_at.items()
                          if now - t < FOCUS_NOTE_MS}
    core.focus_note_at[key] = now
    payload = dumps({"t": "focus-open", "id": u["id"], "name": u["name"],
                     "focus": clean})
    for other in core.users.values():
        ex = other.u["ex"]
        if other is entry or ex.get("focus") or \
                (ex.get("status") or {}).get("s") == "focus":
            continue
        core.send(other, payload)


async def _h_focus_end(core, entry, m):
    core.set_extra(entry.u, "focus", None)


async def _h_profile_set(core, entry, m):
    if not core.spend(entry):
        return
    u = entry.u
    prof = dict(entry.prof)
    if "name" in m:
        name = o.clean_display_name(m.get("name"))
        if not name or name == entry.profile["name"]:
            prof.pop("name", None)
        elif core.name_taken(entry, name):
            core.send(entry, {"t": "prof-err", "reason": "taken"})
            return
        else:
            prof["name"] = name
    if "tz" in m:
        tz = o.clean_tz(m.get("tz"))
        if tz:
            prof["tz"] = tz
        else:
            prof.pop("tz", None)
    if "av" in m:
        avatar = o.clean_avatar(m.get("av"))
        if avatar is None:
            if m.get("av") is not None:
                core.send(entry, {"t": "prof-err", "reason": "avatar"})
                return
            prof.pop("av", None)
        else:
            prof["av"] = avatar
    key = o.profile_key(entry.profile["account_bi"])
    if prof != entry.prof:
        try:
            if prof:
                await core.store.kv.put(key, prof)
            else:
                await core.store.kv.delete([key])
        except Exception:
            core.send(entry, {"t": "prof-err", "reason": "store"})
            return
        entry.prof = prof
    name = prof.get("name") or entry.profile["name"]
    avatar = prof.get("av", o.avatar_for(entry.profile["account_bi"]))
    if name != u["name"] or avatar != u["av"]:
        renamed = name != u["name"]
        u["name"], u["av"] = name, avatar
        core.broadcast({"t": "prof", "id": u["id"], "name": name,
                        "av": avatar})
        if renamed:
            await core._sync_desk_name(
                u, o.owner_key(entry.profile["account_bi"]))
    core.set_extra(u, "tz", prof.get("tz"))
    core.send(entry, {"t": "prof-ok", "name": u["name"],
                      "custom": "name" in prof, "tz": prof.get("tz") or "",
                      "av": u["av"]})


async def _h_desk_deco(core, entry, m):
    if not core.spend(entry):
        return
    key = o.owner_key(entry.profile["account_bi"])
    desk = core.desks.get(key)
    deco = o.clean_deco(m.get("deco"))
    if not desk or deco is None:
        core.send(entry, {"t": "work-err",
                          "reason": "deco" if desk else "no_desk"})
        return
    updated = {k: v for k, v in desk.items() if k != "deco"}
    if not o.deco_empty(deco):
        updated["deco"] = deco
    if updated == desk:
        core.send(entry, {"t": "work-deco", "ok": True})
        return
    core.desks = {**core.desks, key: updated}
    await core.store.kv.put("work-desks", core.desks)
    core.broadcast({"t": "work-desks", "desks": o.desk_list(core.desks)})
    core.send(entry, {"t": "work-deco", "ok": True})


def _matches(entry, target):
    wanted = o.js_str(target).strip().lower()
    return bool(wanted) and entry.profile["account"] == wanted


async def _h_work_ping(core, entry, m):
    if not core.spend(entry) or not isinstance(m.get("to"), list):
        return
    task = o.clean_task(m.get("task"))
    what = m.get("what") if m.get("what") in PING_WHAT else "updated"
    targets = m["to"][:10]
    payload = dumps({"t": "work-ping", "from": entry.u["id"],
                     "name": entry.u["name"], "task": task, "what": what})
    count = 0
    for other in core.users.values():
        if other is entry:
            continue
        if any(_matches(other, t) for t in targets):
            core.send(other, payload)
            count += 1
    core.send(entry, {"t": "work-pinged", "n": count})


async def _h_agents(core, entry, m):
    if not core.spend(entry):
        return
    changes = o.clean_relay(m.get("ch"), core.now())
    if not changes:
        return
    core.broadcast({"t": "agents", "f": entry.u["id"], "ch": changes},
                   except_entry=entry)


def _knock_key(uid, zone):
    return "%s:%s" % (uid, zone)


async def _h_knock(core, entry, m):
    zone = o.js_str(m.get("z"))
    u = entry.u
    if not o.is_private(zone):
        return
    if o.zone_at(u["x"], u["y"]) == zone:
        core.send(entry, {"t": "meet-knock-r", "z": zone, "ok": False,
                          "why": "inside"})
        return
    if zone in core.admitted.get(u["id"], set()):
        core.send(entry, {"t": "meet-knock-r", "z": zone, "ok": True, "n": 0,
                          "already": True})
        return
    now = core.now()
    if not o.knock_allowed(entry.knock_at.get(zone), now) or \
            not core.spend(entry):
        core.send(entry, {"t": "meet-knock-r", "z": zone, "ok": False,
                          "why": "wait"})
        return
    people = core._inside(zone, u["id"])
    if not people:
        core.send(entry, {"t": "meet-knock-r", "z": zone, "ok": False,
                          "why": "empty", "n": 0})
        return
    entry.knock_at[zone] = now
    core.knocks[_knock_key(u["id"], zone)] = {"id": u["id"], "z": zone,
                                              "at": now}
    payload = dumps({"t": "meet-knock", "id": u["id"], "name": u["name"],
                     "z": zone})
    for other in people:
        core.send(other, payload)
    core.send(entry, {"t": "meet-knock-r", "z": zone, "ok": True,
                      "n": len(people)})


async def _settle(core, entry, m, ok):
    uid, zone = o.js_str(m.get("id")), o.js_str(m.get("z"))
    key = _knock_key(uid, zone)
    knock = core.knocks.get(key)
    if not knock or core.now() - knock["at"] > o.KNOCK_TTL_MS:
        return
    if o.zone_at(entry.u["x"], entry.u["y"]) != zone:
        return
    target = core.users.get(uid)
    del core.knocks[key]
    if not target:
        return
    if ok:
        core.admitted.setdefault(uid, set()).add(zone)
        core.send(target, {"t": "meet-admitted", "z": zone,
                           "by": entry.u["name"]})
    else:
        core.send(target, {"t": "meet-knock-denied", "z": zone,
                           "by": entry.u["name"]})
    payload = dumps({"t": "meet-knock-end", "id": uid, "z": zone,
                     "by": entry.u["name"], "ok": bool(ok),
                     "name": target.u["name"]})
    for other in core._inside(zone):
        core.send(other, payload)


async def _h_admit(core, entry, m):
    await _settle(core, entry, m, True)


async def _h_deny(core, entry, m):
    await _settle(core, entry, m, False)


async def _h_wb_open(core, entry, m):
    board = m.get("b")
    if board not in o.BOARDS or not o.bucket(entry.buckets, "wbOpen", 5, 1,
                                             1, core.now()):
        return
    if not core._wb_can_view(entry, board):
        core.send(entry, {"t": "wb.deny", "b": board,
                          "reason": "This room is private."})
        return
    record = await core._board(board)
    if entry.u is None or core.users.get(entry.u["id"]) is not entry:
        return
    if (entry.u["ex"].get("wb") or {}).get("b") != board:
        core.set_extra(entry.u, "wb", {"b": board, "d": 0})
    chunks = o.chunk_items(o.board_items(record))
    editable = core._wb_can_edit(entry.u, board)
    for index, items in enumerate(chunks):
        core.send(entry, {"t": "wb.state", "b": board, "items": items,
                          "part": index, "done": index == len(chunks) - 1,
                          "edit": editable})


async def _h_wb_close(core, entry, m):
    if entry.u["ex"].get("wb"):
        core.set_extra(entry.u, "wb", None)


async def _h_wb_op(core, entry, m):
    board = m.get("b")
    seq = m.get("s")

    def nack(error):
        core.send(entry, {"t": "wb.nack", "b": board, "s": seq, "err": error})

    if board not in o.BOARDS or \
            (entry.u["ex"].get("wb") or {}).get("b") != board:
        nack("closed")
        return
    if not core._wb_can_edit(entry.u, board):
        nack("view")
        return
    now = core.now()
    cost = len(dumps(m.get("op")))
    if not o.bucket(entry.buckets, "wbOps", 30, 12, 1, now) or \
            not o.bucket(entry.buckets, "wbBytes", 64000, 24000, cost, now):
        nack("rate")
        return
    record = await core._board(board)
    result = o.apply_op(record, m.get("op"), entry.u["id"])
    if not result["ok"]:
        nack(result["err"] or "noop")
        return
    dirty = core.wb_dirty.setdefault(board, set())
    if result["op"] and result["op"].get("o") == "clear":
        dirty.update(result["del"])
    else:
        dirty.update(item["id"] for item in result["put"])
        dirty.update(result["del"])
    core._schedule_wb_persist()
    core._wb_relay(board, {"t": "wb.op", "b": board, "by": entry.u["id"],
                           "s": seq, "op": result["op"]})
    if result["err"] == "full":
        core.send(entry, {"t": "wb.nack", "b": board, "s": None,
                          "err": "full"})
    core._wb_drawing(entry, board, True)
    core._schedule_thumb(board)


async def _h_wb_cursor(core, entry, m):
    board = m.get("b")
    if board not in o.BOARDS or \
            (entry.u["ex"].get("wb") or {}).get("b") != board or \
            not o.bucket(entry.buckets, "wbFast", 40, 30, 1, core.now()):
        return
    x, y = o.finite(m.get("x")), o.finite(m.get("y"))
    if x is None or y is None:
        return
    core._wb_relay(board, {
        "t": "wb.cur", "b": board, "id": entry.u["id"],
        "x": max(-1, min(o.BW + 1, o.js_round(x))),
        "y": max(-1, min(o.BH + 1, o.js_round(y)))}, except_entry=entry)
    if entry.u["ex"]["wb"].get("d") and \
            core.now() - entry.wb_draw_at > DRAW_IDLE_MS:
        core._wb_drawing(entry, board, False)


async def _h_wb_live(core, entry, m):
    board = m.get("b")
    if board not in o.BOARDS or \
            (entry.u["ex"].get("wb") or {}).get("b") != board or \
            not core._wb_can_edit(entry.u, board) or \
            not o.bucket(entry.buckets, "wbFast", 40, 30, 1, core.now()):
        return
    points = m.get("pts")
    stroke = o.js_str(m.get("id"))
    if not isinstance(points, list) or len(points) > 600 or \
            not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", stroke):
        return
    clean = []
    for value in points:
        number = o.finite(value)
        clean.append(int(max(-100000, min(100000, o.js_round(number))))
                     if number is not None else 0)
    color = m.get("c") if isinstance(m.get("c"), str) and \
        re.fullmatch(r"#[0-9a-fA-F]{6}", m.get("c")) else "#1d2030"
    width = max(1, min(60, o.to_int32(m.get("w"))))
    core._wb_relay(board, {"t": "wb.live", "b": board, "by": entry.u["id"],
                           "id": stroke, "c": color, "w": width,
                           "hl": 1 if m.get("hl") else 0, "pts": clean,
                           "end": 1 if m.get("end") else 0},
                   except_entry=entry)
    core._wb_drawing(entry, board, True)


async def _h_pong_join(core, entry, m):
    if not core.spend(entry):
        return
    u = entry.u
    wanted = o.js_str(m.get("table"))
    table = next((t for t in o.PONG_TABLES if t["id"] == wanted), None) or \
        next((t for t in o.PONG_TABLES if o.pong_side(t, u["x"], u["y"])),
             None)
    if not table:
        core.send(entry, {"t": "play-err", "reason": "No ping-pong table here"})
        return
    side = o.pong_side(table, u["x"], u["y"])
    if not side:
        core.send(entry, {"t": "play-err",
                          "reason": "Stand at one end of the table to play"})
        return
    mine = core._game_of(u["id"])
    if mine and mine["table"]["id"] != table["id"]:
        core._drop(mine, u["id"], "left")
    game = core.games.get(table["id"])
    if game and game["status"] == "over":
        del core.games[table["id"]]
        game = None
    if not game:
        game = o.new_pong(table)
        core.games[table["id"]] = game
    if game["status"] == "play":
        if core._side_of(game, u["id"]):
            core._push_pong(game)
        else:
            core.send(entry, {"t": "play-err",
                              "reason": "A game is already on at this table"})
        return
    for s in ("L", "R"):
        player = core.users.get(game["players"][s])
        if game["players"][s] and (not player or
                                   core._away_from_end(game, player.u, s)):
            game["players"][s] = None
            game["names"][s] = ""
    current = core._side_of(game, u["id"])
    if current and current != side:
        game["players"][current] = None
        game["names"][current] = ""
    if game["players"][side] and game["players"][side] != u["id"]:
        core.send(entry, {"t": "play-err",
                          "reason": "That end is taken, try the other one"})
        return
    game["players"][side] = u["id"]
    game["names"][side] = u["name"]
    if game["players"]["L"] and game["players"]["R"]:
        o.start_pong(game, core.now(), core.rng)
        core._start_ticks()
    core._push_pong(game)


async def _h_pong_pad(core, entry, m):
    game = core._game_of(entry.u["id"])
    if game:
        o.set_pad(game, core._side_of(game, entry.u["id"]), m.get("y"))


async def _h_pong_leave(core, entry, m):
    game = core._game_of(entry.u["id"])
    if game:
        core._drop(game, entry.u["id"], "left")


async def _h_arcade_start(core, entry, m):
    if not core.spend(entry):
        return
    if not o.near_tile(entry.u["x"], entry.u["y"], "A", o.USE_RADIUS):
        core.send(entry, {"t": "play-err",
                          "reason": "Walk up to an arcade machine first"})
        return
    entry.arcade = core.now()
    core.send(entry, {"t": "play-arcade-go"})


async def _h_arcade_score(core, entry, m):
    started = entry.arcade
    entry.arcade = None
    if started is None:
        core.send(entry, {"t": "play-arcade-result", "ok": False,
                          "reason": "No game in progress"})
        return
    score = m.get("score")
    elapsed = core.now() - started
    if not o.plausible_score(score, elapsed):
        core.send(entry, {"t": "play-arcade-result", "ok": False,
                          "reason": "Score not accepted"})
        return
    score = int(score)
    result = o.insert_score(core.scores, {
        "k": "a:" + o.alias("a:" + entry.profile["account_bi"]),
        "name": entry.u["name"], "score": score, "ts": core.now()})
    core.send(entry, {"t": "play-arcade-result", "ok": True, "score": score,
                      "rank": result["rank"], "top": result["top"]})
    if not result["rank"]:
        return
    core.scores = result["list"]
    await core.store.kv.put("play:scores", core.scores)
    core.broadcast({"t": "play-scores", "list": core.scores})
    if result["top"]:
        core.broadcast({"t": "play-hi", "id": entry.u["id"],
                        "name": entry.u["name"], "score": score})


async def _h_coffee(core, entry, m):
    if not core.spend(entry):
        return
    if not o.near_tile(entry.u["x"], entry.u["y"], "C", o.USE_RADIUS):
        core.send(entry, {"t": "play-err",
                          "reason": "Walk up to the coffee machine first"})
        return
    now = core.now()
    day = o.day_key(now)
    if core.coffee.get("day") != day:
        core.coffee = {"day": day, "counts": {}, "last": {}}
    key = o.alias("a:" + entry.profile["account_bi"])
    last = core.coffee["last"].get(key, 0)
    fresh = now - last >= COFFEE_REPEAT_MS
    if fresh:
        core.coffee["last"][key] = now
        core.coffee["counts"][key] = core.coffee["counts"].get(key, 0) + 1
        await core.store.kv.put("play:coffee", core.coffee)
    core.send(entry, {"t": "play-coffee",
                      "n": core.coffee["counts"].get(key, 0),
                      "repeat": not fresh})


async def _h_cooler(core, entry, m):
    spot = o.near_tile(entry.u["x"], entry.u["y"], "Q", o.USE_RADIUS)
    if not spot:
        core.send(entry, {"t": "play-err",
                          "reason": "Walk up to the water cooler first"})
        return
    now = core.now()
    key = "%d,%d" % (spot["x"], spot["y"])
    if now - core.cooler_at.get(key, 0) < COOLER_COOLDOWN_MS:
        core.send(entry, {"t": "play-err",
                          "reason": "Give the last question a moment"})
        return
    core.cooler_at[key] = now
    question = o.pick_question(core.cooler_last, core.rng)
    core.cooler_last = question
    payload = dumps({"t": "play-icebreaker", "q": question,
                     "by": entry.u["id"], "name": entry.u["name"],
                     "x": spot["x"], "y": spot["y"]})
    for other in core.users.values():
        if max(abs(other.u["x"] - spot["x"]),
               abs(other.u["y"] - spot["y"])) <= o.COOLER_RADIUS:
            core.send(other, payload)


HANDLERS = {
    "m": _h_move,
    "sig": _h_signal,
    "say": _h_say,
    "st": _h_status,
    "emo": _h_emote,
    "wave": _h_wave,
    "ex": _h_extra,
    "pr-att": _h_attendance,
    "pr-summon": _h_summon,
    "pr-summon-a": _h_summon_answer,
    "work-claim": _h_desk_claim,
    "work-release": _h_desk_release,
    "work-deco": _h_desk_deco,
    "work-ping": _h_work_ping,
    "agents": _h_agents,
    "meet-knock": _h_knock,
    "meet-admit": _h_admit,
    "meet-deny": _h_deny,
    "wb.open": _h_wb_open,
    "wb.close": _h_wb_close,
    "wb.op": _h_wb_op,
    "wb.cur": _h_wb_cursor,
    "wb.live": _h_wb_live,
    "play-pong-join": _h_pong_join,
    "play-pong-pad": _h_pong_pad,
    "play-pong-leave": _h_pong_leave,
    "play-arcade-start": _h_arcade_start,
    "play-arcade-score": _h_arcade_score,
    "play-coffee": _h_coffee,
    "play-cooler": _h_cooler,
    "prof-set": _h_profile_set,
    "focus-start": _h_focus_start,
    "focus-end": _h_focus_end,
}


class _Storage:
    def __init__(self, storage):
        self.storage = storage

    async def get(self, key):
        return _decode_value(await self.storage.get(key))

    async def put(self, key, value=None):
        if isinstance(key, dict):
            await self.storage.put(to_js({k: dumps(v) for k, v in key.items()}))
        else:
            await self.storage.put(key, dumps(value))

    async def delete(self, keys):
        await self.storage.delete(to_js([str(k) for k in keys]))

    async def list(self, prefix):
        found = await self.storage.list(to_js({"prefix": prefix}))
        if isinstance(found, dict):
            pairs = found.items()
        else:
            try:
                pairs = [(p[0], p[1]) for p in found.entries()]
            except Exception:
                pairs = []
        out = {}
        for key, value in pairs:
            decoded = _decode_value(value)
            if decoded is not None:
                out[str(key)] = decoded
        return out


def _decode_value(value):
    if not isinstance(value, str):
        return None
    try:
        return json.loads(value)
    except Exception:
        return None


class OrgOfficeDurable:
    traffic_binding = "FORKMESH_ORG_OFFICE"

    def _core(self):
        core = getattr(self, "_office_core", None)
        if core is None:
            env = self.env

            async def first(sql, *args):
                return await d1_first(env, sql, *args)

            async def rows(sql, *args):
                return await d1_all(env, sql, *args)

            async def run(sql, *args):
                return await d1_run(env, sql, *args)

            async def seal(record):
                return await encrypt_row(env, record)

            async def unseal(stored):
                return await decrypt_row(env, stored)

            store = SqlOfficeStore(first, rows, run, seal, unseal,
                                   _Storage(self.ctx.storage))
            core = OfficeCore("", store, recheck=self._recheck)
            self._office_core = core
        return core

    async def _recheck(self, profile):
        return await org_office_member_profile(
            self.env, profile["org_bi"], profile["account_bi"],
            profile["session_id"])

    async def fetch(self, request):
        path = urlparse(request.url).path
        match = ROOM_PATH_RE.fullmatch(path)
        if not match:
            return json_response({"error": "not_found"}, status=404)
        org_bi, action = match.groups()
        core = self._core()
        if not core.org_bi:
            core.org_bi = org_bi
        if core.org_bi != org_bi:
            return json_response({"error": "not_found"}, status=404)
        if action == "revoke":
            if method_name(request) != "POST":
                return json_response({"error": "not_found"}, status=404)
            for entry in list(core.entries.values()):
                await core.leave(entry)
                core.close(entry, 4003, "access revoked")
            return json_response({"ok": True})
        if method_name(request) != "GET" or \
                str(request.headers.get("upgrade") or "").lower() != \
                "websocket":
            return json_response({"error": "not_found"}, status=404)
        profile = decode_claim(request.headers.get(CLAIM_HEADER))
        if not profile or profile["org_bi"] != org_bi:
            return json_response({"error": "not_found"}, status=404)
        if any(e.profile["nonce"] == profile["nonce"]
               for e in core.entries.values()):
            return json_response({"error": "ticket_replayed"}, status=409)
        client, server = WebSocketPair.new().object_values()
        self.ctx.acceptWebSocket(server, to_js([SOCKET_TAG]))
        refused = core.connect(server, profile)
        if refused:
            try:
                server.send(dumps({"t": "denied", "reason": FULL_REASON}))
                server.close(4003, refused)
            except Exception:
                pass
            return JsResponse.new(
                None, to_js({"status": 101, "webSocket": client}))
        try:
            server.serializeAttachment(to_js({"org_bi": org_bi}))
        except Exception:
            pass
        return JsResponse.new(None, to_js({"status": 101, "webSocket": client}))

    async def webSocketMessage(self, ws, message):
        core = self._core()
        if core.entry_for(ws) is None:
            try:
                ws.close(4003, "session ended")
            except Exception:
                pass
            return
        await core.message(ws, message if isinstance(message, str) else None)

    async def webSocketClose(self, ws, code, reason, was_clean):
        core = self._core()
        entry = core.entry_for(ws)
        if entry is not None:
            await core.leave(entry)
        try:
            ws.close(1000, "")
        except Exception:
            pass

    async def webSocketError(self, ws, error):
        core = self._core()
        entry = core.entry_for(ws)
        if entry is not None:
            await core.leave(entry)

    web_socket_message = webSocketMessage
    web_socket_close = webSocketClose
    web_socket_error = webSocketError
