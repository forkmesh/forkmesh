"""Encrypted collaborative notes shared by the web, World, and desktop apps.

The adapter supplied by ``entry.py`` owns runtime-specific concerns (D1,
account sessions, encryption and Durable Objects).  This module keeps the API
contract and authorization rules independently testable.
"""

import json
import re
import unicodedata

from organization_rbac import ORG_INSIDER_ROLE_SQL


PREFIX = "/api/notes"
MAX_BODY_BYTES = 1024 * 1024
MAX_TITLE = 200
MAX_MARKDOWN = 512 * 1024
MAX_NOTES = 500
MAX_VERSIONS = 100
BIND_CHUNK = 90
NOTE_ID_RE = re.compile(r"^[0-9a-f]{32}$")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
LINK_KINDS = frozenset({"issue", "pull", "discussion"})
ROLES = frozenset({"viewer", "editor"})
MAX_SUGGESTIONS = 8
MAX_QUERY = 63
MAX_SEARCH = 200
MAX_EVENTS = 500
MAX_ACTIVITY = 200
VIEW_WINDOW_MS = 30 * 60 * 1000
CLIENT_EVENTS = frozenset({"view", "copy-link", "export", "task", "issue"})
MAX_COMMENTS = 1000
MAX_COMMENT_BODY = 4000
MAX_QUOTE = 1000
MAX_CONTEXT = 32
MAX_OFFSET = 1000000
MAX_PEOPLE = 200
MENTION_RE = re.compile(r"(?<![\w@])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)", re.IGNORECASE)
CLIENT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
PRESENCE_STATES = frozenset({"viewing", "editing", "left"})
PAGES = PREFIX + "/pages"
PAGE_ACCENTS = ("blue", "purple", "pink", "red", "orange", "yellow", "teal", "gray")
PAGE_LAYOUTS = ("outline", "continuous")
PAGE_KINDS = frozenset({"note", "section", "link"})
MAX_PAGE_ENTRIES = 300
MAX_PAGE_DESCRIPTION = 2000
MAX_PAGE_CONTENT = 4 * 1024 * 1024
MAX_PAGES = 200
ENTRY_ID_RE = re.compile(r"^[A-Za-z0-9_-]{8,32}$")


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload, status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _title(value):
    return " ".join(str(value or "").replace("<", " ").replace(">", " ").split())[:MAX_TITLE]


def _emoji(value):
    text = str(value if value is not None else "").strip()
    if not text:
        return ""
    if len(text) > 16 or len(text.encode("utf-8")) > 64:
        return None
    keycap = "\u20e3" in text
    for char in text:
        category = unicodedata.category(char)
        if char.isspace() or category in ("Cc", "Zs", "Zl", "Zp"):
            return None
        if char.isascii() and char.isalpha():
            return None
        if char.isascii() and (char.isdigit() or char in "#*") and not keycap:
            return None
        if char.isascii() and not (char.isdigit() or char in "#*"):
            return None
        if not char.isascii() and category[0] not in ("S", "M") and category != "Cf":
            return None
    return text


def _markdown(value):
    value = str(value or "").replace("\x00", "")
    return value[:MAX_MARKDOWN]


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return "collection", "", "", ""
    if clean == PAGES:
        return "pages", "", "", ""
    page = re.fullmatch(r"/api/notes/pages/([0-9a-f]{32})(?:/(shares))?", clean)
    if page:
        return "pages", page.group(1), page.group(2) or "", ""
    match = re.fullmatch(
        r"/api/notes/([0-9a-f]{32})(?:/(versions|shares|links|activity|ws|presence|comments)"
        r"(?:/([0-9a-f]{32}))?)?", clean)
    if not match or (match.group(3) and match.group(2) != "comments"):
        return None
    return "note", match.group(1), match.group(2) or "", match.group(3) or ""


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    name = str((record or {}).get("name") or "").strip().lower()
    return str(account_bi or ""), name


async def _row(runtime, note_id):
    return await runtime.d1_first(
        "SELECT note_id,owner_bi,visibility,version,created_at,updated_at,"
        "published_at,data FROM notes WHERE note_id=?", note_id)


async def _role(runtime, row, account_bi):
    if not row:
        return ""
    if account_bi and str(row.get("owner_bi") or "") == account_bi:
        return "owner"
    if account_bi:
        direct = await runtime.d1_first(
            "SELECT role FROM note_shares WHERE note_id=? "
            "AND principal_type='user' AND principal_bi=?",
            row["note_id"], account_bi)
        if direct:
            return str(direct.get("role") or "viewer")
        organization = await runtime.d1_first(
            "SELECT s.role AS role, m.role AS member_role FROM note_shares s "
            "INNER JOIN org_members m "
            "ON m.org_bi=s.principal_bi WHERE s.note_id=? "
            "AND s.principal_type='organization' AND m.member_bi=? "
            "AND m.role IN " + ORG_INSIDER_ROLE_SQL + " "
            "ORDER BY CASE WHEN s.role='editor' AND m.role<>'auditor' "
            "THEN 0 ELSE 1 END LIMIT 1",
            row["note_id"], account_bi)
        if organization:
            if str(organization.get("member_role") or "") == "auditor":
                return "viewer"
            return str(organization.get("role") or "viewer")
    if str(row.get("visibility") or "") == "public":
        return "public"
    return ""


async def _shares(runtime, note_id):
    rows = await runtime.d1_all(
        "SELECT principal_type,principal_name,role,created_at "
        "FROM note_shares WHERE note_id=? "
        "ORDER BY principal_type,principal_name COLLATE NOCASE", note_id)
    return [{
        "type": str(row.get("principal_type") or ""),
        "name": str(row.get("principal_name") or ""),
        "role": str(row.get("role") or "viewer"),
        "createdAt": int(row.get("created_at") or 0),
    } for row in rows or []]


def _counts(row, activity=None):
    row, activity = row or {}, activity or {}
    web_views = int(row.get("reads") or 0)
    web_readers = int(row.get("readers") or 0)
    member_views = int(activity.get("views") or 0)
    return {
        "views": web_views + member_views,
        "readers": web_readers + int(activity.get("readers") or 0),
        "lastViewedAt": max(int(row.get("last_at") or 0),
                            int(activity.get("last_view_at") or 0)),
        "webViews": web_views,
        "webReaders": web_readers,
        "webLastViewedAt": int(row.get("last_at") or 0),
        "actions": int(activity.get("actions") or 0),
    }


ACTIVITY_COUNTS = (
    "COALESCE(SUM(CASE WHEN kind='view' THEN hits ELSE 0 END),0) AS views,"
    "COUNT(DISTINCT CASE WHEN kind='view' THEN actor_bi END) AS readers,"
    "COALESCE(MAX(CASE WHEN kind='view' THEN last_at END),0) AS last_view_at,"
    "COALESCE(SUM(CASE WHEN kind<>'view' THEN 1 ELSE 0 END),0) AS actions "
    "FROM note_events")


async def _views(runtime, note_id, public=True):
    web = await runtime.d1_first(
        "SELECT COUNT(*) AS readers,COALESCE(SUM(hits),0) AS reads,"
        "COALESCE(MAX(last_at),0) AS last_at FROM note_views WHERE note_id=?",
        note_id) if public else None
    activity = await runtime.d1_first(
        "SELECT " + ACTIVITY_COUNTS + " WHERE note_id=?", note_id)
    return _counts(web, activity)


