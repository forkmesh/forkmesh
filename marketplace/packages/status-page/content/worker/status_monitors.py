import re
from urllib.parse import urlsplit


PREFIX = "/api/status-monitors"
PLUGIN_ID = "status-page"
REQUEST_PROOF = "forkmesh-status-monitors-request-v1"
MAX_BODY_BYTES = 8 * 1024
MAX_MONITORS = 20
MAX_NAME = 40
MAX_URL = 300
ID_RE = re.compile(r"^[0-9a-f]{32}$")
HOST_RE = re.compile(r"^[A-Za-z0-9.:\[\]-]{1,253}$")

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS status_monitor_lists (
        account_bi TEXT PRIMARY KEY,
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
]

DATA_SCHEMA_DOWN = [
    "DROP TABLE IF EXISTS status_monitor_lists",
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
    if parts[0] != "monitors":
        return ()
    if len(parts) == 1:
        return ("POST",)
    if len(parts) == 2:
        return ("PUT", "DELETE")
    return ()


def clean_name(value):
    if not isinstance(value, str):
        return ""
    name = " ".join(value.replace("\x00", "").split())
    return name if 1 <= len(name) <= MAX_NAME else ""


def clean_url(value):
    if not isinstance(value, str):
        return ""
    url = value.strip()
    if not url or len(url) > MAX_URL or any(ord(ch) < 33 or ord(ch) == 127 for ch in url):
        return ""
    try:
        parts = urlsplit(url)
        port = parts.port
    except ValueError:
        return ""
    if parts.scheme not in ("http", "https") or not parts.hostname:
        return ""
    if parts.username is not None or parts.password is not None or "@" in parts.netloc:
        return ""
    if not HOST_RE.fullmatch(parts.netloc) or (port is not None and not 0 < port < 65536):
        return ""
    return url


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    name = " ".join(str((record or {}).get("name") or "").split())[:64].lower()
    return str(account_bi or ""), name


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


def _project(monitor):
    return {"id": monitor["id"], "name": monitor["name"], "url": monitor["url"]}


async def load_monitors(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT data FROM status_monitor_lists WHERE account_bi=?", account_bi)
    if not row:
        return []
    opened = await runtime.open(row.get("data"))
    rows = opened.get("monitors") if isinstance(opened, dict) else None
    monitors = []
    for item in rows or []:
        if not isinstance(item, dict):
            continue
        monitor_id = str(item.get("id") or "")
        name = clean_name(item.get("name"))
        url = clean_url(item.get("url"))
        if ID_RE.fullmatch(monitor_id) and name and url:
            monitors.append({"id": monitor_id, "name": name, "url": url})
    return monitors[:MAX_MONITORS]


async def _save(runtime, account_bi, monitors):
    sealed = await runtime.seal({"monitors": monitors})
    await runtime.d1_run(
        "INSERT INTO status_monitor_lists(account_bi,updated_at,data) VALUES(?,?,?) "
        "ON CONFLICT(account_bi) DO UPDATE SET updated_at=excluded.updated_at,"
        "data=excluded.data",
        account_bi, runtime.now(), sealed,
    )


def _listing(monitors, installed=True):
    return {
        "ok": True,
        "installed": installed,
        "max": MAX_MONITORS,
        "monitors": [_project(monitor) for monitor in monitors],
    }


def _fields(data):
    name = clean_name(data.get("name"))
    if not name:
        return None, None, "invalid_name"
    url = clean_url(data.get("url"))
    if not url:
        return None, None, "invalid_url"
    return name, url, ""


async def _create(runtime, account_bi, actor, data):
    name, url, error = _fields(data)
    if error:
        return _response(runtime, {"error": error}, 400)
    monitors = await load_monitors(runtime, account_bi)
    if len(monitors) >= MAX_MONITORS:
        return _response(runtime, {"error": "too_many_monitors", "max": MAX_MONITORS}, 409)
    monitor = {"id": runtime.new_id(), "name": name, "url": url}
    monitors.append(monitor)
    await _save(runtime, account_bi, monitors)
    await runtime.audit(actor, "status-monitors.create", "status_monitor", actor)
    payload = _listing(monitors)
    payload["monitor"] = _project(monitor)
    return _response(runtime, payload)


async def _update(runtime, account_bi, actor, monitor_id, data):
    name, url, error = _fields(data)
    if error:
        return _response(runtime, {"error": error}, 400)
    monitors = await load_monitors(runtime, account_bi)
    for monitor in monitors:
        if monitor["id"] == monitor_id:
            monitor["name"] = name
            monitor["url"] = url
            await _save(runtime, account_bi, monitors)
            await runtime.audit(actor, "status-monitors.update", "status_monitor", actor)
            payload = _listing(monitors)
            payload["monitor"] = _project(monitor)
            return _response(runtime, payload)
    return _response(runtime, {"error": "monitor_not_found"}, 404)


async def _delete(runtime, account_bi, actor, monitor_id):
    monitors = await load_monitors(runtime, account_bi)
    kept = [monitor for monitor in monitors if monitor["id"] != monitor_id]
    if len(kept) == len(monitors):
        return _response(runtime, {"error": "monitor_not_found"}, 404)
    await _save(runtime, account_bi, kept)
    await runtime.audit(actor, "status-monitors.delete", "status_monitor", actor)
    return _response(runtime, _listing(kept))


async def account_installed(runtime, account_bi):
    return await runtime.account_plugin_installed(account_bi)


async def account_has_data(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT 1 AS one FROM status_monitor_lists WHERE account_bi=? LIMIT 1", account_bi)
    return bool(row)


async def purge_account_data(runtime, account_bi, account_name=""):
    await runtime.d1_run(
        "DELETE FROM status_monitor_lists WHERE account_bi=?", account_bi)


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
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    await _ensure_tables(runtime)
    installed = await runtime.account_plugin_installed(account_bi)
    if not parts:
        if not installed:
            return _response(runtime, _listing([], installed=False))
        return _response(runtime, _listing(await load_monitors(runtime, account_bi)))
    if not installed:
        return _response(runtime, {"error": "status_page_not_installed"}, 409)
    if len(parts) == 1:
        return await _create(runtime, account_bi, actor, data)
    if not ID_RE.fullmatch(parts[1]):
        return _response(runtime, {"error": "monitor_not_found"}, 404)
    if method == "DELETE":
        return await _delete(runtime, account_bi, actor, parts[1])
    return await _update(runtime, account_bi, actor, parts[1], data)
