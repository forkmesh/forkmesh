import re


PREFIX = "/api/video-mix-dj-studio"
PLUGIN_ID = "video-mix-dj-studio"
REQUEST_PROOF = "forkmesh-video-mix-dj-studio-request-v1"
MAX_BODY_BYTES = 16 * 1024
MAX_JAMS_PER_ACCOUNT = 20
MAX_LISTED_JAMS = 50
MAX_MEMBERS = 32
MAX_QUEUE = 100
MAX_POSITION = 864_000.0
PITCH_RANGE = 0.16
SIDES = ("a", "b")
ACTIVE = ("owner", "joined")
VISIBLE = ("owner", "joined", "invited")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
NAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")
VIDEO_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")
REACTION_RE = re.compile(r"^[a-z0-9-]{1,24}$")
STATES = ("studio", "listening", "idle")
MAX_MOTION = 16
MOTION_LIMIT = 2.0
PRESENCE_SECONDS = 30
REACTION_SECONDS = 12
MAX_REACTIONS = 60
MAX_REACTIONS_PER_MEMBER = 8

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS vmdj_jams (
        jam_id TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_vmdj_jams_owner ON vmdj_jams(owner_bi)",
    """CREATE TABLE IF NOT EXISTS vmdj_jam_members (
        jam_id TEXT NOT NULL,
        account_name TEXT NOT NULL,
        account_bi TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL CHECK(status IN ('owner','joined','invited')),
        invited_by TEXT NOT NULL DEFAULT '',
        joined_at INTEGER NOT NULL DEFAULT 0 CHECK(joined_at >= 0),
        PRIMARY KEY(jam_id,account_name))""",
    "CREATE INDEX IF NOT EXISTS idx_vmdj_jam_members_name ON vmdj_jam_members(account_name,status)",
    """CREATE TABLE IF NOT EXISTS vmdj_jam_queue (
        item_id TEXT PRIMARY KEY,
        jam_id TEXT NOT NULL,
        added_by TEXT NOT NULL,
        added_at INTEGER NOT NULL CHECK(added_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_vmdj_jam_queue_jam ON vmdj_jam_queue(jam_id,added_at)",
    """CREATE TABLE IF NOT EXISTS vmdj_jam_decks (
        jam_id TEXT NOT NULL,
        side TEXT NOT NULL CHECK(side IN ('a','b')),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        updated_by TEXT NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY(jam_id,side))""",
    """CREATE TABLE IF NOT EXISTS vmdj_jam_presence (
        jam_id TEXT NOT NULL,
        account_name TEXT NOT NULL,
        seen_at INTEGER NOT NULL CHECK(seen_at >= 0),
        data TEXT NOT NULL,
        PRIMARY KEY(jam_id,account_name))""",
    """CREATE TABLE IF NOT EXISTS vmdj_jam_reactions (
        reaction_id TEXT PRIMARY KEY,
        jam_id TEXT NOT NULL,
        account_name TEXT NOT NULL,
        at INTEGER NOT NULL CHECK(at >= 0),
        kind TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_vmdj_jam_reactions_jam ON vmdj_jam_reactions(jam_id,at)",
]

DATA_SCHEMA_DOWN = [
    "DROP INDEX IF EXISTS idx_vmdj_jam_reactions_jam",
    "DROP TABLE IF EXISTS vmdj_jam_reactions",
    "DROP TABLE IF EXISTS vmdj_jam_presence",
    "DROP TABLE IF EXISTS vmdj_jam_decks",
    "DROP INDEX IF EXISTS idx_vmdj_jam_queue_jam",
    "DROP TABLE IF EXISTS vmdj_jam_queue",
    "DROP INDEX IF EXISTS idx_vmdj_jam_members_name",
    "DROP TABLE IF EXISTS vmdj_jam_members",
    "DROP INDEX IF EXISTS idx_vmdj_jams_owner",
    "DROP TABLE IF EXISTS vmdj_jams",
]

_SCHEMA_READY = False


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _error(runtime, code, status=400):
    return _response(runtime, {"ok": False, "error": code}, status)


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def _name(value):
    name = _clean(value, 64).lower().lstrip("@")
    return name if NAME_RE.fullmatch(name) else ""


def _number(value, default, low, high):
    if isinstance(value, bool):
        return default
    try:
        number = float(value)
    except (TypeError, ValueError):
        return default
    if number != number or number in (float("inf"), float("-inf")):
        return default
    return min(high, max(low, number))


def _video(value):
    video = str(value or "").strip()
    return video if VIDEO_RE.fullmatch(video) else ""


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ()
    if not clean.startswith(PREFIX + "/"):
        return None
    parts = tuple(part for part in clean[len(PREFIX) + 1:].split("/") if part)
    if any(len(part) > 64 for part in parts):
        return None
    return parts


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _name((record or {}).get("name"))


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


async def _opened(runtime, value):
    opened = await runtime.open(value) if value else None
    return opened if isinstance(opened, dict) else {}


async def _jam_row(runtime, jam_id, account_bi, actor):
    return await runtime.d1_first(
        "SELECT j.jam_id,j.owner_bi,j.owner_name,j.created_at,j.updated_at,j.data,m.status "
        "FROM vmdj_jams j JOIN vmdj_jam_members m ON m.jam_id=j.jam_id "
        "WHERE j.jam_id=? AND m.account_name=? AND m.status IN ('owner','joined','invited') "
        "AND (m.account_bi='' OR m.account_bi=?)",
        jam_id, actor, account_bi,
    )


async def _members(runtime, jam_id):
    return await runtime.d1_all(
        "SELECT account_name,account_bi,status,invited_by,joined_at FROM vmdj_jam_members "
        "WHERE jam_id=? ORDER BY CASE status WHEN 'owner' THEN 0 WHEN 'joined' THEN 1 ELSE 2 END,"
        "joined_at,account_name LIMIT ?",
        jam_id, MAX_MEMBERS + 1,
    ) or []


async def _queue(runtime, jam_id):
    rows = await runtime.d1_all(
        "SELECT item_id,added_by,added_at,data FROM vmdj_jam_queue WHERE jam_id=? "
        "ORDER BY added_at,item_id LIMIT ?",
        jam_id, MAX_QUEUE,
    ) or []
    items = []
    for row in rows:
        data = await _opened(runtime, row.get("data"))
        video = _video(data.get("videoId"))
        if not video:
            continue
        items.append({
            "id": str(row.get("item_id") or ""),
            "videoId": video,
            "title": _clean(data.get("title"), 300),
            "author": _clean(data.get("author"), 200),
            "addedBy": str(row.get("added_by") or ""),
        })
    return items


async def _decks(runtime, jam_id):
    rows = await runtime.d1_all(
        "SELECT side,updated_at,updated_by,data FROM vmdj_jam_decks WHERE jam_id=?", jam_id) or []
    decks = {}
    for row in rows:
        side = str(row.get("side") or "")
        if side not in SIDES:
            continue
        data = await _opened(runtime, row.get("data"))
        decks[side] = dict(_deck(data), at=int(row.get("updated_at") or 0),
                           by=str(row.get("updated_by") or ""))
    return decks


def _motion(value):
    samples = []
    for item in value if isinstance(value, list) else []:
        if len(samples) >= MAX_MOTION:
            break
        if not isinstance(item, (list, tuple)) or len(item) != 2:
            continue
        samples.append([round(_number(part, 0.0, -MOTION_LIMIT, MOTION_LIMIT), 2) for part in item])
    return samples


def _presence_data(data):
    state = str(data.get("state") or "")
    return {
        "state": state if state in STATES else "studio",
        "playing": data.get("playing") is True,
        "energy": round(_number(data.get("energy"), 0.0, 0.0, 1.0), 3),
        "motion": _motion(data.get("motion")),
    }


async def _presence(runtime, jam_id, now):
    rows = await runtime.d1_all(
        "SELECT account_name,seen_at,data FROM vmdj_jam_presence WHERE jam_id=? AND seen_at>=? "
        "ORDER BY account_name LIMIT ?",
        jam_id, now - PRESENCE_SECONDS * 1000, MAX_MEMBERS + 1,
    ) or []
    people = []
    for row in rows:
        data = _presence_data(await _opened(runtime, row.get("data")))
        people.append(dict(data, name=str(row.get("account_name") or ""),
                           ago=max(0, now - int(row.get("seen_at") or 0))))
    return people


async def _reactions(runtime, jam_id, now):
    rows = await runtime.d1_all(
        "SELECT reaction_id,account_name,at,kind FROM vmdj_jam_reactions WHERE jam_id=? AND at>=? "
        "ORDER BY at,reaction_id LIMIT ?",
        jam_id, now - REACTION_SECONDS * 1000, MAX_REACTIONS,
    ) or []
    return [{"id": str(row.get("reaction_id") or ""), "name": str(row.get("account_name") or ""),
             "kind": str(row.get("kind") or ""), "ago": max(0, now - int(row.get("at") or 0))}
            for row in rows]


async def _save_presence(runtime, jam_id, actor, data):
    await runtime.d1_run(
        "INSERT INTO vmdj_jam_presence(jam_id,account_name,seen_at,data) VALUES(?,?,?,?) "
        "ON CONFLICT(jam_id,account_name) DO UPDATE SET seen_at=excluded.seen_at,data=excluded.data",
        jam_id, actor, runtime.now(), await runtime.seal(_presence_data(data)),
    )


async def _react(runtime, jam_id, actor, data):
    kind = str(data.get("kind") or "").strip()
    if not REACTION_RE.fullmatch(kind):
        return _error(runtime, "invalid_reaction")
    now = runtime.now()
    await runtime.d1_run(
        "DELETE FROM vmdj_jam_reactions WHERE jam_id=? AND at<?", jam_id, now - REACTION_SECONDS * 1000)
    recent = await runtime.d1_first(
        "SELECT COUNT(*) AS total FROM vmdj_jam_reactions WHERE jam_id=? AND account_name=?", jam_id, actor)
    if int((recent or {}).get("total") or 0) >= MAX_REACTIONS_PER_MEMBER:
        return _error(runtime, "too_many_reactions", 429)
    await runtime.d1_run(
        "INSERT INTO vmdj_jam_reactions(reaction_id,jam_id,account_name,at,kind) VALUES(?,?,?,?,?)",
        runtime.new_id(), jam_id, actor, now, kind,
    )
    return None


async def _forget_member(runtime, jam_id, name):
    for table in ("vmdj_jam_members", "vmdj_jam_presence"):
        await runtime.d1_run("DELETE FROM " + table + " WHERE jam_id=? AND account_name=?", jam_id, name)


def _deck(data):
    return {
        "videoId": _video(data.get("videoId")),
        "title": _clean(data.get("title"), 300),
        "author": _clean(data.get("author"), 200),
        "playing": data.get("playing") is True,
        "position": round(_number(data.get("position"), 0.0, 0.0, MAX_POSITION), 3),
        "rate": round(_number(data.get("rate"), 1.0, 1 - PITCH_RANGE, 1 + PITCH_RANGE), 4),
    }


async def _project(runtime, row, actor):
    jam_id = str(row.get("jam_id") or "")
    data = await _opened(runtime, row.get("data"))
    members = await _members(runtime, jam_id)
    status = next((str(item.get("status")) for item in members
                   if str(item.get("account_name")) == actor), "")
    now = runtime.now()
    active = status in ACTIVE
    return {
        "id": jam_id,
        "title": _clean(data.get("title"), 80),
        "owner": str(row.get("owner_name") or ""),
        "me": actor,
        "myStatus": status,
        "now": now,
        "members": [{"name": str(item.get("account_name") or ""),
                     "status": str(item.get("status") or "")} for item in members],
        "queue": await _queue(runtime, jam_id) if status in ACTIVE else [],
        "decks": await _decks(runtime, jam_id) if status in ACTIVE else {},
        "presence": await _presence(runtime, jam_id, now) if active else [],
        "reactions": await _reactions(runtime, jam_id, now) if active else [],
    }


async def _jam_response(runtime, jam_id, account_bi, actor, extra=None):
    row = await _jam_row(runtime, jam_id, account_bi, actor)
    if not row:
        return _response(runtime, dict(extra or {}, ok=True, jam=None))
    return _response(runtime, dict(extra or {}, ok=True, jam=await _project(runtime, row, actor)))


async def _list(runtime, account_bi, actor):
    rows = await runtime.d1_all(
        "SELECT j.jam_id,j.owner_name,j.updated_at,j.data,m.status,"
        "(SELECT COUNT(*) FROM vmdj_jam_members c WHERE c.jam_id=j.jam_id "
        "AND c.status IN ('owner','joined')) AS listeners "
        "FROM vmdj_jams j JOIN vmdj_jam_members m ON m.jam_id=j.jam_id "
        "WHERE m.account_name=? AND m.status IN ('owner','joined','invited') "
        "AND (m.account_bi='' OR m.account_bi=?) ORDER BY j.updated_at DESC LIMIT ?",
        actor, account_bi, MAX_LISTED_JAMS,
    ) or []
    jams = []
    for row in rows:
        data = await _opened(runtime, row.get("data"))
        jams.append({
            "id": str(row.get("jam_id") or ""),
            "title": _clean(data.get("title"), 80),
            "owner": str(row.get("owner_name") or ""),
            "myStatus": str(row.get("status") or ""),
            "members": int(row.get("listeners") or 0),
        })
    return _response(runtime, {"ok": True, "jams": jams})


async def _touch(runtime, jam_id):
    await runtime.d1_run("UPDATE vmdj_jams SET updated_at=? WHERE jam_id=?", runtime.now(), jam_id)


async def _notify(runtime, recipient, actor, jam_id, title):
    notify = getattr(runtime, "notify", None)
    if notify is None:
        return
    await notify(
        recipient, "plugin_activity", "%s invited you to a DJ jam" % actor,
        body=("Open Video Mix DJ Studio to join %s." % title) if title else
        "Open Video Mix DJ Studio to join and share the decks.",
        href="/dashboard/video-mix-dj-studio?jam=" + jam_id,
        dedupe="vmdj:invite:" + jam_id + ":" + recipient,
        meta={"plugin": PLUGIN_ID, "jamId": jam_id},
    )


async def _invite(runtime, jam_id, actor, names, title):
    existing = {str(item.get("account_name")): item for item in await _members(runtime, jam_id)}
    invited, skipped = [], []
    for raw in names if isinstance(names, list) else []:
        name = _name(raw)
        if not name:
            if _clean(raw, 64):
                skipped.append(_clean(raw, 64))
            continue
        if name == actor or name in invited or name in skipped or name in existing:
            continue
        check = getattr(runtime, "account_exists", None)
        if len(existing) >= MAX_MEMBERS or check is None or not await check(name):
            skipped.append(name)
            continue
        await runtime.d1_run(
            "INSERT INTO vmdj_jam_members(jam_id,account_name,account_bi,status,invited_by,joined_at) "
            "VALUES(?,?,'','invited',?,?) ON CONFLICT(jam_id,account_name) DO NOTHING",
            jam_id, name, actor, runtime.now(),
        )
        existing[name] = {"account_name": name, "status": "invited"}
        invited.append(name)
        await _notify(runtime, name, actor, jam_id, title)
    return invited, skipped


async def _create(runtime, account_bi, actor, data):
    owned = await runtime.d1_first(
        "SELECT COUNT(*) AS total FROM vmdj_jams WHERE owner_bi=?", account_bi)
    if int((owned or {}).get("total") or 0) >= MAX_JAMS_PER_ACCOUNT:
        return _error(runtime, "too_many_jams", 409)
    jam_id = runtime.new_id()
    title = _clean(data.get("title"), 80) or ("%s's jam" % actor)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO vmdj_jams(jam_id,owner_bi,owner_name,created_at,updated_at,data) "
        "VALUES(?,?,?,?,?,?)",
        jam_id, account_bi, actor, now, now, await runtime.seal({"title": title}),
    )
    await runtime.d1_run(
        "INSERT INTO vmdj_jam_members(jam_id,account_name,account_bi,status,invited_by,joined_at) "
        "VALUES(?,?,?,'owner',?,?)",
        jam_id, actor, account_bi, actor, now,
    )
    for side, deck in (data.get("decks") or {}).items() if isinstance(data.get("decks"), dict) else ():
        if side in SIDES and isinstance(deck, dict):
            await _save_deck(runtime, jam_id, side, actor, deck)
    invited, skipped = await _invite(runtime, jam_id, actor, data.get("members"), title)
    await runtime.audit(actor, "video_mix_dj.jam_create", "jam", jam_id)
    return await _jam_response(runtime, jam_id, account_bi, actor,
                               {"invited": invited, "skipped": skipped})