def _detail(value):
    try:
        data = json.loads(value or "{}")
    except (TypeError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


async def _log(runtime, note_id, account_bi, kind, detail=None):
    if not account_bi:
        return
    try:
        now = runtime.now()
        if kind == "view":
            bumped = await runtime.d1_run(
                "UPDATE note_events SET last_at=?,hits=hits+1 WHERE id=("
                "SELECT id FROM note_events WHERE note_id=? AND actor_bi=? "
                "AND kind='view' AND last_at>=? ORDER BY last_at DESC LIMIT 1)",
                now, note_id, account_bi, now - VIEW_WINDOW_MS)
            if runtime.changes(bumped) == 1:
                await runtime.broadcast(note_id, {"type": "note-activity"})
                return
        await runtime.d1_run(
            "INSERT INTO note_events(note_id,kind,actor_bi,created_at,last_at,"
            "hits,detail) VALUES(?,?,?,?,?,1,?)",
            note_id, kind, account_bi, now, now,
            json.dumps(detail or {}, separators=(",", ":"), sort_keys=True))
        await runtime.d1_run(
            "DELETE FROM note_events WHERE note_id=? AND id NOT IN (SELECT id "
            "FROM note_events WHERE note_id=? ORDER BY last_at DESC,id DESC "
            "LIMIT ?)", note_id, note_id, MAX_EVENTS)
        await runtime.broadcast(note_id, {"type": "note-activity"})
    except Exception:
        pass


async def _record_view(runtime, note_id):
    """Count one anonymous read of a published note.

    The key is whatever blinded reader identity the runtime derives; a runtime
    that cannot derive one (or a test harness that does not care) returns "",
    and the read simply goes uncounted rather than inflating the total.
    """
    try:
        viewer_key = str(await runtime.viewer(note_id) or "")[:64]
    except Exception:
        viewer_key = ""
    if not viewer_key:
        return
    now = runtime.now()
    try:
        await runtime.d1_run(
            "INSERT INTO note_views(note_id,viewer_key,first_at,last_at,hits) "
            "VALUES(?,?,?,?,1) ON CONFLICT(note_id,viewer_key) DO UPDATE SET "
            "last_at=excluded.last_at,hits=hits+1",
            note_id, viewer_key, now, now)
    except Exception:
        # A read is never worth failing the page it was counting.
        pass


async def _links(runtime, note_id):
    rows = await runtime.d1_all(
        "SELECT owner,repo,kind,number,created_at FROM note_links "
        "WHERE note_id=? ORDER BY created_at,owner,repo,kind,number", note_id)
    return [{
        "owner": str(row.get("owner") or ""),
        "repo": str(row.get("repo") or ""),
        "kind": str(row.get("kind") or ""),
        "number": int(row.get("number") or 0),
        "createdAt": int(row.get("created_at") or 0),
    } for row in rows or []]


async def _bulk(runtime, note_ids, sql, tail=""):
    """Run one keyed lookup over many notes in as few queries as possible.

    D1 caps bound parameters per statement, so the ids are chunked; every
    chunk still beats the per-note query it replaces on a 500-note listing.
    """
    rows = []
    for start in range(0, len(note_ids), BIND_CHUNK):
        chunk = note_ids[start:start + BIND_CHUNK]
        rows.extend(await runtime.d1_all(
            sql + " WHERE note_id IN (" + ",".join(["?"] * len(chunk)) + ")"
            + tail, *chunk) or [])
    return rows


async def _shares_by_note(runtime, note_ids):
    grouped = {}
    rows = await _bulk(
        runtime,
        note_ids,
        "SELECT note_id,principal_type,principal_name,role,created_at "
        "FROM note_shares",
        " ORDER BY principal_type,principal_name COLLATE NOCASE",
    )
    for row in rows:
        grouped.setdefault(str(row.get("note_id") or ""), []).append({
            "type": str(row.get("principal_type") or ""),
            "name": str(row.get("principal_name") or ""),
            "role": str(row.get("role") or "viewer"),
            "createdAt": int(row.get("created_at") or 0),
        })
    return grouped


async def _views_by_note(runtime, note_ids):
    rows = await _bulk(
        runtime,
        note_ids,
        "SELECT note_id,COUNT(*) AS readers,COALESCE(SUM(hits),0) AS reads,"
        "COALESCE(MAX(last_at),0) AS last_at FROM note_views",
        " GROUP BY note_id",
    )
    activity = await _bulk(
        runtime, note_ids, "SELECT note_id," + ACTIVITY_COUNTS, " GROUP BY note_id")
    web = {str(row.get("note_id") or ""): row for row in rows}
    members = {str(row.get("note_id") or ""): row for row in activity}
    return {note_id: _counts(web.get(note_id), members.get(note_id))
            for note_id in set(web) | set(members)}


async def _keyed(runtime, note_ids, sql, tail, *lead):
    rows = []
    for start in range(0, len(note_ids), BIND_CHUNK):
        chunk = note_ids[start:start + BIND_CHUNK]
        rows.extend(await runtime.d1_all(
            sql + " IN (" + ",".join(["?"] * len(chunk)) + ")" + tail,
            *lead, *chunk) or [])
    return rows


async def _freshness(runtime, rows, account_bi, actor):
    note_ids = [str(row.get("note_id") or "") for row in rows]
    if not note_ids or not account_bi:
        return {}
    seen = {str(item.get("note_id") or ""): int(item.get("seen") or 0) for item in await _keyed(
        runtime, note_ids,
        "SELECT note_id,MAX(last_at) AS seen FROM note_events WHERE kind='view' "
        "AND actor_bi=? AND note_id", " GROUP BY note_id", account_bi)}
    editors = {str(item.get("note_id") or ""): str(item.get("author_bi") or "") for item in await _keyed(
        runtime, note_ids,
        "SELECT v.note_id AS note_id,v.author_bi AS author_bi FROM note_versions v "
        "JOIN notes n ON n.note_id=v.note_id AND n.version=v.version WHERE v.note_id", "")}
    try:
        fresh = {str(item.get("note_id") or ""): int(item.get("total") or 0) for item in await _keyed(
            runtime, note_ids,
            "SELECT c.note_id AS note_id,COUNT(*) AS total FROM note_comments c "
            "LEFT JOIN (SELECT note_id,MAX(last_at) AS seen FROM note_events "
            "WHERE kind='view' AND actor_bi=? GROUP BY note_id) e ON e.note_id=c.note_id "
            "WHERE c.author_bi<>? AND c.created_at>COALESCE(e.seen,0) AND c.note_id",
            " GROUP BY c.note_id", account_bi, account_bi)}
        threads = {str(item.get("note_id") or ""): int(item.get("total") or 0) for item in await _keyed(
            runtime, note_ids,
            "SELECT note_id,COUNT(*) AS total FROM note_comments WHERE parent_id='' "
            "AND resolved_at=0 AND note_id", " GROUP BY note_id")}
    except Exception:
        fresh, threads = {}, {}
    names = {account_bi: actor} if actor else {}
    for editor in sorted(set(editors.values()) - set(names) - {""}):
        names[editor] = str(await runtime.account_name(editor) or "")
    result = {}
    for row in rows:
        note_id = str(row.get("note_id") or "")
        editor = editors.get(note_id, "")
        viewed = seen.get(note_id, 0)
        result[note_id] = {
            "lastEditedBy": names.get(editor, ""),
            "myLastViewedAt": viewed,
            "unreadComments": fresh.get(note_id, 0),
            "openComments": threads.get(note_id, 0),
            "unread": bool((editor and editor != account_bi
                            and int(row.get("updated_at") or 0) > viewed)
                           or fresh.get(note_id, 0)),
        }
    return result


async def _project(runtime, row, role, full=True, counts=None):
    data = await runtime.open(row.get("data"))
    data = data if isinstance(data, dict) else {}
    result = {
        "id": row["note_id"],
        "title": _title(data.get("title")) or "Untitled note",
        "emoji": _emoji(data.get("emoji")) or "",
        "visibility": str(row.get("visibility") or "private"),
        "version": int(row.get("version") or 1),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "publishedAt": int(row.get("published_at") or 0),
        "role": role,
    }
    # Read counts are a non-identifying aggregate over a document the reader
    # is already allowed to open, so every role — public included — sees them.
    if isinstance(counts, dict):
        result.update(counts)
    else:
        result.update(await _views(
            runtime, row["note_id"], result["visibility"] == "public"))
    if full:
        result["markdown"] = _markdown(data.get("markdown"))
        # Publishing exposes the authored document, not its ACL or possibly
        # private repository context. An authenticated collaborator receives
        # the management metadata; an anonymous public reader never does.
        if role != "public":
            result["shares"] = await _shares(runtime, row["note_id"])
            result["links"] = await _links(runtime, row["note_id"])
            result["webSocketUrl"] = PREFIX + "/" + row["note_id"] + "/ws"
    return result


async def _matches(runtime, row, query):
    data = await runtime.open(row.get("data"))
    data = data if isinstance(data, dict) else {}
    title = _title(data.get("title")) or "Untitled note"
    return query in title.lower() or query in _markdown(data.get("markdown")).lower()


async def _list(runtime, account_bi, query="", actor=""):
    rows = await runtime.d1_all(
        "SELECT DISTINCT n.note_id,n.owner_bi,n.visibility,n.version,"
        "n.created_at,n.updated_at,n.published_at,n.data FROM notes n "
        "LEFT JOIN note_shares s ON s.note_id=n.note_id "
        "LEFT JOIN org_members m ON s.principal_type='organization' "
        "AND m.org_bi=s.principal_bi AND m.member_bi=? "
        "AND m.role IN " + ORG_INSIDER_ROLE_SQL + " "
        "WHERE n.owner_bi=? OR (s.principal_type='user' AND s.principal_bi=?) "
        "OR m.member_bi=? ORDER BY n.updated_at DESC LIMIT ?",
        account_bi, account_bi, account_bi, account_bi, MAX_NOTES)
    rows = list(rows or [])
    # The listing is what every client's note sidebar renders, so it carries
    # the sharing and read counts inline: one batched query each, not one per
    # note, and no follow-up fetch per row just to label the list.
    note_ids = [str(row.get("note_id") or "") for row in rows]
    shares = await _shares_by_note(runtime, note_ids)
    views = await _views_by_note(runtime, note_ids)
    fresh = await _freshness(runtime, rows, account_bi, actor)
    notes = []
    for row in rows:
        role = await _role(runtime, row, account_bi)
        if not role:
            continue
        if query and not await _matches(runtime, row, query):
            continue
        note = await _project(runtime, row, role, full=False,
                              counts=views.get(row["note_id"], _counts(None)))
        note["shares"] = shares.get(row["note_id"], [])
        note.update(fresh.get(row["note_id"], {}))
        notes.append(note)
    return _response(runtime, {"ok": True, "notes": notes})


async def _create(runtime, account_bi, actor, data):
    title = _title(data.get("title")) or "Untitled note"
    markdown = _markdown(data.get("markdown"))
    visibility = "public" if data.get("visibility") == "public" else "private"
    emoji = _emoji(data.get("emoji"))
    if emoji is None:
        return _response(runtime, {"error": "invalid_emoji"}, status=400)
    now = runtime.now()
    note_id = runtime.new_id()
    sealed = await runtime.seal({"title": title, "markdown": markdown, "emoji": emoji})
    published = now if visibility == "public" else 0
    await runtime.d1_run(
        "INSERT INTO notes(note_id,owner_bi,visibility,version,created_at,"
        "updated_at,published_at,data) VALUES(?,?,?,1,?,?,?,?)",
        note_id, account_bi, visibility, now, now, published, sealed)
    await runtime.d1_run(
        "INSERT INTO note_versions(note_id,version,author_bi,created_at,data) "
        "VALUES(?,1,?,?,?)", note_id, account_bi, now, sealed)
    row = await _row(runtime, note_id)
    await runtime.audit(actor, "note.create", "note", note_id)
    return _response(runtime, {
        "ok": True, "note": await _project(runtime, row, "owner")}, status=201)


async def _update(runtime, row, account_bi, actor, data, event=None):
    role = await _role(runtime, row, account_bi)
    if role not in ("owner", "editor"):
        return _response(runtime, {"error": "note_not_found"}, status=404)
    try:
        base_version = int(data.get("baseVersion"))
    except (TypeError, ValueError, OverflowError):
        return _response(runtime, {"error": "base_version_required"}, status=400)
    current_version = int(row.get("version") or 1)
    if base_version != current_version:
        return _response(runtime, {
            "error": "version_conflict",
            "note": await _project(runtime, row, role),
        }, status=409)
    old = await runtime.open(row.get("data"))
    old = old if isinstance(old, dict) else {}
    title = _title(data.get("title", old.get("title"))) or "Untitled note"
    markdown = _markdown(data.get("markdown", old.get("markdown")))
    emoji = _emoji(data.get("emoji", old.get("emoji")))
    if emoji is None:
        return _response(runtime, {"error": "invalid_emoji"}, status=400)
    visibility = str(row.get("visibility") or "private")
    if "visibility" in data:
        if role != "owner" or data.get("visibility") not in ("private", "public"):
            return _response(runtime, {"error": "owner_required"}, status=403)
        visibility = data["visibility"]
    new_version = current_version + 1
    now = runtime.now()
    published = int(row.get("published_at") or 0)
    if visibility == "public" and not published:
        published = now
    if visibility == "private":
        published = 0
    sealed = await runtime.seal({"title": title, "markdown": markdown, "emoji": emoji})
    result = await runtime.d1_run(
        "UPDATE notes SET visibility=?,version=?,updated_at=?,published_at=?,"
        "data=? WHERE note_id=? AND version=?",
        visibility, new_version, now, published, sealed,
        row["note_id"], current_version)
    if runtime.changes(result) != 1:
        latest = await _row(runtime, row["note_id"])
        return _response(runtime, {
            "error": "version_conflict",
            "note": await _project(runtime, latest, await _role(runtime, latest, account_bi)),
        }, status=409)
    await runtime.d1_run(
        "INSERT INTO note_versions(note_id,version,author_bi,created_at,data) "
        "VALUES(?,?,?,?,?)", row["note_id"], new_version, account_bi, now, sealed)
    await runtime.d1_run(
        "DELETE FROM note_versions WHERE note_id=? AND version NOT IN "
        "(SELECT version FROM note_versions WHERE note_id=? "
        "ORDER BY version DESC LIMIT ?)", row["note_id"], row["note_id"], MAX_VERSIONS)
    updated = await _row(runtime, row["note_id"])
    await runtime.broadcast(row["note_id"], {
        "type": "note-updated", "version": new_version,
        "updatedAt": now, "actor": actor})
    if visibility != str(row.get("visibility") or "private"):
        await _log(runtime, row["note_id"], account_bi,
                   "publish" if visibility == "public" else "unpublish")
    if event:
        await _log(runtime, row["note_id"], account_bi, event[0],
                   {**event[1], "as": new_version})
    return _response(runtime, {
        "ok": True, "note": await _project(runtime, updated, role)})


async def _author_names(runtime, note_id, rows, account_bi, actor):
    names = {account_bi: actor} if account_bi and actor else {}
    wanted = {str(item.get("author_bi") or "") for item in rows or []}
    wanted.discard("")
    if wanted - set(names):
        for share in await runtime.d1_all(
                "SELECT principal_bi,principal_name FROM note_shares "
                "WHERE note_id=? AND principal_type='user'", note_id) or []:
            names.setdefault(str(share.get("principal_bi") or ""),
                             str(share.get("principal_name") or ""))
    for author_bi in sorted(wanted - set(names)):
        names[author_bi] = str(await runtime.account_name(author_bi) or "")
    return names


async def _versions(runtime, row, account_bi, actor, method, data):
    role = await _role(runtime, row, account_bi)
    if not role:
        return _response(runtime, {"error": "note_not_found"}, status=404)
    if method == "GET":
        rows = await runtime.d1_all(
            "SELECT version,author_bi,created_at FROM note_versions "
            "WHERE note_id=? ORDER BY version DESC LIMIT ?",
            row["note_id"], MAX_VERSIONS)
        names = (await _author_names(runtime, row["note_id"], rows,
                                     account_bi, actor)
                 if role != "public" else None)
        versions = []
        for item in rows or []:
            entry = {
                "version": int(item.get("version") or 0),
                "createdAt": int(item.get("created_at") or 0),
            }
            if names is not None:
                entry["author"] = names.get(str(item.get("author_bi") or ""), "")
            versions.append(entry)
        return _response(runtime, {"ok": True, "versions": versions})
    if role not in ("owner", "editor"):
        return _response(runtime, {"error": "editor_required"}, status=403)
    try:
        target = int(data.get("version"))
        base = int(data.get("baseVersion"))
    except (TypeError, ValueError, OverflowError):
        return _response(runtime, {"error": "invalid_version"}, status=400)
    version_row = await runtime.d1_first(
        "SELECT data FROM note_versions WHERE note_id=? AND version=?",
        row["note_id"], target)
    if not version_row:
        return _response(runtime, {"error": "version_not_found"}, status=404)
    restored = await runtime.open(version_row.get("data"))
    return await _update(runtime, row, account_bi, actor, {
        "baseVersion": base,
        "title": (restored or {}).get("title", "Untitled note"),
        "markdown": (restored or {}).get("markdown", ""),
        "emoji": (restored or {}).get("emoji", ""),
    }, event=("restore", {"version": target}))


async def _suggestions(runtime, account_bi, actor, shares, query):
    users = await runtime.search_accounts(query, MAX_SUGGESTIONS * 2)
    organizations = await runtime.member_organizations(account_bi)
    taken = {(share["type"], share["name"].lower()) for share in shares}
    found = []
    for kind, names in (("user", users), ("organization", organizations)):
        for value in names or []:
            name = str(value or "").strip().lower()
            if (not name or (kind == "user" and name == actor)
                    or (kind == "organization" and query not in name)
                    or (kind, name) in taken):
                continue
            taken.add((kind, name))
            found.append({"type": kind, "name": name})
    found.sort(key=lambda item: not item["name"].startswith(query))
    return found[:MAX_SUGGESTIONS]


async def _share(runtime, row, account_bi, actor, method, data):
    if await _role(runtime, row, account_bi) != "owner":
        return _response(runtime, {"error": "note_not_found"}, status=404)
    if method == "GET":
        shares = await _shares(runtime, row["note_id"])
        payload = {"ok": True, "shares": shares}
        if "query" in runtime.query_params():
            query = str(runtime.query_params().get("query") or "").strip().lstrip("@").lower()
            if len(query) > MAX_QUERY:
                return _response(runtime, {"error": "invalid_query"}, status=400)
            payload["suggestions"] = (await _suggestions(
                runtime, account_bi, actor, shares, query) if query else [])
        return _response(runtime, payload)
    kind = str(data.get("type") or "").strip().lower()
    name = str(data.get("name") or "").strip().lower()
    role = str(data.get("role") or "viewer").strip().lower()
    if kind not in ("user", "organization") or not NAME_RE.fullmatch(name):
        return _response(runtime, {"error": "invalid_principal"}, status=400)
    principal_bi, canonical = await runtime.principal(kind, name)
    if not principal_bi:
        return _response(runtime, {"error": "principal_not_found"}, status=404)
    previous = await runtime.d1_first(
        "SELECT role FROM note_shares WHERE note_id=? AND principal_type=? "
        "AND principal_bi=?", row["note_id"], kind, principal_bi)
    target = {"type": kind, "name": canonical}
    event = None
    if method == "DELETE":
        await runtime.d1_run(
            "DELETE FROM note_shares WHERE note_id=? AND principal_type=? "
            "AND principal_bi=?", row["note_id"], kind, principal_bi)
        if previous:
            event = ("unshare", target)
    else:
        if role not in ROLES:
            return _response(runtime, {"error": "invalid_role"}, status=400)
        if not previous:
            event = ("share", {**target, "role": role})
        elif str(previous.get("role") or "") != role:
            event = ("role", {**target, "role": role,
                              "from": str(previous.get("role") or "")})
        await runtime.d1_run(
            "INSERT INTO note_shares(note_id,principal_type,principal_bi,"
            "principal_name,role,added_by_bi,created_at) VALUES(?,?,?,?,?,?,?) "
            "ON CONFLICT(note_id,principal_type,principal_bi) DO UPDATE SET "
            "principal_name=excluded.principal_name,role=excluded.role",
            row["note_id"], kind, principal_bi, canonical, role,
            account_bi, runtime.now())
    # Force existing sockets through the current ACL immediately.
    await runtime.broadcast(row["note_id"], {"type": "access-changed"})
    if event:
        await _log(runtime, row["note_id"], account_bi, *event)
    return _response(runtime, {"ok": True, "shares": await _shares(runtime, row["note_id"])})


async def _link(runtime, row, account_bi, actor, method, data):
    role = await _role(runtime, row, account_bi)
    if role not in ("owner", "editor"):
        return _response(runtime, {"error": "note_not_found"}, status=404)
    if method == "GET":
        return _response(runtime, {"ok": True, "links": await _links(runtime, row["note_id"])})
    owner = str(data.get("owner") or "").strip().lower()
    repo = str(data.get("repo") or "").strip()
    kind = str(data.get("kind") or "").strip().lower()
    try:
        number = int(data.get("number"))
    except (TypeError, ValueError, OverflowError):
        number = 0
    if (not NAME_RE.fullmatch(owner) or not REPO_RE.fullmatch(repo)
            or kind not in LINK_KINDS or number < 1 or number > 999999999):
        return _response(runtime, {"error": "invalid_link"}, status=400)
    if not await runtime.repository_access(owner, repo, actor):
        return _response(runtime, {"error": "repository_not_found"}, status=404)
    if method == "DELETE":
        result = await runtime.d1_run(
            "DELETE FROM note_links WHERE note_id=? AND owner=? AND repo=? "
            "AND kind=? AND number=?", row["note_id"], owner, repo, kind, number)
    else:
        result = await runtime.d1_run(
            "INSERT OR IGNORE INTO note_links(note_id,owner,repo,kind,number,created_at) "
            "VALUES(?,?,?,?,?,?)", row["note_id"], owner, repo, kind, number, runtime.now())
    if runtime.changes(result) == 1:
        await _log(runtime, row["note_id"], account_bi,
                   "unlink" if method == "DELETE" else "link",
                   {"owner": owner, "repo": repo, "kind": kind, "number": number})
    return _response(runtime, {"ok": True, "links": await _links(runtime, row["note_id"])})


def _client_detail(kind, data):
    detail = data.get("detail") if isinstance(data.get("detail"), dict) else {}
    if kind != "issue":
        return {}
    owner = str(detail.get("owner") or "").strip().lower()
    repo = str(detail.get("repo") or "").strip()
    try:
        number = int(detail.get("number"))
    except (TypeError, ValueError, OverflowError):
        number = 0
    if (not NAME_RE.fullmatch(owner) or not REPO_RE.fullmatch(repo)
            or number < 1 or number > 999999999):
        return {}
    return {"owner": owner, "repo": repo, "number": number}


async def _activity(runtime, row, account_bi, actor, method, data):
    role = await _role(runtime, row, account_bi)
    if role in ("", "public"):
        return _response(runtime, {"error": "note_not_found"}, status=404)
    note_id = row["note_id"]
    if method == "POST":
        kind = str(data.get("kind") or "").strip().lower()
        if kind not in CLIENT_EVENTS:
            return _response(runtime, {"error": "invalid_event"}, status=400)
        await _log(runtime, note_id, account_bi, kind, _client_detail(kind, data))
    events = await runtime.d1_all(
        "SELECT kind,actor_bi,created_at,last_at,hits,detail FROM note_events "
        "WHERE note_id=? ORDER BY last_at DESC,id DESC LIMIT ?",
        note_id, MAX_ACTIVITY)
    versions = await runtime.d1_all(
        "SELECT version,author_bi,created_at FROM note_versions "
        "WHERE note_id=? AND version>1 ORDER BY version DESC LIMIT ?",
        note_id, MAX_VERSIONS)
    owner = {"author_bi": str(row.get("owner_bi") or "")}
    people = ([{"author_bi": item.get("actor_bi")} for item in events or []]
              + list(versions or []) + [owner])
    names = await _author_names(runtime, note_id, people, account_bi, actor)
    entries = []
    for item in events or []:
        entry = {
            "kind": str(item.get("kind") or ""),
            "actor": names.get(str(item.get("actor_bi") or ""), ""),
            "at": int(item.get("last_at") or 0),
            "firstAt": int(item.get("created_at") or 0),
            "count": int(item.get("hits") or 1),
        }
        detail = _detail(item.get("detail"))
        if detail:
            entry["detail"] = detail
        entries.append(entry)
    for item in versions or []:
        entries.append({
            "kind": "edit",
            "actor": names.get(str(item.get("author_bi") or ""), ""),
            "at": int(item.get("created_at") or 0),
            "version": int(item.get("version") or 0),
        })
    entries.append({
        "kind": "create",
        "actor": names.get(owner["author_bi"], ""),
        "at": int(row.get("created_at") or 0),
        "version": 1,
    })
    entries.sort(key=lambda entry: entry["at"], reverse=True)
    counts = await _views(runtime, note_id, str(row.get("visibility") or "") == "public")
    counts["versions"] = int(row.get("version") or 1)
    return _response(runtime, {"ok": True, "counts": counts, "events": entries})


def _comment_text(value, limit):
    return str(value or "").replace("\x00", "")[:limit]


def _offset(value):
    try:
        return max(0, min(MAX_OFFSET, int(value)))
    except (TypeError, ValueError, OverflowError):
        return 0


async def _comment_rows(runtime, note_id):
    return await runtime.d1_all(
        "SELECT comment_id,parent_id,author_bi,anchor_start,anchor_end,"
        "created_at,updated_at,edited_at,resolved_at,resolved_bi,data "
        "FROM note_comments WHERE note_id=? ORDER BY created_at,comment_id",
        note_id) or []


async def _comment_row(runtime, note_id, comment_id):
    return await runtime.d1_first(
        "SELECT comment_id,parent_id,author_bi,resolved_at,data "
        "FROM note_comments WHERE note_id=? AND comment_id=?",
        note_id, comment_id)


async def _comments_key(runtime, note_id):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS total,COALESCE(MAX(updated_at),0) AS latest "
        "FROM note_comments WHERE note_id=?", note_id) or {}
    return "%d:%d" % (int(row.get("total") or 0), int(row.get("latest") or 0))


