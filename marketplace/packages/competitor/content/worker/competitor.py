"""Organization-private competitor watchlists."""

import re


PREFIX = "/api/competitors"
PLUGIN_ID = "competitor"
MAX_BODY_BYTES = 256 * 1024
MAX_ITEMS = 1000
MANAGER_ROLES = frozenset({"owner", "admin"})
ORG_INSIDER_ROLES = frozenset({"owner", "admin", "maintainer", "member", "auditor"})
ORG_READ_ONLY_ROLES = frozenset({"auditor"})
STATUSES = frozenset({"watch", "primary", "archived"})
ORG_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
HTTPS_RE = re.compile(r"^https://[^\s]{1,990}$", re.IGNORECASE)


DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS competitor_profiles (
        competitor_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        status TEXT NOT NULL CHECK(status IN ('watch','primary','archived')),
        created_by_bi TEXT NOT NULL,
        created_by_name TEXT NOT NULL,
        updated_by_bi TEXT NOT NULL,
        updated_by_name TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        last_reviewed_at INTEGER NOT NULL DEFAULT 0 CHECK(last_reviewed_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_competitor_profiles_org_status "
    "ON competitor_profiles(org_bi,status,updated_at DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_competitor_profiles_limit
        BEFORE INSERT ON competitor_profiles
        WHEN (SELECT COUNT(*) FROM competitor_profiles WHERE org_bi=NEW.org_bi)>=1000
        BEGIN SELECT RAISE(ABORT,'competitor_profiles_full'); END""",
]
DATA_SCHEMA_DOWN = ["DROP TABLE IF EXISTS competitor_profiles"]


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
    return str(value or "").replace("\x00", "")[:limit]


def _integer(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _status(value):
    value = _clean(value, 20).lower()
    return value if value in STATUSES else "watch"


def _website(value):
    value = _clean(value, 1000)
    return value if not value or HTTPS_RE.fullmatch(value) else ""


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return "organizations", "", ""
    match = re.fullmatch(
        r"/api/competitors/([a-z][a-z0-9-]{0,62})"
        r"(?:/items(?:/([0-9a-f]{32}))?)?",
        clean,
    )
    return ("organization", match.group(1), match.group(2) or "") if match else None


def _profile_data(data, existing=None):
    result = dict(existing or {})
    for key, limit, cleaner in (
        ("name", 160, _clean),
        ("website", 1000, _clean),
        ("category", 100, _clean),
        ("positioning", 1000, _text),
        ("strengths", 10000, _text),
        ("weaknesses", 10000, _text),
        ("notes", 20000, _text),
    ):
        if key in data:
            result[key] = cleaner(data.get(key), limit)
    result["website"] = _website(result.get("website"))
    return result


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _clean((record or {}).get("name"), 64).lower()


async def _organizations(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT o.name,m.role,p.plugin AS installed FROM org_members m "
        "JOIN orgs o ON o.org_bi=m.org_bi LEFT JOIN org_plugin_installs p "
        "ON p.org_bi=o.org_bi AND p.plugin=? WHERE m.member_bi=? "
        "ORDER BY o.name COLLATE NOCASE LIMIT 200",
        PLUGIN_ID,
        account_bi,
    )
    return [
        {
            "name": str(row.get("name") or ""),
            "role": str(row.get("role") or "member"),
            "installed": bool(row.get("installed")),
        }
        for row in rows or []
    ]


async def _row(runtime, org_bi, competitor_id):
    return await runtime.d1_first(
        "SELECT competitor_id,org_name,status,created_by_name,updated_by_name,"
        "created_at,updated_at,last_reviewed_at,data FROM competitor_profiles "
        "WHERE org_bi=? AND competitor_id=?",
        org_bi,
        competitor_id,
    )


async def _project(runtime, row):
    data = await runtime.open(row.get("data"))
    data = data if isinstance(data, dict) else {}
    return {
        "id": str(row.get("competitor_id") or ""),
        "organization": str(row.get("org_name") or ""),
        "status": str(row.get("status") or "watch"),
        "name": str(data.get("name") or ""),
        "website": str(data.get("website") or ""),
        "category": str(data.get("category") or ""),
        "positioning": str(data.get("positioning") or ""),
        "strengths": str(data.get("strengths") or ""),
        "weaknesses": str(data.get("weaknesses") or ""),
        "notes": str(data.get("notes") or ""),
        "createdBy": str(row.get("created_by_name") or ""),
        "updatedBy": str(row.get("updated_by_name") or ""),
        "createdAt": _integer(row.get("created_at")),
        "updatedAt": _integer(row.get("updated_at")),
        "lastReviewedAt": _integer(row.get("last_reviewed_at")),
    }


async def _profiles(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT competitor_id,org_name,status,created_by_name,updated_by_name,"
        "created_at,updated_at,last_reviewed_at,data FROM competitor_profiles "
        "WHERE org_bi=? ORDER BY status='primary' DESC,status='watch' DESC,"
        "updated_at DESC,competitor_id LIMIT ?",
        org_bi,
        MAX_ITEMS,
    )
    return [await _project(runtime, row) for row in rows or []]


def _summary(profiles, now):
    review_before = now - 90 * 24 * 60 * 60 * 1000
    active = [item for item in profiles if item["status"] != "archived"]
    return {
        "watchlist": len(active),
        "primary": sum(item["status"] == "primary" for item in profiles),
        "archived": sum(item["status"] == "archived" for item in profiles),
        "needsReview": sum(
            not item["lastReviewedAt"] or item["lastReviewedAt"] < review_before
            for item in active
        ),
    }


async def _overview(runtime, org_bi, org_name, role):
    profiles = await _profiles(runtime, org_bi)
    return _response(
        runtime,
        {
            "ok": True,
            "organization": org_name,
            "role": role,
            "canManagePlugin": role in MANAGER_ROLES,
            "competitors": profiles,
            "summary": _summary(profiles, runtime.now()),
        },
    )


async def _create(runtime, org_bi, org_name, account_bi, actor, data):
    envelope = _profile_data(data)
    if not envelope.get("name"):
        return _response(runtime, {"error": "name_required"}, 400)
    if data.get("website") and not envelope.get("website"):
        return _response(runtime, {"error": "invalid_website"}, 400)
    now, competitor_id = runtime.now(), runtime.new_id()
    reviewed = max(0, _integer(data.get("lastReviewedAt"), now))
    await runtime.d1_run(
        "INSERT INTO competitor_profiles(competitor_id,org_bi,org_name,status,"
        "created_by_bi,created_by_name,updated_by_bi,updated_by_name,created_at,"
        "updated_at,last_reviewed_at,data) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        competitor_id,
        org_bi,
        org_name,
        _status(data.get("status")),
        account_bi,
        actor,
        account_bi,
        actor,
        now,
        now,
        reviewed,
        await runtime.seal(envelope),
    )
    await runtime.audit(actor, "competitor.create", "competitor_profile", competitor_id)
    return _response(
        runtime,
        {"ok": True, "competitor": await _project(runtime, await _row(runtime, org_bi, competitor_id))},
        201,
    )


async def _update(runtime, row, org_bi, account_bi, actor, data):
    existing = await runtime.open(row.get("data"))
    envelope = _profile_data(data, existing if isinstance(existing, dict) else {})
    if not envelope.get("name"):
        return _response(runtime, {"error": "name_required"}, 400)
    if "website" in data and data.get("website") and not envelope.get("website"):
        return _response(runtime, {"error": "invalid_website"}, 400)
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE competitor_profiles SET status=?,updated_by_bi=?,updated_by_name=?,"
        "updated_at=?,last_reviewed_at=?,data=? WHERE org_bi=? AND competitor_id=?",
        _status(data.get("status", row.get("status"))),
        account_bi,
        actor,
        now,
        max(0, _integer(data.get("lastReviewedAt"), row.get("last_reviewed_at"))),
        await runtime.seal(envelope),
        org_bi,
        row["competitor_id"],
    )
    await runtime.audit(actor, "competitor.update", "competitor_profile", row["competitor_id"])
    return _response(
        runtime,
        {"ok": True, "competitor": await _project(runtime, await _row(runtime, org_bi, row["competitor_id"]))},
    )


async def org_has_data(runtime, org_bi):
    row = await runtime.d1_first(
        "SELECT competitor_id FROM competitor_profiles WHERE org_bi=? LIMIT 1",
        org_bi,
    )
    return bool(row)


async def export_org_data(runtime, org_bi, org_name):
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-competitor-export-v1",
        "organization": org_name,
        "exportedAt": runtime.now(),
        "competitors": await _profiles(runtime, org_bi),
    }


async def purge_org_data(runtime, org_bi):
    await runtime.d1_run("DELETE FROM competitor_profiles WHERE org_bi=?", org_bi)


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    kind, org_name, item = route
    allowed = ("GET",) if kind == "organizations" else (
        ("GET", "PATCH", "DELETE") if item else ("GET", "POST")
    )
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(
                runtime,
                {"error": error},
                413 if error == "payload_too_large" else 400,
            )
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    if kind == "organizations":
        return _response(
            runtime,
            {"ok": True, "actor": actor, "organizations": await _organizations(runtime, account_bi)},
        )
    if not ORG_RE.fullmatch(org_name):
        return _response(runtime, {"error": "organization_not_found"}, 404)
    org_bi, role = await runtime.organization(org_name, account_bi)
    if not org_bi or role not in ORG_INSIDER_ROLES:
        return _response(runtime, {"error": "organization_not_found"}, 404)
    if role in ORG_READ_ONLY_ROLES and method not in ("GET", "HEAD"):
        return _response(runtime, {"error": "read_only_role"}, 403)
    if not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return _response(
            runtime,
            {"error": "plugin_not_installed", "organization": org_name, "canInstall": role in MANAGER_ROLES},
            409,
        )
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    if not item:
        if method == "GET":
            return await _overview(runtime, org_bi, org_name, role)
        return await _create(runtime, org_bi, org_name, account_bi, actor, data)
    row = await _row(runtime, org_bi, item)
    if not row:
        return _response(runtime, {"error": "competitor_not_found"}, 404)
    if method == "GET":
        return _response(runtime, {"ok": True, "competitor": await _project(runtime, row)})
    if method == "DELETE":
        await runtime.d1_run(
            "DELETE FROM competitor_profiles WHERE org_bi=? AND competitor_id=?",
            org_bi,
            item,
        )
        await runtime.audit(actor, "competitor.delete", "competitor_profile", item)
        return _response(runtime, {"ok": True, "deleted": True})
    return await _update(runtime, row, org_bi, account_bi, actor, data)