async def _save_deck(runtime, jam_id, side, actor, data):
    await runtime.d1_run(
        "INSERT INTO vmdj_jam_decks(jam_id,side,updated_at,updated_by,data) VALUES(?,?,?,?,?) "
        "ON CONFLICT(jam_id,side) DO UPDATE SET updated_at=excluded.updated_at,"
        "updated_by=excluded.updated_by,data=excluded.data",
        jam_id, side, runtime.now(), actor, await runtime.seal(_deck(data)),
    )


async def _delete_jam(runtime, jam_id):
    for table in ("vmdj_jam_reactions", "vmdj_jam_presence", "vmdj_jam_decks", "vmdj_jam_queue",
                  "vmdj_jam_members", "vmdj_jams"):
        await runtime.d1_run("DELETE FROM " + table + " WHERE jam_id=?", jam_id)


async def _add_to_queue(runtime, jam_id, actor, data):
    video = _video(data.get("videoId"))
    if not video:
        return _error(runtime, "invalid_video")
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS total FROM vmdj_jam_queue WHERE jam_id=?", jam_id)
    if int((count or {}).get("total") or 0) >= MAX_QUEUE:
        return _error(runtime, "queue_full", 409)
    await runtime.d1_run(
        "INSERT INTO vmdj_jam_queue(item_id,jam_id,added_by,added_at,data) VALUES(?,?,?,?,?)",
        runtime.new_id(), jam_id, actor, runtime.now(), await runtime.seal({
            "videoId": video,
            "title": _clean(data.get("title"), 300),
            "author": _clean(data.get("author"), 200),
        }),
    )
    return None