async def _collaborator(runtime, row, account_bi):
    return bool(account_bi) and await _role(runtime, row, account_bi) in (
        "owner", "editor", "viewer")


async def _mentions(runtime, row, body):
    found = []
    for match in MENTION_RE.finditer(body):
        name = match.group(1).lower()
        if name in found:
            continue
        if len(found) >= 20:
            break
        principal_bi, canonical = await runtime.principal("user", name)
        if principal_bi and await _collaborator(runtime, row, principal_bi):
            found.append(canonical or name)
    return found


async def _comment_payload(runtime, row, account_bi, actor, role):
    note_id = row["note_id"]
    rows = await _comment_rows(runtime, note_id)
    owner_bi = str(row.get("owner_bi") or "")
    wanted = (list(rows) + [{"author_bi": item.get("resolved_bi")} for item in rows]
              + [{"author_bi": owner_bi}])
    names = await _author_names(runtime, note_id, wanted, account_bi, actor)
    comments = []
    for item in rows:
        data = await runtime.open(item.get("data"))
        data = data if isinstance(data, dict) else {}
        mentions = data.get("mentions") if isinstance(data.get("mentions"), list) else []
        comments.append({
            "id": str(item.get("comment_id") or ""),
            "parentId": str(item.get("parent_id") or ""),
            "author": names.get(str(item.get("author_bi") or ""), ""),
            "body": _comment_text(data.get("body"), MAX_COMMENT_BODY),
            "quote": _comment_text(data.get("quote"), MAX_QUOTE),
            "prefix": _comment_text(data.get("prefix"), MAX_CONTEXT),
            "suffix": _comment_text(data.get("suffix"), MAX_CONTEXT),
            "start": int(item.get("anchor_start") or 0),
            "end": int(item.get("anchor_end") or 0),
            "mentions": [str(name) for name in mentions][:20],
            "createdAt": int(item.get("created_at") or 0),
            "updatedAt": int(item.get("updated_at") or 0),
            "editedAt": int(item.get("edited_at") or 0),
            "resolvedAt": int(item.get("resolved_at") or 0),
            "resolvedBy": names.get(str(item.get("resolved_bi") or ""), "")
            if int(item.get("resolved_at") or 0) else "",
        })
    people = [names.get(owner_bi, "")]
    for share in await runtime.d1_all(
            "SELECT principal_name FROM note_shares WHERE note_id=? "
            "AND principal_type='user' ORDER BY principal_name", note_id) or []:
        people.append(str(share.get("principal_name") or ""))
    people += [comment["author"] for comment in comments]
    seen, unique = set(), []
    for name in people:
        name = str(name or "").strip().lower()
        if name and name not in seen:
            seen.add(name)
            unique.append(name)
    return {"ok": True, "comments": comments, "people": unique[:MAX_PEOPLE],
            "canResolve": role in ("owner", "editor")}


