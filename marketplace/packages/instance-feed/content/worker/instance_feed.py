import json
import re


PREFIX = "/api/instance-feed"
PLUGIN_ID = "instance-feed"
REQUEST_PROOF = "forkmesh-instance-feed-request-v1"
MAX_BODY_BYTES = 16 * 1024
MAX_TEXT = 500
MAX_SPOILER = 100
PAGE_SIZE = 20
MAX_PAGE_SIZE = 40
MAX_THREAD = 200
MAX_EXPORT = 20000
MAX_MENTIONS = 5
BURST_WINDOW_MS = 10 * 60 * 1000
BURST_LIMIT = 30
ID_RE = re.compile(r"^[0-9a-f]{32}$")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
CASE_RE = re.compile(r"^[A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")
CURSOR_RE = re.compile(r"^(\d{1,15})-([0-9a-f]{32})$")
MENTION_RE = re.compile(r"(?<![A-Za-z0-9_@/])@([A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)\b")

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS instance_feed_posts (
        post_id TEXT PRIMARY KEY,
        author_bi TEXT NOT NULL,
        author_name TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        reply_to TEXT NOT NULL DEFAULT '',
        boost_of TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_instance_feed_posts_time "
    "ON instance_feed_posts(created_at DESC,post_id DESC)",
    "CREATE INDEX IF NOT EXISTS idx_instance_feed_posts_author "
    "ON instance_feed_posts(author_bi,created_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_instance_feed_posts_reply "
    "ON instance_feed_posts(reply_to,created_at)",
    "CREATE INDEX IF NOT EXISTS idx_instance_feed_posts_boost "
    "ON instance_feed_posts(boost_of,author_bi)",
    """CREATE TABLE IF NOT EXISTS instance_feed_favourites (
        post_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        PRIMARY KEY (post_id,account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_instance_feed_favourites_account "
    "ON instance_feed_favourites(account_bi)",
]

DATA_SCHEMA_DOWN = [
    "DROP INDEX IF EXISTS idx_instance_feed_favourites_account",
    "DROP TABLE IF EXISTS instance_feed_favourites",
    "DROP INDEX IF EXISTS idx_instance_feed_posts_boost",
    "DROP INDEX IF EXISTS idx_instance_feed_posts_reply",
    "DROP INDEX IF EXISTS idx_instance_feed_posts_author",
    "DROP INDEX IF EXISTS idx_instance_feed_posts_time",
    "DROP TABLE IF EXISTS instance_feed_posts",
]

POST_COLUMNS = "post_id,author_bi,author_name,created_at,reply_to,boost_of,data"
VIEWER_FIELDS = ("boosted", "favourited", "mine", "canDelete")

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


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def _text(value, limit):
    text = str(value or "").replace("\x00", "").replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    return text[:limit]


def _integer(value, default=0):
    if isinstance(value, bool):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


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


def _allowed_methods(parts):
    if not parts:
        return ("GET",)
    head = parts[0]
    if head == "export" and len(parts) == 1:
        return ("GET",)
    if head == "public" and len(parts) == 2:
        return ("GET",)
    if head != "posts":
        return ()
    if len(parts) == 1:
        return ("POST",)
    if len(parts) == 2:
        return ("GET", "DELETE")
    if len(parts) == 3 and parts[2] in ("favourite", "boost"):
        return ("POST", "DELETE")
    return ()


def _display(record, name):
    chosen = str((record or {}).get("nameCase") or "").strip()
    return chosen if CASE_RE.fullmatch(chosen) and chosen.lower() == name else name


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    name = _clean((record or {}).get("name"), 64).lower()
    return str(account_bi or ""), name, _display(record, name)


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


async def _installed(runtime, account_bi):
    check = getattr(runtime, "account_plugin_installed", None)
    return bool(await check(account_bi)) if callable(check) else False


async def _is_admin(runtime, actor):
    check = getattr(runtime, "is_admin", None)
    if not callable(check):
        return False
    try:
        return bool(await check(actor))
    except Exception:
        return False


async def _open(runtime, row):
    opened = await runtime.open(row.get("data")) if row.get("data") else None
    return opened if isinstance(opened, dict) else {}


def _ids_json(ids):
    return json.dumps(sorted({str(value) for value in ids if value}))


async def _post_rows(runtime, ids):
    if not ids:
        return {}
    rows = await runtime.d1_all(
        "SELECT " + POST_COLUMNS + " FROM instance_feed_posts "
        "WHERE post_id IN (SELECT value FROM json_each(?))",
        _ids_json(ids),
    )
    return {str(row["post_id"]): row for row in rows or []}


async def _counts(runtime, sql, ids):
    if not ids:
        return {}
    rows = await runtime.d1_all(sql, _ids_json(ids))
    return {str(row["post_id"]): _integer(row.get("total")) for row in rows or []}


async def _mine(runtime, sql, ids, account_bi):
    if not ids:
        return set()
    rows = await runtime.d1_all(sql, _ids_json(ids), account_bi)
    return {str(row["post_id"]) for row in rows or []}


async def _project_many(runtime, rows, account_bi, admin=False):
    rows = list(rows or [])
    originals = await _post_rows(
        runtime, [row["boost_of"] for row in rows if row.get("boost_of")])
    statuses = [row for row in rows if not row.get("boost_of")] + list(originals.values())
    ids = [str(row["post_id"]) for row in statuses]
    parents = await _post_rows(
        runtime, [row["reply_to"] for row in statuses if row.get("reply_to")])
    replies = await _counts(
        runtime,
        "SELECT reply_to AS post_id,COUNT(*) AS total FROM instance_feed_posts "
        "WHERE reply_to IN (SELECT value FROM json_each(?)) GROUP BY reply_to",
        ids,
    )
    boosts = await _counts(
        runtime,
        "SELECT boost_of AS post_id,COUNT(*) AS total FROM instance_feed_posts "
        "WHERE boost_of IN (SELECT value FROM json_each(?)) GROUP BY boost_of",
        ids,
    )
    favourites = await _counts(
        runtime,
        "SELECT post_id,COUNT(*) AS total FROM instance_feed_favourites "
        "WHERE post_id IN (SELECT value FROM json_each(?)) GROUP BY post_id",
        ids,
    )
    my_boosts = await _mine(
        runtime,
        "SELECT boost_of AS post_id FROM instance_feed_posts "
        "WHERE boost_of IN (SELECT value FROM json_each(?)) AND author_bi=?",
        ids,
        account_bi,
    )
    my_favourites = await _mine(
        runtime,
        "SELECT post_id FROM instance_feed_favourites "
        "WHERE post_id IN (SELECT value FROM json_each(?)) AND account_bi=?",
        ids,
        account_bi,
    )
    statuses_by_id = {}
    for row in statuses:
        post_id = str(row["post_id"])
        data = await _open(runtime, row)
        parent = parents.get(str(row.get("reply_to") or ""))
        statuses_by_id[post_id] = {
            "type": "post",
            "id": post_id,
            "author": str(row.get("author_name") or ""),
            "authorName": str(data.get("authorName") or row.get("author_name") or ""),
            "text": str(data.get("text") or ""),
            "spoiler": str(data.get("spoiler") or ""),
            "createdAt": _integer(row.get("created_at")),
            "replyTo": str(row.get("reply_to") or ""),
            "replyToAuthor": str((parent or {}).get("author_name") or ""),
            "replies": replies.get(post_id, 0),
            "boosts": boosts.get(post_id, 0),
            "favourites": favourites.get(post_id, 0),
            "boosted": post_id in my_boosts,
            "favourited": post_id in my_favourites,
            "mine": str(row.get("author_bi") or "") == account_bi,
            "canDelete": admin or str(row.get("author_bi") or "") == account_bi,
        }
    projected = []
    for row in rows:
        if not row.get("boost_of"):
            projected.append(statuses_by_id[str(row["post_id"])])
            continue
        original = statuses_by_id.get(str(row["boost_of"]))
        if original is None:
            continue
        data = await _open(runtime, row)
        projected.append({
            "type": "boost",
            "id": str(row["post_id"]),
            "booster": str(row.get("author_name") or ""),
            "boosterName": str(data.get("authorName") or row.get("author_name") or ""),
            "createdAt": _integer(row.get("created_at")),
            "post": original,
        })
    return projected


def _cursor(value):
    match = CURSOR_RE.fullmatch(_clean(value, 64))
    return (int(match.group(1)), match.group(2)) if match else None


async def _timeline(runtime, account_bi, actor, admin, params):
    limit = max(1, min(MAX_PAGE_SIZE, _integer(params.get("limit"), PAGE_SIZE)))
    where, args = [], []
    author = _clean(params.get("author"), 64).lower().lstrip("@")
    profile = None
    if author:
        if not NAME_RE.fullmatch(author):
            return _response(runtime, {"error": "account_not_found"}, 404)
        where.append("author_name=?")
        args.append(author)
        profile = await _profile(runtime, author)
    else:
        where.append("reply_to=''")
    cursor = _cursor(params.get("before"))
    if cursor:
        where.append("(created_at<? OR (created_at=? AND post_id<?))")
        args.extend([cursor[0], cursor[0], cursor[1]])
    rows = await runtime.d1_all(
        "SELECT " + POST_COLUMNS + " FROM instance_feed_posts WHERE "
        + " AND ".join(where)
        + " ORDER BY created_at DESC,post_id DESC LIMIT ?",
        *args,
        limit + 1,
    )
    rows = list(rows or [])
    more = len(rows) > limit
    rows = rows[:limit]
    payload = {
        "ok": True,
        "installed": True,
        "actor": actor,
        "admin": admin,
        "maxLength": MAX_TEXT,
        "posts": await _project_many(runtime, rows, account_bi, admin),
        "next": "%d-%s" % (_integer(rows[-1]["created_at"]), rows[-1]["post_id"])
        if more and rows else "",
    }
    if author:
        payload["author"] = author
        payload["profile"] = profile
    return _response(runtime, payload)


async def _profile(runtime, author):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS posts,MIN(created_at) AS first FROM instance_feed_posts "
        "WHERE author_name=? AND boost_of=''",
        author,
    )
    latest = await runtime.d1_first(
        "SELECT data FROM instance_feed_posts WHERE author_name=? "
        "ORDER BY created_at DESC LIMIT 1",
        author,
    )
    data = await _open(runtime, latest or {})
    return {
        "name": author,
        "displayName": str(data.get("authorName") or author),
        "posts": _integer((row or {}).get("posts")),
        "since": _integer((row or {}).get("first")),
    }


async def _row(runtime, post_id):
    return await runtime.d1_first(
        "SELECT " + POST_COLUMNS + " FROM instance_feed_posts WHERE post_id=?",
        post_id,
    )


async def _recent_count(runtime, account_bi, now):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS total FROM instance_feed_posts "
        "WHERE author_bi=? AND created_at>?",
        account_bi,
        now - BURST_WINDOW_MS,
    )
    return _integer((row or {}).get("total"))


async def _notify(runtime, recipient, actor, title, body, post_id, dedupe):
    if not recipient or recipient == actor or not callable(getattr(runtime, "notify", None)):
        return
    try:
        await runtime.notify(
            recipient,
            "plugin_activity",
            title,
            body=body,
            href="/dashboard/instance-feed?post=" + post_id,
            dedupe=dedupe,
        )
    except Exception:
        return


async def _mentioned(runtime, text, actor):
    names = []
    for match in MENTION_RE.finditer(text):
        name = match.group(1).lower()
        if name != actor and name not in names:
            names.append(name)
        if len(names) >= MAX_MENTIONS:
            break
    found = []
    lookup = getattr(runtime, "account", None)
    for name in names:
        if not callable(lookup):
            break
        account_bi, clean = await lookup(name)
        if account_bi and clean and await _installed(runtime, account_bi):
            found.append(clean)
    return found


def _excerpt(text):
    text = _clean(text, 140)
    return text if len(text) < 140 else text[:137] + "…"


async def _create(runtime, account_bi, actor, display, data):
    text = _text(data.get("text"), MAX_TEXT + 1)
    if not text:
        return _response(runtime, {"error": "text_required"}, 400)
    if len(text) > MAX_TEXT:
        return _response(runtime, {"error": "text_too_long", "maxLength": MAX_TEXT}, 400)
    spoiler = _clean(data.get("spoiler"), MAX_SPOILER)
    reply_to = _clean(data.get("replyTo"), 32)
    parent = None
    if reply_to:
        parent = await _row(runtime, reply_to) if ID_RE.fullmatch(reply_to) else None
        if not parent or parent.get("boost_of"):
            return _response(runtime, {"error": "post_not_found"}, 404)
    now = runtime.now()
    if await _recent_count(runtime, account_bi, now) >= BURST_LIMIT:
        return _response(runtime, {"error": "posting_too_fast"}, 429)
    post_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO instance_feed_posts(post_id,author_bi,author_name,created_at,"
        "reply_to,boost_of,data) VALUES(?,?,?,?,?,?,?)",
        post_id,
        account_bi,
        actor,
        now,
        reply_to,
        "",
        await runtime.seal({"text": text, "spoiler": spoiler, "authorName": display}),
    )
    await runtime.audit(actor, "instance-feed.post", "instance_feed_post", post_id)
    if parent:
        await _notify(
            runtime, str(parent.get("author_name") or ""), actor,
            display + " replied to your post", _excerpt(text), post_id,
            "instance-feed-reply:" + post_id)
    for name in await _mentioned(runtime, text, actor):
        if parent and name == parent.get("author_name"):
            continue
        await _notify(
            runtime, name, actor, display + " mentioned you", _excerpt(text), post_id,
            "instance-feed-mention:" + post_id + ":" + name)
    projected = await _project_many(runtime, [await _row(runtime, post_id)], account_bi)
    return _response(runtime, {"ok": True, "post": projected[0]}, 201)


async def _thread(runtime, row, account_bi, admin):
    ancestors = []
    seen = {str(row["post_id"])}
    parent_id = str(row.get("reply_to") or "")
    while parent_id and parent_id not in seen and len(ancestors) < MAX_THREAD:
        parent = await _row(runtime, parent_id)
        if not parent:
            break
        seen.add(parent_id)
        ancestors.insert(0, parent)
        parent_id = str(parent.get("reply_to") or "")
    descendants, frontier = [], [str(row["post_id"])]
    while frontier and len(descendants) < MAX_THREAD:
        children = await runtime.d1_all(
            "SELECT " + POST_COLUMNS + " FROM instance_feed_posts "
            "WHERE reply_to IN (SELECT value FROM json_each(?)) "
            "ORDER BY created_at,post_id LIMIT ?",
            _ids_json(frontier),
            MAX_THREAD - len(descendants),
        )
        frontier = []
        for child in children or []:
            child_id = str(child["post_id"])
            if child_id in seen:
                continue
            seen.add(child_id)
            descendants.append(child)
            frontier.append(child_id)
    descendants = _thread_order(str(row["post_id"]), descendants)
    projected = await _project_many(runtime, ancestors + [row] + descendants, account_bi, admin)
    by_id = {item["id"]: item for item in projected}
    return {
        "ok": True,
        "post": by_id.get(str(row["post_id"])),
        "ancestors": [by_id[str(item["post_id"])] for item in ancestors
                      if str(item["post_id"]) in by_id],
        "replies": [by_id[str(item["post_id"])] for item in descendants
                    if str(item["post_id"]) in by_id],
    }


def _thread_order(root_id, rows):
    children = {}
    for row in rows:
        children.setdefault(str(row.get("reply_to") or ""), []).append(row)
    ordered, stack = [], list(reversed(children.get(root_id, [])))
    while stack:
        row = stack.pop()
        ordered.append(row)
        stack.extend(reversed(children.get(str(row["post_id"]), [])))
    return ordered


def _public_entry(entry):
    return {key: value for key, value in entry.items() if key not in VIEWER_FIELDS}


async def _public_post(runtime, post_id):
    await _ensure_tables(runtime)
    row = await _row(runtime, post_id) if ID_RE.fullmatch(post_id) else None
    if row and row.get("boost_of"):
        row = await _row(runtime, str(row["boost_of"]))
    if not row:
        return _response(runtime, {"error": "post_not_found"}, 404)
    thread = await _thread(runtime, row, "", False)
    return _response(runtime, {
        "ok": True,
        "post": _public_entry(thread["post"]),
        "ancestors": [_public_entry(item) for item in thread["ancestors"]],
        "replies": [_public_entry(item) for item in thread["replies"]],
    })


async def _delete_post(runtime, row, actor):
    post_id = str(row["post_id"])
    await runtime.d1_run(
        "DELETE FROM instance_feed_favourites WHERE post_id=?", post_id)
    await runtime.d1_run(
        "DELETE FROM instance_feed_posts WHERE boost_of=?", post_id)
    await runtime.d1_run(
        "DELETE FROM instance_feed_posts WHERE post_id=?", post_id)
    await runtime.audit(actor, "instance-feed.delete", "instance_feed_post", post_id)


async def _favourite(runtime, row, account_bi, actor, display, add):
    post_id = str(row["post_id"])
    if add:
        existing = await runtime.d1_first(
            "SELECT 1 AS one FROM instance_feed_favourites WHERE post_id=? AND account_bi=?",
            post_id, account_bi)
        if not existing:
            await runtime.d1_run(
                "INSERT INTO instance_feed_favourites(post_id,account_bi,created_at) "
                "VALUES(?,?,?) ON CONFLICT(post_id,account_bi) DO NOTHING",
                post_id, account_bi, runtime.now())
            data = await _open(runtime, row)
            await _notify(
                runtime, str(row.get("author_name") or ""), actor,
                display + " favourited your post", _excerpt(data.get("text")), post_id,
                "instance-feed-favourite:" + post_id + ":" + actor)
    else:
        await runtime.d1_run(
            "DELETE FROM instance_feed_favourites WHERE post_id=? AND account_bi=?",
            post_id, account_bi)
    projected = await _project_many(runtime, [row], account_bi)
    return _response(runtime, {"ok": True, "post": projected[0]})


async def _boost(runtime, row, account_bi, actor, display, add):
    post_id = str(row["post_id"])
    existing = await runtime.d1_first(
        "SELECT post_id FROM instance_feed_posts WHERE boost_of=? AND author_bi=?",
        post_id, account_bi)
    if add and not existing:
        boost_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO instance_feed_posts(post_id,author_bi,author_name,created_at,"
            "reply_to,boost_of,data) VALUES(?,?,?,?,?,?,?)",
            boost_id, account_bi, actor, runtime.now(), "", post_id,
            await runtime.seal({"authorName": display}))
        await runtime.audit(actor, "instance-feed.boost", "instance_feed_post", post_id)
        data = await _open(runtime, row)
        await _notify(
            runtime, str(row.get("author_name") or ""), actor,
            display + " boosted your post", _excerpt(data.get("text")), post_id,
            "instance-feed-boost:" + post_id + ":" + actor)
    elif not add and existing:
        await runtime.d1_run(
            "DELETE FROM instance_feed_posts WHERE boost_of=? AND author_bi=?",
            post_id, account_bi)
    projected = await _project_many(runtime, [row], account_bi)
    return _response(runtime, {"ok": True, "post": projected[0]})


async def account_has_data(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT 1 AS one FROM instance_feed_posts WHERE author_bi=? LIMIT 1", account_bi)
    if row:
        return True
    row = await runtime.d1_first(
        "SELECT 1 AS one FROM instance_feed_favourites WHERE account_bi=? LIMIT 1", account_bi)
    return bool(row)


async def export_account_data(runtime, account_bi, actor):
    rows = await runtime.d1_all(
        "SELECT " + POST_COLUMNS + " FROM instance_feed_posts WHERE author_bi=? "
        "ORDER BY created_at,post_id LIMIT ?",
        account_bi,
        MAX_EXPORT,
    )
    posts, boosts = [], []
    for row in rows or []:
        if row.get("boost_of"):
            boosts.append({"postId": str(row["boost_of"]),
                           "boostedAt": _integer(row.get("created_at"))})
            continue
        data = await _open(runtime, row)
        posts.append({
            "id": str(row["post_id"]),
            "createdAt": _integer(row.get("created_at")),
            "replyTo": str(row.get("reply_to") or ""),
            "spoiler": str(data.get("spoiler") or ""),
            "text": str(data.get("text") or ""),
        })
    favourites = await runtime.d1_all(
        "SELECT post_id,created_at FROM instance_feed_favourites WHERE account_bi=? "
        "ORDER BY created_at LIMIT ?",
        account_bi,
        MAX_EXPORT,
    )
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-instance-feed-export-v1",
        "account": actor,
        "exportedAt": runtime.now(),
        "posts": posts,
        "boosts": boosts,
        "favourites": [{"postId": str(row["post_id"]),
                        "favouritedAt": _integer(row.get("created_at"))}
                       for row in favourites or []],
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    await runtime.d1_run(
        "DELETE FROM instance_feed_favourites WHERE account_bi=? OR post_id IN "
        "(SELECT post_id FROM instance_feed_posts WHERE author_bi=?)",
        account_bi, account_bi)
    await runtime.d1_run(
        "DELETE FROM instance_feed_posts WHERE boost_of IN "
        "(SELECT post_id FROM instance_feed_posts WHERE author_bi=?)",
        account_bi)
    await runtime.d1_run(
        "DELETE FROM instance_feed_posts WHERE author_bi=?", account_bi)


async def handle(runtime, path):
    parts = _route(path)
    if parts is None:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    allowed = _allowed_methods(parts)
    if not allowed:
        return _response(runtime, {"error": "not_found"}, 404)
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(runtime, {"error": error},
                             413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    if parts[0:1] == ("public",):
        return await _public_post(runtime, parts[1])
    account_bi, actor, display = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    await _ensure_tables(runtime)
    if not await _installed(runtime, account_bi):
        if not parts:
            return _response(runtime, {"ok": True, "installed": False, "actor": actor})
        return _response(runtime, {"error": "plugin_not_installed", "canInstall": True}, 409)
    params = runtime.query_params() if hasattr(runtime, "query_params") else {}
    admin = await _is_admin(runtime, actor)
    if not parts:
        return await _timeline(runtime, account_bi, actor, admin, params)
    if parts[0] == "export":
        return _response(runtime, {
            "ok": True,
            "filename": "forkmesh-instance-feed-" + actor + ".json",
            "export": await export_account_data(runtime, account_bi, actor),
        })
    if len(parts) == 1:
        return await _create(runtime, account_bi, actor, display, data)
    post_id = parts[1]
    row = await _row(runtime, post_id) if ID_RE.fullmatch(post_id) else None
    if not row:
        return _response(runtime, {"error": "post_not_found"}, 404)
    if len(parts) == 3:
        if row.get("boost_of"):
            return _response(runtime, {"error": "post_not_found"}, 404)
        if parts[2] == "favourite":
            return await _favourite(runtime, row, account_bi, actor, display, method == "POST")
        return await _boost(runtime, row, account_bi, actor, display, method == "POST")
    if method == "GET":
        if row.get("boost_of"):
            row = await _row(runtime, str(row["boost_of"]))
            if not row:
                return _response(runtime, {"error": "post_not_found"}, 404)
        return _response(runtime, await _thread(runtime, row, account_bi, admin))
    if str(row.get("author_bi") or "") != account_bi and not admin:
        return _response(runtime, {"error": "forbidden"}, 403)
    await _delete_post(runtime, row, actor)
    return _response(runtime, {"ok": True, "deleted": True})