def _allowed_methods(parts):
    if not parts:
        return ("GET",)
    if parts[0] != "jams":
        return ()
    if len(parts) == 1:
        return ("GET", "POST")
    if len(parts) == 2:
        return ("GET", "DELETE")
    if len(parts) == 3 and parts[2] in ("join", "leave"):
        return ("POST",)
    if len(parts) == 3 and parts[2] in ("members", "queue", "presence", "reactions"):
        return ("POST",)
    if len(parts) == 4 and parts[2] in ("members", "queue"):
        return ("DELETE",)
    if len(parts) == 4 and parts[2] == "decks":
        return ("PUT",)
    return ()


async def handle(runtime, path):
    parts = _route(path)
    if parts is None:
        return _error(runtime, "not_found", 404)
    method = runtime.method()
    allowed = _allowed_methods(parts)
    if not allowed:
        return _error(runtime, "not_found", 404)
    if method not in allowed:
        return _response(runtime, {"ok": False, "error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _error(runtime, "origin_not_allowed", 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _error(runtime, error, 413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _error(runtime, "invalid_session", 401)
    await _ensure_tables(runtime)
    if not parts:
        return _response(runtime, {"ok": True, "actor": actor})
    if len(parts) == 1:
        if method == "POST":
            return await _create(runtime, account_bi, actor, data)
        return await _list(runtime, account_bi, actor)
    jam_id = parts[1]
    row = await _jam_row(runtime, jam_id, account_bi, actor) if ID_RE.fullmatch(jam_id) else None
    if not row:
        return _error(runtime, "jam_not_found", 404)
    status = str(row.get("status") or "")
    owner = status == "owner"
    action = parts[2] if len(parts) > 2 else ""
    if len(parts) == 2 and method == "GET":
        return _response(runtime, {"ok": True, "jam": await _project(runtime, row, actor)})
    if len(parts) == 2:
        if owner:
            await _delete_jam(runtime, jam_id)
            await runtime.audit(actor, "video_mix_dj.jam_end", "jam", jam_id)
            return _response(runtime, {"ok": True, "ended": True})
        await _forget_member(runtime, jam_id, actor)
        return _response(runtime, {"ok": True, "left": True})
    if action == "join":
        if status == "invited":
            await runtime.d1_run(
                "UPDATE vmdj_jam_members SET status='joined',account_bi=?,joined_at=? "
                "WHERE jam_id=? AND account_name=?",
                account_bi, runtime.now(), jam_id, actor)
            await _touch(runtime, jam_id)
        return await _jam_response(runtime, jam_id, account_bi, actor)
    if action == "leave":
        if owner:
            return _error(runtime, "owner_cannot_leave", 409)
        await _forget_member(runtime, jam_id, actor)
        return _response(runtime, {"ok": True, "left": True})
    if status not in ACTIVE:
        return _error(runtime, "jam_not_joined", 403)
    if action == "members" and method == "POST":
        title = _clean((await _opened(runtime, row.get("data"))).get("title"), 80)
        invited, skipped = await _invite(runtime, jam_id, actor, data.get("names"), title)
        if invited:
            await _touch(runtime, jam_id)
        return await _jam_response(runtime, jam_id, account_bi, actor,
                                   {"invited": invited, "skipped": skipped})
    if action == "members":
        name = _name(parts[3])
        if not owner and name != actor:
            return _error(runtime, "jam_forbidden", 403)
        if name == str(row.get("owner_name") or ""):
            return _error(runtime, "owner_cannot_leave", 409)
        await _forget_member(runtime, jam_id, name)
        await _touch(runtime, jam_id)
        return await _jam_response(runtime, jam_id, account_bi, actor)
    if action == "presence":
        await _save_presence(runtime, jam_id, actor, data)
        return await _jam_response(runtime, jam_id, account_bi, actor)
    if action == "reactions":
        problem = await _react(runtime, jam_id, actor, data)
        if problem is not None:
            return problem
        return await _jam_response(runtime, jam_id, account_bi, actor)
    if action == "queue" and method == "POST":
        problem = await _add_to_queue(runtime, jam_id, actor, data)
        if problem is not None:
            return problem
        await _touch(runtime, jam_id)
        return await _jam_response(runtime, jam_id, account_bi, actor)
    if action == "queue":
        item = await runtime.d1_first(
            "SELECT added_by FROM vmdj_jam_queue WHERE jam_id=? AND item_id=?", jam_id, parts[3])
        if item:
            await runtime.d1_run(
                "DELETE FROM vmdj_jam_queue WHERE jam_id=? AND item_id=?", jam_id, parts[3])
            await _touch(runtime, jam_id)
        return await _jam_response(runtime, jam_id, account_bi, actor)
    side = parts[3]
    if side not in SIDES:
        return _error(runtime, "invalid_deck")
    await _save_deck(runtime, jam_id, side, actor, data)
    await _touch(runtime, jam_id)
    return await _jam_response(runtime, jam_id, account_bi, actor)


async def account_has_data(runtime, account_bi):
    await _ensure_tables(runtime)
    row = await runtime.d1_first(
        "SELECT 1 AS found FROM vmdj_jam_members WHERE account_bi=? LIMIT 1", account_bi)
    return bool(row)


async def purge_account_data(runtime, account_bi, account_name=""):
    await _ensure_tables(runtime)
    rows = await runtime.d1_all(
        "SELECT jam_id FROM vmdj_jams WHERE owner_bi=?", account_bi) or []
    for row in rows:
        await _delete_jam(runtime, str(row.get("jam_id") or ""))
    joined = await runtime.d1_all(
        "SELECT jam_id,account_name FROM vmdj_jam_members WHERE account_bi=?", account_bi) or []
    for row in joined:
        for table in ("vmdj_jam_presence", "vmdj_jam_reactions"):
            await runtime.d1_run("DELETE FROM " + table + " WHERE jam_id=? AND account_name=?",
                                 str(row.get("jam_id") or ""), str(row.get("account_name") or ""))
    await runtime.d1_run("DELETE FROM vmdj_jam_members WHERE account_bi=?", account_bi)
    name = _name(account_name)
    if name:
        await runtime.d1_run(
            "DELETE FROM vmdj_jam_members WHERE account_name=? AND status='invited'", name)