async def _comment_ping(runtime, row, comment_id, parent_id, account_bi,
                        actor, body, mentions):
    try:
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        title = _title(data.get("title")) or "Untitled note"
        excerpt = " ".join(body.split())[:200]
        pinged = {actor}
        for name in mentions:
            if name not in pinged:
                pinged.add(name)
                await runtime.notify(name, actor, row["note_id"], comment_id,
                                     "mention", title, excerpt)
        if not parent_id:
            return
        thread = await runtime.d1_all(
            "SELECT DISTINCT author_bi FROM note_comments WHERE note_id=? "
            "AND (comment_id=? OR parent_id=?)", row["note_id"], parent_id, parent_id)
        names = await _author_names(runtime, row["note_id"], thread, account_bi, actor)
        for item in thread or []:
            author_bi = str(item.get("author_bi") or "")
            name = names.get(author_bi, "")
            if (name and name not in pinged
                    and await _collaborator(runtime, row, author_bi)):
                pinged.add(name)
                await runtime.notify(name, actor, row["note_id"], comment_id,
                                     "reply", title, excerpt)
    except Exception:
        pass


def _with_comment(payload, comment_id):
    payload["comment"] = next((item for item in payload["comments"]
                               if item["id"] == comment_id), None)
    return payload


async def _comments(runtime, row, account_bi, actor, role, method, comment_id, data):
    if role not in ("owner", "editor", "viewer"):
        return _response(runtime, {"error": "note_not_found"}, status=404)
    note_id = row["note_id"]
    if method == "GET":
        return _response(runtime, await _comment_payload(
            runtime, row, account_bi, actor, role))
    if method == "POST":
        body = _comment_text(data.get("body"), MAX_COMMENT_BODY + 1).strip()
        if not body or len(body) > MAX_COMMENT_BODY:
            return _response(runtime, {"error": "invalid_comment"}, status=400)
        parent_id = str(data.get("parentId") or "").strip().lower()
        quote, prefix, suffix, start, end = "", "", "", 0, 0
        if parent_id:
            parent = (await _comment_row(runtime, note_id, parent_id)
                      if NOTE_ID_RE.fullmatch(parent_id) else None)
            if not parent or parent.get("parent_id"):
                return _response(runtime, {"error": "comment_not_found"}, status=404)
        else:
            quote = _comment_text(data.get("quote"), MAX_QUOTE)
            if quote:
                prefix = _comment_text(data.get("prefix"), MAX_OFFSET)[-MAX_CONTEXT:]
                suffix = _comment_text(data.get("suffix"), MAX_CONTEXT)
                start = _offset(data.get("start"))
                end = max(start, _offset(data.get("end")))
        total = await runtime.d1_first(
            "SELECT COUNT(*) AS total FROM note_comments WHERE note_id=?", note_id) or {}
        if int(total.get("total") or 0) >= MAX_COMMENTS:
            return _response(runtime, {"error": "too_many_comments"}, status=409)
        mentions = await _mentions(runtime, row, body)
        comment_id = runtime.new_id()
        now = runtime.now()
        sealed = await runtime.seal({
            "body": body, "quote": quote, "prefix": prefix, "suffix": suffix,
            "mentions": mentions})
        await runtime.d1_run(
            "INSERT INTO note_comments(comment_id,note_id,parent_id,author_bi,"
            "anchor_start,anchor_end,created_at,updated_at,edited_at,resolved_at,"
            "resolved_bi,data) VALUES(?,?,?,?,?,?,?,?,0,0,'',?)",
            comment_id, note_id, parent_id, account_bi, start, end, now, now, sealed)
        await _log(runtime, note_id, account_bi, "comment",
                   {"reply": True} if parent_id else None)
        await _comment_ping(runtime, row, comment_id, parent_id, account_bi,
                            actor, body, mentions)
        await runtime.broadcast(note_id, {"type": "note-comments", "actor": actor})
        payload = await _comment_payload(runtime, row, account_bi, actor, role)
        return _response(runtime, _with_comment(payload, comment_id), status=201)
    existing = await _comment_row(runtime, note_id, comment_id)
    if not existing:
        return _response(runtime, {"error": "comment_not_found"}, status=404)
    mine = str(existing.get("author_bi") or "") == account_bi
    if method == "DELETE":
        if not mine and role != "owner":
            return _response(runtime, {"error": "comment_author_required"}, status=403)
        await runtime.d1_run(
            "DELETE FROM note_comments WHERE note_id=? AND (comment_id=? OR parent_id=?)",
            note_id, comment_id, comment_id)
        await runtime.broadcast(note_id, {"type": "note-comments", "actor": actor})
        return _response(runtime, await _comment_payload(
            runtime, row, account_bi, actor, role))
    now = runtime.now()
    if "resolved" in data:
        if existing.get("parent_id"):
            return _response(runtime, {"error": "comment_not_found"}, status=404)
        if not mine and role not in ("owner", "editor"):
            return _response(runtime, {"error": "resolve_not_allowed"}, status=403)
        resolved = data.get("resolved") is True
        was = bool(int(existing.get("resolved_at") or 0))
        if resolved != was:
            await runtime.d1_run(
                "UPDATE note_comments SET resolved_at=?,resolved_bi=?,updated_at=? "
                "WHERE note_id=? AND comment_id=?",
                now if resolved else 0, account_bi if resolved else "", now,
                note_id, comment_id)
            await _log(runtime, note_id, account_bi,
                       "resolve" if resolved else "reopen")
    else:
        if not mine:
            return _response(runtime, {"error": "comment_author_required"}, status=403)
        body = _comment_text(data.get("body"), MAX_COMMENT_BODY + 1).strip()
        if not body or len(body) > MAX_COMMENT_BODY:
            return _response(runtime, {"error": "invalid_comment"}, status=400)
        old = await runtime.open(existing.get("data"))
        old = old if isinstance(old, dict) else {}
        before = old.get("mentions") if isinstance(old.get("mentions"), list) else []
        mentions = await _mentions(runtime, row, body)
        await runtime.d1_run(
            "UPDATE note_comments SET data=?,updated_at=?,edited_at=? "
            "WHERE note_id=? AND comment_id=?",
            await runtime.seal({**old, "body": body, "mentions": mentions}),
            now, now, note_id, comment_id)
        added = [name for name in mentions if name not in before]
        if added:
            await _comment_ping(runtime, row, comment_id, "", account_bi,
                                actor, body, added)
    await runtime.broadcast(note_id, {"type": "note-comments", "actor": actor})
    payload = await _comment_payload(runtime, row, account_bi, actor, role)
    return _response(runtime, _with_comment(payload, comment_id))


