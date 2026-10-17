import base64
import re


PREFIX = "/api/app-store-status"
PLUGIN_ID = "app-store-status"
REQUEST_PROOF = "forkmesh-app-store-status-request-v1"
FORMAT = "forkmesh-app-store-status-v1"
MAX_BODY_BYTES = 256 * 1024
MAX_SCREENSHOT_BYTES = 3 * 1024 * 1024
MAX_SCREENSHOT_BODY_BYTES = MAX_SCREENSHOT_BYTES * 4 // 3 + 64 * 1024
MAX_SCREENSHOTS = 4
SCREENSHOT_MIMES = {"image/jpeg": b"\xff\xd8\xff", "image/png": b"\x89PNG\r\n\x1a\n"}
SCREENSHOT_ID_RE = re.compile(r"^[0-9a-f]{24}$")
TONES = frozenset({"action", "pending", "good", "neutral"})
SECTIONS = ("versions", "submissions", "downloads", "builds", "groups", "feedback", "reviews")
COUNT_KINDS = ("downloads", "redownloads", "updates")
MAX_ROWS = 20
MAX_ATTENTION = 10
MAX_ITEMS = 20
MAX_GROUP_NAMES = 50
MAX_COUNT = 10 ** 12
STAMP_RE = re.compile(r"^\d{4}-\d{2}-\d{2}(?:[T ][0-9:.]{2,15}(?:Z|[+-]\d{2}:?\d{2})?)?$")
MONTH_RE = re.compile(r"^\d{4}-(0[1-9]|1[0-2])$")
LINK_RE = re.compile(r"^https://testflight\.apple\.com/join/[A-Za-z0-9]{1,32}$")

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS app_store_status_snapshots (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        published_at INTEGER NOT NULL CHECK(published_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS app_store_status_screenshots (
        account_bi TEXT NOT NULL,
        id TEXT NOT NULL,
        stored_at INTEGER NOT NULL CHECK(stored_at >= 0),
        data TEXT NOT NULL,
        PRIMARY KEY(account_bi, id))""",
]

DATA_SCHEMA_DOWN = [
    "DROP TABLE IF EXISTS app_store_status_screenshots",
    "DROP TABLE IF EXISTS app_store_status_snapshots",
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


def _text(value, limit):
    if isinstance(value, (dict, list, tuple)):
        return ""
    return " ".join(str(value if value is not None else "").replace("\x00", "").split())[:limit]


def _integer(value, low=0, high=MAX_COUNT):
    if isinstance(value, bool):
        return low
    try:
        number = int(value)
    except (TypeError, ValueError):
        try:
            number = int(float(value))
        except (TypeError, ValueError, OverflowError):
            return low
    return max(low, min(high, number))


def _stamp(value):
    value = _text(value, 40)
    return value if STAMP_RE.fullmatch(value) else ""


def _dict(value):
    return value if isinstance(value, dict) else {}


def _list(value, limit):
    return [item for item in value[:limit]] if isinstance(value, list) else []


def _status(value):
    value = _dict(value)
    tone = _text(value.get("tone"), 16)
    state = _text(value.get("state"), 64)
    return {
        "state": state,
        "label": _text(value.get("label"), 80) or state or "Unknown",
        "tone": tone if tone in TONES else "neutral",
    }


def _counts(value):
    value = _dict(value)
    return {kind: _integer(value.get(kind)) for kind in COUNT_KINDS}


def _version(row):
    row = _dict(row)
    return {
        "version": _text(row.get("version"), 32),
        "build": _text(row.get("build"), 32),
        "releaseType": _text(row.get("releaseType"), 40),
        "createdAt": _stamp(row.get("createdAt")),
        "status": _status(row.get("status")),
    }


def _submission(row):
    row = _dict(row)
    return {
        "id": _text(row.get("id"), 64),
        "submittedAt": _stamp(row.get("submittedAt")),
        "status": _status(row.get("status")),
        "items": [{"name": _text(_dict(item).get("name"), 64),
                   "status": _status(_dict(item).get("status"))}
                  for item in _list(row.get("items"), MAX_ITEMS)],
    }


def _downloads(value):
    value = _dict(value)
    since = _text(value.get("since"), 7)
    recent = []
    for span in _list(value.get("recent"), 6):
        span = _dict(span)
        days = _integer(span.get("days"), 1, 3660)
        recent.append({"days": days, **_counts(span)})
    return {
        "available": bool(value.get("available")),
        "note": _text(value.get("note"), 300),
        "since": since if MONTH_RE.fullmatch(since) else "",
        "total": _counts(value.get("total")),
        "recent": recent,
    }


def _build(row):
    row = _dict(row)
    return {
        "version": _text(row.get("version"), 32),
        "build": _text(row.get("build"), 32),
        "uploadedAt": _stamp(row.get("uploadedAt")),
        "expired": bool(row.get("expired")),
        "processing": _status(row.get("processing")),
        "internal": _status(row.get("internal")),
        "external": _status(row.get("external")),
        "betaReview": _status(row.get("betaReview")),
        "groups": [name for name in (_text(item, 80) for item in
                                     _list(row.get("groups"), MAX_GROUP_NAMES)) if name],
    }


def _group(row):
    row = _dict(row)
    link = _text(row.get("publicLink"), 80)
    enabled = bool(row.get("publicLinkEnabled"))
    return {
        "name": _text(row.get("name"), 80),
        "internal": bool(row.get("internal")),
        "testers": _integer(row.get("testers"), 0, 100000),
        "allBuilds": bool(row.get("allBuilds")),
        "publicLinkEnabled": enabled,
        "publicLink": link if enabled and LINK_RE.fullmatch(link) else "",
        "publicLinkLimit": _integer(row.get("publicLinkLimit"), 0, 100000),
        "feedbackEnabled": bool(row.get("feedbackEnabled")),
    }


def _screenshot(row):
    row = _dict(row)
    shot_id = _text(row.get("id"), 24)
    if not SCREENSHOT_ID_RE.fullmatch(shot_id):
        return None
    return {
        "id": shot_id,
        "width": _integer(row.get("width"), 0, 20000),
        "height": _integer(row.get("height"), 0, 20000),
    }


def _feedback(row):
    row = _dict(row)
    kind = _text(row.get("kind"), 16)
    shots = [shot for shot in (_screenshot(item) for item in
                               _list(row.get("screenshots"), MAX_SCREENSHOTS)) if shot]
    return {
        "kind": kind if kind in ("crash", "screenshot") else "screenshot",
        "createdAt": _stamp(row.get("createdAt")),
        "from": _text(row.get("from"), 120) or "anonymous",
        "device": _text(row.get("device"), 80),
        "comment": _text(row.get("comment"), 300),
        "screenshots": shots,
    }


def screenshot_ids(snapshot):
    found = []
    for row in _dict(snapshot).get("feedback") or []:
        for shot in _dict(row).get("screenshots") or []:
            shot_id = _dict(shot).get("id")
            if shot_id and shot_id not in found:
                found.append(shot_id)
    return found


def clean_screenshot(data):
    data = _dict(data)
    mime = _text(data.get("mime"), 32).lower()
    if mime not in SCREENSHOT_MIMES:
        return None, "unsupported_image"
    encoded = data.get("data")
    if not isinstance(encoded, str) or not encoded:
        return None, "invalid_image"
    if len(encoded) > MAX_SCREENSHOT_BYTES * 4 // 3 + 4:
        return None, "image_too_large"
    try:
        raw = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError):
        return None, "invalid_image"
    if not raw.startswith(SCREENSHOT_MIMES[mime]):
        return None, "invalid_image"
    if len(raw) > MAX_SCREENSHOT_BYTES:
        return None, "image_too_large"
    return {
        "mime": mime,
        "width": _integer(data.get("width"), 0, 20000),
        "height": _integer(data.get("height"), 0, 20000),
        "data": base64.b64encode(raw).decode("ascii"),
    }, ""


def _review(row):
    row = _dict(row)
    return {
        "createdAt": _stamp(row.get("createdAt")),
        "rating": _integer(row.get("rating"), 0, 5),
        "title": _text(row.get("title"), 120),
        "territory": _text(row.get("territory"), 8),
        "body": _text(row.get("body"), 300),
    }


def clean_snapshot(data, now=0):
    data = _dict(data)
    app = _dict(data.get("app"))
    unavailable = _dict(data.get("unavailable"))
    generated = _integer(data.get("generatedAt"), 0, 10 ** 14)
    return {
        "format": FORMAT,
        "generatedAt": generated or int(now or 0),
        "app": {"id": _text(app.get("id"), 32), "bundleId": _text(app.get("bundleId"), 160)},
        "attention": [line for line in (_text(item, 300) for item in
                                        _list(data.get("attention"), MAX_ATTENTION)) if line],
        "versions": [_version(row) for row in _list(data.get("versions"), MAX_ROWS)],
        "submissions": [_submission(row) for row in _list(data.get("submissions"), MAX_ROWS)],
        "downloads": _downloads(data.get("downloads")),
        "builds": [_build(row) for row in _list(data.get("builds"), MAX_ROWS)],
        "groups": [_group(row) for row in _list(data.get("groups"), 200)],
        "feedback": [_feedback(row) for row in _list(data.get("feedback"), MAX_ROWS)],
        "reviews": [_review(row) for row in _list(data.get("reviews"), MAX_ROWS)],
        "unavailable": {section: _text(unavailable.get(section), 200)
                        for section in SECTIONS if _text(unavailable.get(section), 200)},
    }


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
    if parts == ("snapshot",):
        return ("PUT", "DELETE")
    if parts == ("export",):
        return ("GET",)
    if len(parts) == 2 and parts[0] == "screenshots" and SCREENSHOT_ID_RE.fullmatch(parts[1]):
        return ("GET", "PUT")
    return ()


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _text((record or {}).get("name"), 64).lower()


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


async def _row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT published_at,data FROM app_store_status_snapshots WHERE account_bi=?",
        account_bi)


async def _stored(runtime, account_bi):
    row = await _row(runtime, account_bi)
    if not row:
        return None, 0
    opened = await runtime.open(row.get("data"))
    if not isinstance(opened, dict):
        return None, 0
    return clean_snapshot(opened), _integer(row.get("published_at"), 0, 10 ** 14)


async def _publish(runtime, account_bi, actor, data):
    now = runtime.now()
    snapshot = clean_snapshot(data, now)
    await runtime.d1_run(
        "INSERT INTO app_store_status_snapshots(account_bi,account_name,published_at,data) "
        "VALUES(?,?,?,?) ON CONFLICT(account_bi) DO UPDATE SET "
        "account_name=excluded.account_name,published_at=excluded.published_at,"
        "data=excluded.data",
        account_bi, actor, now, await runtime.seal(snapshot),
    )
    wanted = screenshot_ids(snapshot)
    stored = await runtime.d1_all(
        "SELECT id FROM app_store_status_screenshots WHERE account_bi=?", account_bi)
    have = {str(row.get("id") or "") for row in stored or []}
    for old in sorted(have - set(wanted)):
        await runtime.d1_run(
            "DELETE FROM app_store_status_screenshots WHERE account_bi=? AND id=?",
            account_bi, old)
    await runtime.audit(actor, "app-store-status.publish", "app_store_status", actor)
    return _response(runtime, {
        "ok": True, "publishedAt": now,
        "missingScreenshots": [shot for shot in wanted if shot not in have],
    })


async def _store_screenshot(runtime, account_bi, actor, shot_id, data):
    snapshot, _published_at = await _stored(runtime, account_bi)
    if shot_id not in screenshot_ids(snapshot):
        return _response(runtime, {"error": "screenshot_not_in_snapshot"}, 409)
    image, error = clean_screenshot(data)
    if error:
        return _response(runtime, {"error": error}, 413 if error == "image_too_large" else 400)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO app_store_status_screenshots(account_bi,id,stored_at,data) "
        "VALUES(?,?,?,?) ON CONFLICT(account_bi,id) DO UPDATE SET "
        "stored_at=excluded.stored_at,data=excluded.data",
        account_bi, shot_id, now, await runtime.seal(image),
    )
    await runtime.audit(actor, "app-store-status.screenshot", "app_store_status", actor)
    return _response(runtime, {"ok": True, "id": shot_id, "storedAt": now})


async def _read_screenshot(runtime, account_bi, shot_id):
    row = await runtime.d1_first(
        "SELECT data FROM app_store_status_screenshots WHERE account_bi=? AND id=?",
        account_bi, shot_id)
    image = await runtime.open(row.get("data")) if row else None
    if not isinstance(image, dict):
        return _response(runtime, {"error": "screenshot_not_found"}, 404)
    image, error = clean_screenshot(image)
    if error:
        return _response(runtime, {"error": "screenshot_not_found"}, 404)
    return _response(runtime, {"ok": True, "id": shot_id, **image})


async def account_installed(runtime, account_bi):
    return bool(await _row(runtime, account_bi))


async def account_has_data(runtime, account_bi):
    return bool(await _row(runtime, account_bi))


async def export_account_data(runtime, account_bi, actor):
    snapshot, published_at = await _stored(runtime, account_bi)
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-app-store-status-export-v1",
        "account": actor,
        "exportedAt": runtime.now(),
        "publishedAt": published_at,
        "snapshot": snapshot,
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    await runtime.d1_run(
        "DELETE FROM app_store_status_snapshots WHERE account_bi=?", account_bi)
    await runtime.d1_run(
        "DELETE FROM app_store_status_screenshots WHERE account_bi=?", account_bi)


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
    if method == "PUT":
        limit = MAX_SCREENSHOT_BODY_BYTES if parts[:1] == ("screenshots",) else MAX_BODY_BYTES
        data, error = await runtime.json_body(limit)
        if error:
            return _response(runtime, {"error": error},
                             413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    await _ensure_tables(runtime)
    if parts[:1] == ("screenshots",):
        if method == "PUT":
            return await _store_screenshot(runtime, account_bi, actor, parts[1], data)
        return await _read_screenshot(runtime, account_bi, parts[1])
    if parts == ("snapshot",):
        if method == "DELETE":
            await purge_account_data(runtime, account_bi, actor)
            await runtime.audit(actor, "app-store-status.forget", "app_store_status", actor)
            return _response(runtime, {"ok": True, "published": False})
        if _text(data.get("format"), 64) != FORMAT:
            return _response(runtime, {"error": "unsupported_format"}, 400)
        return await _publish(runtime, account_bi, actor, data)
    if parts == ("export",):
        return _response(runtime, {
            "ok": True, "filename": "forkmesh-app-store-status-" + actor + ".json",
            "export": await export_account_data(runtime, account_bi, actor),
        })
    snapshot, published_at = await _stored(runtime, account_bi)
    return _response(runtime, {
        "ok": True, "actor": actor, "published": snapshot is not None,
        "publishedAt": published_at, "snapshot": snapshot,
    })