async def _presence(runtime, row, actor, role, method, data):
    if role not in ("owner", "editor", "viewer"):
        return _response(runtime, {"error": "note_not_found"}, status=404)
    frame = None
    client_id = str(runtime.query_params().get("clientId") or "")
    if method == "POST":
        client_id = str(data.get("clientId") or "")
        state = str(data.get("state") or "viewing").strip().lower()
        if not CLIENT_ID_RE.fullmatch(client_id) or state not in PRESENCE_STATES:
            return _response(runtime, {"error": "invalid_presence"}, status=400)
        if state == "editing" and role not in ("owner", "editor"):
            state = "viewing"
        frame = {"clientId": client_id, "name": actor, "state": state}
    try:
        peers = await runtime.presence(row["note_id"], frame)
    except Exception:
        peers = []
    peers = [peer for peer in peers or []
             if isinstance(peer, dict) and peer.get("clientId") != client_id]
    return _response(runtime, {
        "ok": True, "peers": peers,
        "commentsKey": await _comments_key(runtime, row["note_id"])})


async def _page_row(runtime, page_id):
    return await runtime.d1_first(
        "SELECT page_id,owner_bi,version,created_at,updated_at,data "
        "FROM note_pages WHERE page_id=?", page_id)


async def _page_role(runtime, row, account_bi):
    if not row or not account_bi:
        return ""
    if str(row.get("owner_bi") or "") == account_bi:
        return "owner"
    direct = await runtime.d1_first(
        "SELECT role FROM note_page_shares WHERE page_id=? "
        "AND principal_type='user' AND principal_bi=?", row["page_id"], account_bi)
    if direct:
        return str(direct.get("role") or "viewer")
    organization = await runtime.d1_first(
        "SELECT s.role AS role, m.role AS member_role FROM note_page_shares s "
        "INNER JOIN org_members m ON m.org_bi=s.principal_bi WHERE s.page_id=? "
        "AND s.principal_type='organization' AND m.member_bi=? "
        "AND m.role IN " + ORG_INSIDER_ROLE_SQL + " "
        "ORDER BY CASE WHEN s.role='editor' AND m.role<>'auditor' "
        "THEN 0 ELSE 1 END LIMIT 1", row["page_id"], account_bi)
    if organization:
        if str(organization.get("member_role") or "") == "auditor":
            return "viewer"
        return str(organization.get("role") or "viewer")
    return ""


async def _page_shares(runtime, page_id):
    rows = await runtime.d1_all(
        "SELECT principal_type,principal_name,role,created_at FROM note_page_shares "
        "WHERE page_id=? ORDER BY principal_type,principal_name COLLATE NOCASE", page_id)
    return [{
        "type": str(row.get("principal_type") or ""),
        "name": str(row.get("principal_name") or ""),
        "role": str(row.get("role") or "viewer"),
        "createdAt": int(row.get("created_at") or 0),
    } for row in rows or []]


def _page_entry(value):
    if not isinstance(value, dict):
        return None
    entry_id = str(value.get("id") or "")
    kind = str(value.get("kind") or "")
    if not ENTRY_ID_RE.fullmatch(entry_id) or kind not in PAGE_KINDS:
        return None
    depth = value.get("depth", 0)
    if isinstance(depth, bool) or not isinstance(depth, int) or not 0 <= depth <= 3:
        return None
    entry = {"id": entry_id, "kind": kind, "depth": depth,
             "label": _title(value.get("label")), "noteId": "", "url": "",
             "collapsed": value.get("collapsed") is True}
    if kind == "note":
        note_id = str(value.get("noteId") or "").lower()
        if not NOTE_ID_RE.fullmatch(note_id):
            return None
        entry["noteId"] = note_id
    elif kind == "link":
        url = str(value.get("url") or "").strip()
        if (not url.startswith("https://") or len(url) > 2000
                or any(char.isspace() or ord(char) < 32 for char in url)):
            return None
        entry["url"] = url
        entry["label"] = entry["label"] or url[8:][:MAX_TITLE]
    elif not entry["label"]:
        return None
    return entry


def _page_fields(data, old):
    fields = dict(old)
    if "title" in data:
        fields["title"] = _title(data.get("title"))
    if "emoji" in data:
        emoji = _emoji(data.get("emoji"))
        if emoji is None:
            return None, "invalid_emoji"
        fields["emoji"] = emoji
    if "description" in data:
        description = str(data.get("description") or "").replace("\x00", "")
        if len(description) > MAX_PAGE_DESCRIPTION:
            return None, "invalid_page"
        fields["description"] = description
    if "accent" in data:
        if data.get("accent") not in PAGE_ACCENTS:
            return None, "invalid_page"
        fields["accent"] = data["accent"]
    if "layout" in data:
        if data.get("layout") not in PAGE_LAYOUTS:
            return None, "invalid_page"
        fields["layout"] = data["layout"]
    if "showEmoji" in data:
        if not isinstance(data.get("showEmoji"), bool):
            return None, "invalid_page"
        fields["showEmoji"] = data["showEmoji"]
    if "outline" in data:
        outline = data.get("outline")
        if not isinstance(outline, list):
            return None, "invalid_page"
        if len(outline) > MAX_PAGE_ENTRIES:
            return None, "too_many_entries"
        entries = [_page_entry(item) for item in outline]
        if any(entry is None for entry in entries):
            return None, "invalid_page"
        if len({entry["id"] for entry in entries}) != len(entries):
            return None, "invalid_page"
        fields["outline"] = entries
    fields.setdefault("title", "")
    fields.setdefault("emoji", "")
    fields.setdefault("description", "")
    fields.setdefault("accent", "blue")
    fields.setdefault("layout", "outline")
    fields.setdefault("showEmoji", True)
    fields.setdefault("outline", [])
    return fields, ""


async def _page_data(runtime, row):
    data = await runtime.open(row.get("data"))
    data = data if isinstance(data, dict) else {}
    fields, _ = _page_fields({}, data)
    return fields


async def _page_project(runtime, row, role, full=True):
    data = await _page_data(runtime, row)
    result = {
        "id": row["page_id"],
        "title": data["title"] or "Untitled page",
        "emoji": data["emoji"],
        "description": data["description"],
        "accent": data["accent"],
        "layout": data["layout"],
        "showEmoji": data["showEmoji"],
        "role": role,
        "version": int(row.get("version") or 1),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "entries": len(data["outline"]),
        "shares": await _page_shares(runtime, row["page_id"]),
    }
    if full:
        result["outline"] = data["outline"]
    return result


async def _page_notes(runtime, outline, account_bi, actor, content):
    rows, roles = [], {}
    for note_id in dict.fromkeys(entry["noteId"] for entry in outline if entry["kind"] == "note"):
        row = await _row(runtime, note_id)
        role = await _role(runtime, row, account_bi)
        if row and role:
            rows.append(row)
            roles[note_id] = role
    fresh = await _freshness(runtime, rows, account_bi, actor)
    notes, budget = {}, MAX_PAGE_CONTENT
    for row in rows:
        note_id = row["note_id"]
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        summary = {
            "id": note_id,
            "title": _title(data.get("title")) or "Untitled note",
            "emoji": _emoji(data.get("emoji")) or "",
            "updatedAt": int(row.get("updated_at") or 0),
            "version": int(row.get("version") or 1),
            "role": roles[note_id],
            "lastEditedBy": fresh.get(note_id, {}).get("lastEditedBy", ""),
            "unread": fresh.get(note_id, {}).get("unread", False),
        }
        if content:
            markdown = _markdown(data.get("markdown"))
            size = len(markdown.encode("utf-8"))
            if size <= budget:
                summary["markdown"] = markdown
                budget -= size
        notes[note_id] = summary
    return notes


async def _page_list(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT DISTINCT p.page_id,p.owner_bi,p.version,p.created_at,p.updated_at,p.data "
        "FROM note_pages p LEFT JOIN note_page_shares s ON s.page_id=p.page_id "
        "LEFT JOIN org_members m ON s.principal_type='organization' "
        "AND m.org_bi=s.principal_bi AND m.member_bi=? "
        "AND m.role IN " + ORG_INSIDER_ROLE_SQL + " "
        "WHERE p.owner_bi=? OR (s.principal_type='user' AND s.principal_bi=?) "
        "OR m.member_bi=? ORDER BY p.updated_at DESC LIMIT ?",
        account_bi, account_bi, account_bi, account_bi, MAX_PAGES)
    pages = []
    for row in rows or []:
        role = await _page_role(runtime, row, account_bi)
        if role:
            pages.append(await _page_project(runtime, row, role, full=False))
    return _response(runtime, {"ok": True, "pages": pages})


async def _page_unknown_notes(runtime, outline, known, account_bi):
    for entry in outline:
        if entry["kind"] != "note" or entry["noteId"] in known:
            continue
        row = await _row(runtime, entry["noteId"])
        if await _role(runtime, row, account_bi) not in ("owner", "editor", "viewer", "public"):
            return True
        known.add(entry["noteId"])
    return False


async def _page_create(runtime, account_bi, data):
    fields, error = _page_fields(data, {})
    if error:
        return _response(runtime, {"error": error}, status=400)
    if await _page_unknown_notes(runtime, fields["outline"], set(), account_bi):
        return _response(runtime, {"error": "note_not_found"}, status=404)
    page_id = runtime.new_id()
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO note_pages(page_id,owner_bi,version,created_at,updated_at,data) "
        "VALUES(?,?,1,?,?,?)", page_id, account_bi, now, now, await runtime.seal(fields))
    row = await _page_row(runtime, page_id)
    return _response(runtime, {"ok": True, "page": await _page_project(runtime, row, "owner")},
                     status=201)


async def _page_update(runtime, row, account_bi, role, data):
    if role not in ("owner", "editor"):
        return _response(runtime, {"error": "page_not_found"}, status=404)
    try:
        base_version = int(data.get("baseVersion"))
    except (TypeError, ValueError, OverflowError):
        return _response(runtime, {"error": "base_version_required"}, status=400)
    current = int(row.get("version") or 1)
    if base_version != current:
        return _response(runtime, {"error": "version_conflict",
                                   "page": await _page_project(runtime, row, role)}, status=409)
    old = await _page_data(runtime, row)
    fields, error = _page_fields(data, old)
    if error:
        return _response(runtime, {"error": error}, status=400)
    known = {entry["noteId"] for entry in old["outline"] if entry["kind"] == "note"}
    if await _page_unknown_notes(runtime, fields["outline"], known, account_bi):
        return _response(runtime, {"error": "note_not_found"}, status=404)
    result = await runtime.d1_run(
        "UPDATE note_pages SET version=?,updated_at=?,data=? WHERE page_id=? AND version=?",
        current + 1, runtime.now(), await runtime.seal(fields), row["page_id"], current)
    latest = await _page_row(runtime, row["page_id"])
    if runtime.changes(result) != 1:
        return _response(runtime, {"error": "version_conflict",
                                   "page": await _page_project(runtime, latest, role)}, status=409)
    return _response(runtime, {"ok": True, "page": await _page_project(runtime, latest, role)})


async def _page_share(runtime, row, account_bi, method, data):
    page_id = row["page_id"]
    if method == "GET":
        return _response(runtime, {"ok": True, "shares": await _page_shares(runtime, page_id)})
    kind = str(data.get("type") or "").strip().lower()
    name = str(data.get("name") or "").strip().lower()
    role = str(data.get("role") or "viewer").strip().lower()
    if kind not in ("user", "organization") or not NAME_RE.fullmatch(name):
        return _response(runtime, {"error": "invalid_principal"}, status=400)
    principal_bi, canonical = await runtime.principal(kind, name)
    if not principal_bi:
        return _response(runtime, {"error": "principal_not_found"}, status=404)
    if method == "DELETE":
        await runtime.d1_run(
            "DELETE FROM note_page_shares WHERE page_id=? AND principal_type=? "
            "AND principal_bi=?", page_id, kind, principal_bi)
        return _response(runtime, {"ok": True, "shares": await _page_shares(runtime, page_id)})
    if role not in ROLES:
        return _response(runtime, {"error": "invalid_role"}, status=400)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO note_page_shares(page_id,principal_type,principal_bi,principal_name,"
        "role,added_by_bi,created_at) VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(page_id,principal_type,principal_bi) DO UPDATE SET "
        "principal_name=excluded.principal_name,role=excluded.role",
        page_id, kind, principal_bi, canonical, role, account_bi, now)
    shared_notes = []
    if data.get("includeNotes") is True:
        outline = (await _page_data(runtime, row))["outline"]
        for note_id in dict.fromkeys(entry["noteId"] for entry in outline if entry["kind"] == "note"):
            note = await _row(runtime, note_id)
            if not note or str(note.get("owner_bi") or "") != account_bi:
                continue
            previous = await runtime.d1_first(
                "SELECT role FROM note_shares WHERE note_id=? AND principal_type=? "
                "AND principal_bi=?", note_id, kind, principal_bi)
            if previous and (str(previous.get("role") or "") == "editor" or role == "viewer"):
                continue
            await runtime.d1_run(
                "INSERT INTO note_shares(note_id,principal_type,principal_bi,principal_name,"
                "role,added_by_bi,created_at) VALUES(?,?,?,?,?,?,?) "
                "ON CONFLICT(note_id,principal_type,principal_bi) DO UPDATE SET "
                "principal_name=excluded.principal_name,role=excluded.role",
                note_id, kind, principal_bi, canonical, role, account_bi, now)
            await _log(runtime, note_id, account_bi,
                       "role" if previous else "share",
                       {"type": kind, "name": canonical, "role": role,
                        **({"from": str(previous.get("role") or "")} if previous else {})})
            await runtime.broadcast(note_id, {"type": "access-changed"})
            shared_notes.append(note_id)
    return _response(runtime, {"ok": True, "shares": await _page_shares(runtime, page_id),
                               "sharedNotes": shared_notes})


async def _pages(runtime, method, page_id, action, account_bi, actor, data):
    if not account_bi:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    if not page_id:
        if method == "GET":
            return await _page_list(runtime, account_bi)
        return await _page_create(runtime, account_bi, data)
    row = await _page_row(runtime, page_id)
    role = await _page_role(runtime, row, account_bi)
    if not row or not role:
        return _response(runtime, {"error": "page_not_found"}, status=404)
    if action == "shares":
        if role != "owner":
            return _response(runtime, {"error": "page_not_found"}, status=404)
        return await _page_share(runtime, row, account_bi, method, data)
    if method == "GET":
        page = await _page_project(runtime, row, role)
        content = str(runtime.query_params().get("content") or "") == "1"
        notes = await _page_notes(runtime, page["outline"], account_bi, actor, content)
        return _response(runtime, {"ok": True, "page": page, "notes": notes})
    if method == "DELETE":
        if role != "owner":
            return _response(runtime, {"error": "owner_required"}, status=403)
        await runtime.d1_run("DELETE FROM note_pages WHERE page_id=?", page_id)
        return _response(runtime, {"ok": True})
    return await _page_update(runtime, row, account_bi, role, data)


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    kind, note_id, action, comment_id = route
    allowed = (("GET", "POST") if kind == "pages" and not note_id else
               ("GET", "POST", "DELETE") if kind == "pages" and action == "shares" else
               ("GET", "PATCH", "DELETE") if kind == "pages" else
               ("GET", "POST") if not note_id else
               ("PATCH", "DELETE") if comment_id else
               ("GET", "POST") if action in ("versions", "activity", "presence", "comments") else
               ("GET", "POST", "DELETE") if action in ("shares", "links") else
               ("GET",) if action == "ws" else
               ("GET", "PATCH", "DELETE"))
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, status=405,
                         allow=", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, status=403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(runtime, {"error": error}, status=413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if kind == "pages":
        return await _pages(runtime, method, note_id, action, account_bi, actor, data)
    if not note_id:
        if not account_bi:
            return _response(runtime, {"error": "invalid_session"}, status=401)
        if method == "GET":
            query = str(runtime.query_params().get("query") or "").strip().lower()
            if len(query) > MAX_SEARCH:
                return _response(runtime, {"error": "invalid_query"}, status=400)
            return await _list(runtime, account_bi, query, actor)
        return await _create(runtime, account_bi, actor, data)
    row = await _row(runtime, note_id)
    role = await _role(runtime, row, account_bi)
    if not row or not role:
        return _response(runtime, {"error": "note_not_found"}, status=404)
    if action == "ws":
        if not account_bi:
            return _response(runtime, {"error": "invalid_session"}, status=401)
        return await runtime.realtime(note_id, account_bi, actor, role)
    if action == "versions":
        if not account_bi:
            return _response(runtime, {"error": "invalid_session"}, status=401)
        return await _versions(runtime, row, account_bi, actor, method, data)
    if action == "shares":
        if not account_bi:
            return _response(runtime, {"error": "invalid_session"}, status=401)
        return await _share(runtime, row, account_bi, actor, method, data)
    if action == "links":
        if not account_bi:
            return _response(runtime, {"error": "invalid_session"}, status=401)
        return await _link(runtime, row, account_bi, actor, method, data)
    if action == "activity":
        if not account_bi:
            return _response(runtime, {"error": "invalid_session"}, status=401)
        return await _activity(runtime, row, account_bi, actor, method, data)
    if action == "comments":
        if not account_bi:
            return _response(runtime, {"error": "invalid_session"}, status=401)
        return await _comments(runtime, row, account_bi, actor, role, method,
                               comment_id, data)
    if action == "presence":
        if not account_bi:
            return _response(runtime, {"error": "invalid_session"}, status=401)
        return await _presence(runtime, row, actor, role, method, data)
    if method == "GET":
        # Only anonymous reads of a published note count: an owner opening
        # their own draft, a collaborator, and the desktop's 3s freshness
        # poll all authenticate, so none of them inflate the number.
        if role == "public":
            await _record_view(runtime, note_id)
        note = await _project(runtime, row, role)
        if role != "public":
            note.update((await _freshness(runtime, [row], account_bi, actor)).get(note_id, {}))
        return _response(runtime, {"ok": True, "note": note})
    if not account_bi:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    if method == "DELETE":
        if role != "owner":
            return _response(runtime, {"error": "owner_required"}, status=403)
        await runtime.d1_run("DELETE FROM notes WHERE note_id=?", note_id)
        await runtime.broadcast(note_id, {"type": "note-deleted"})
        await runtime.audit(actor, "note.delete", "note", note_id)
        return _response(runtime, {"ok": True})
    return await _update(runtime, row, account_bi, actor, data)
