from __future__ import annotations

import json
import re
from urllib.parse import quote

import instance_features


USER_AGENT = "ForkMesh-archive-sync/1.0 (+https://forkmesh.com/docs)"
S3_ORIGIN = "https://s3.us.archive.org"
METADATA_ORIGIN = "https://archive.org/metadata/"
DETAILS_ORIGIN = "https://archive.org/details/"
COLLECTION = "open_source_software"
REQUEST_TIMEOUT_SECONDS = 15
MAX_RESPONSE_CHARS = 2 * 1024 * 1024
MAX_HISTORY = 20
SYNC_COOLDOWN_MS = 10 * 60 * 1000
DAY_MS = 24 * 60 * 60 * 1000
INTERVALS = {
    "daily": {"label": "Every day", "ms": DAY_MS},
    "weekly": {"label": "Every week", "ms": 7 * DAY_MS},
    "monthly": {"label": "Every month", "ms": 30 * DAY_MS},
}
DEFAULT_INTERVAL = "weekly"

_IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{4,99}$")
_KEY_RE = re.compile(r"^[A-Za-z0-9]{8,64}$")
_FILE_RE = re.compile(r"[^A-Za-z0-9_.-]")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clean_identifier(value):
    raw = str(value or "").strip()
    for prefix in (DETAILS_ORIGIN, "http://archive.org/details/",
                   "archive.org/details/"):
        if raw.lower().startswith(prefix):
            raw = raw[len(prefix):].split("/", 1)[0].split("?", 1)[0]
            break
    if not _IDENTIFIER_RE.fullmatch(raw) or ".." in raw:
        return ""
    return raw


def clean_key(value):
    key = str(value or "").strip()
    return key if _KEY_RE.fullmatch(key) else ""


def clean_interval(value):
    value = str(value or "").strip().lower()
    return value if value in INTERVALS else ""


def secret_hint(secret):
    secret = str(secret or "")
    return ("…" + secret[-4:]) if len(secret) >= 8 else "…"


def bundle_name(repo):
    name = _FILE_RE.sub("-", str(repo or "")).strip(".-") or "repository"
    return name[:100] + ".bundle"


def upload_url(identifier, repo):
    return "%s/%s/%s" % (S3_ORIGIN, identifier, bundle_name(repo))


def item_url(identifier):
    return DETAILS_ORIGIN + identifier


def catalog():
    return {
        "intervals": [{"key": key, "label": spec["label"]}
                      for key, spec in INTERVALS.items()],
        "defaultInterval": DEFAULT_INTERVAL,
        "keysUrl": "https://archive.org/account/s3.php",
    }


def public_view(row, record):
    record = record if isinstance(record, dict) else {}
    status = record.get("status") if isinstance(
        record.get("status"), dict) else {}
    identifier = str(record.get("identifier") or "")
    return {
        "identifier": identifier,
        "itemUrl": item_url(identifier) if identifier else "",
        "file": bundle_name(record.get("repo")),
        "accessKey": str(record.get("accessKey") or ""),
        "secretHint": str(record.get("secretHint") or ""),
        "interval": clean_interval(record.get("interval"))
        or DEFAULT_INTERVAL,
        "active": int((row or {}).get("active") or 0) == 1,
        "createdAt": int((row or {}).get("created_at") or 0),
        "updatedAt": int((row or {}).get("updated_at") or 0),
        "requestedAt": int(record.get("requestedAt") or 0),
        "status": {
            "username": str(status.get("username") or ""),
            "verifiedAt": int(status.get("verifiedAt") or 0),
            "itemExists": bool(status.get("itemExists")),
            "error": str(status.get("error") or ""),
            "upload": dict(status.get("upload") or {}),
        },
        "history": list(record.get("history") or [])[:MAX_HISTORY],
    }


def _now():
    return int(Date.now())


async def _repo_bi(env, owner, repo):
    return await blind_index(env, str(owner).lower() + "/" + str(repo))


def _site_origin(env):
    base = str(getattr(env, "PUBLIC_BASE_URL", "") or "").strip().rstrip("/")
    return base if base.startswith("https://") else "https://forkmesh.com"


def _headers(record=None):
    headers = {"accept": "application/json",
               "user-agent": instance_features.user_agent(
                   USER_AGENT, "archive-sync")}
    if record:
        headers["authorization"] = "LOW %s:%s" % (
            record["accessKey"], record["secret"])
    return headers


async def _http(method, url, headers):
    init = {"method": method, "headers": headers, "redirect": "manual"}
    try:
        response = await js_fetch_with_timeout(
            url, init, REQUEST_TIMEOUT_SECONDS)
    except Exception:
        return 0, None
    if response is None:
        return 0, None
    status = int(getattr(response, "status", 0) or 0)
    try:
        text = str(await response.text() or "")
    except Exception:
        text = ""
    if len(text) > MAX_RESPONSE_CHARS:
        return 502, None
    try:
        data = json.loads(text) if text else None
    except ValueError:
        data = None
    return status, data


async def _verify(record):
    now = _now()
    out = {"verifiedAt": now, "username": "", "itemExists": False,
           "error": ""}
    status, data = await _http("GET", S3_ORIGIN + "/?check_auth=1",
                               _headers(record))
    if status != 200 or not isinstance(data, dict) or data.get("error"):
        message = str((data or {}).get("error") or "") if isinstance(
            data, dict) else ""
        out["error"] = ("The Internet Archive refused these keys: %s"
                        % message[:200]) if message else (
            "The Internet Archive refused these keys." if status in (
                401, 403) or isinstance(data, dict)
            else "The Internet Archive did not answer (HTTP %d)." % status)
        return out
    out["username"] = str(data.get("screenname") or data.get("username")
                          or "")[:100]
    status, item = await _http(
        "GET", METADATA_ORIGIN + quote(record["identifier"], safe=""),
        _headers())
    if status == 200 and isinstance(item, dict):
        out["itemExists"] = bool(item.get("metadata"))
    return out


async def _load(env, repo_bi):
    row = await d1_first(
        env,
        "SELECT repo_bi, active, created_at, updated_at, data FROM"
        " repo_archive_syncs WHERE repo_bi=?", repo_bi)
    if not row:
        return None, None
    record = await decrypt_row(env, row.get("data"))
    if not isinstance(record, dict) or not clean_identifier(
            record.get("identifier")):
        return row, None
    return row, record


async def _store(env, repo_bi, record, active=None):
    now = _now()
    encrypted = await encrypt_row(env, record)
    existing = await d1_first(
        env, "SELECT active FROM repo_archive_syncs WHERE repo_bi=?", repo_bi)
    if not existing:
        await d1_run(
            env,
            "INSERT INTO repo_archive_syncs (repo_bi, active, created_at,"
            " updated_at, data) VALUES (?,?,?,?,?)",
            repo_bi, 0 if active is False else 1, now, now, encrypted)
        return
    await d1_run(
        env,
        "UPDATE repo_archive_syncs SET active=?, updated_at=?, data=?"
        " WHERE repo_bi=?",
        int(existing.get("active") or 0) if active is None
        else (1 if active else 0), now, encrypted, repo_bi)


def _note_history(record, kind, ok, message):
    history = list(record.get("history") or [])
    history.insert(0, {"at": _now(), "kind": kind, "ok": bool(ok),
                       "message": str(message or "")[:300]})
    record["history"] = history[:MAX_HISTORY]


def _apply_verification(record, verification):
    status = dict(record.get("status") or {})
    status.update({key: verification.get(key) for key in (
        "username", "verifiedAt", "itemExists", "error")})
    record["status"] = status


async def sync_config(env, owner, name, aliases=()):
    try:
        if await _repo_is_private(env, owner, name):
            return None
        for candidate in (owner,) + tuple(aliases or ()):
            row, record = await _load(env, await _repo_bi(env, candidate,
                                                          name))
            if not record or int((row or {}).get("active") or 0) != 1:
                continue
            interval = clean_interval(record.get("interval")) \
                or DEFAULT_INTERVAL
            upload = dict((record.get("status") or {}).get("upload") or {})
            config = {
                "revision": str(record.get("revision") or "archive"),
                "url": upload_url(record["identifier"], name),
                "accessKey": record["accessKey"],
                "secret": record["secret"],
                "intervalSeconds": INTERVALS[interval]["ms"] // 1000,
                "collection": COLLECTION,
                "title": "%s/%s" % (candidate, name),
                "source": "%s/%s/%s" % (_site_origin(env), candidate, name),
            }
            if upload.get("ok") and upload.get("digest"):
                config["uploaded"] = {
                    "revision": str(upload.get("revision") or ""),
                    "digest": str(upload.get("digest") or ""),
                    "at": int(upload.get("at") or 0),
                }
            return config
    except Exception:
        return None
    return None


async def record_sync(env, owner, name, report, aliases=()):
    if not isinstance(report, dict):
        return False
    try:
        for candidate in (owner,) + tuple(aliases or ()):
            repo_bi = await _repo_bi(env, candidate, name)
            row, record = await _load(env, repo_bi)
            if not record:
                continue
            status = dict(record.get("status") or {})
            previous = dict(status.get("upload") or {})
            current = {
                "at": int(report.get("at") or 0),
                "ok": bool(report.get("ok")),
                "error": str(report.get("error") or "")[:300],
                "bytes": max(0, int(report.get("bytes") or 0)),
                "digest": str(report.get("digest") or "")[:64],
                "revision": str(report.get("revision") or "")[:80],
            }
            if previous == current:
                return False
            if not current["ok"] and previous.get("ok"):
                current["lastOkAt"] = int(previous.get("at") or 0)
            elif not current["ok"]:
                current["lastOkAt"] = int(previous.get("lastOkAt") or 0)
            status["upload"] = current
            record["status"] = status
            _note_history(
                record, "upload", current["ok"],
                "Uploaded %s (%d bytes)" % (bundle_name(name),
                                            current["bytes"])
                if current["ok"] else current["error"])
            await _store(env, repo_bi, record)
            return True
    except Exception:
        return False
    return False


async def _code_hosted(env, owner, repo):
    try:
        row = await d1_first(
            env, "SELECT id FROM hosted_repos WHERE key_bi=? AND"
            " deleted_at=0", await _repo_bi(env, owner, repo))
        return bool(row)
    except Exception:
        return False


async def _response(env, owner, repo, repo_bi, extra=None):
    row, record = await _load(env, repo_bi)
    body = {
        "ok": True,
        "archive": public_view(row, record) if record else None,
        "catalog": catalog(),
        "codeHosted": await _code_hosted(env, owner, repo),
        "private": bool(await _repo_is_private(env, owner, repo)),
    }
    if extra:
        body.update(extra)
    return json_response(body, cache_control="no-store")


async def repo_archive_sync_handler(env, request, owner, repo):
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    await ensure_schema(env)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    if not await _authorize_repo_owner_web(env, request, owner, repo, data):
        return json_response({"error": "not_authorized"}, status=403)
    action = clean_string(data.get("action", "get"), 16).lower()
    owner = str(owner or "").strip().lower()
    repo_bi = await _repo_bi(env, owner, repo)
    row, record = await _load(env, repo_bi)

    if action == "get":
        return await _response(env, owner, repo, repo_bi)

    if action == "save":
        if await _repo_is_private(env, owner, repo):
            return json_response({"error": "private_repository"},
                                 status=400)
        identifier = clean_identifier(data.get("identifier"))
        if not identifier:
            return json_response({"error": "invalid_identifier"},
                                 status=400)
        access_key = clean_key(data.get("accessKey"))
        if not access_key:
            return json_response({"error": "invalid_access_key"},
                                 status=400)
        previous = record or {}
        secret = clean_key(data.get("secret"))
        if not secret and str(data.get("secret") or "").strip():
            return json_response({"error": "invalid_secret"}, status=400)
        if not secret and previous.get("accessKey") == access_key:
            secret = previous.get("secret", "")
        if not secret:
            return json_response({"error": "secret_required"}, status=400)
        if "interval" in data and data.get("interval") not in (None, ""):
            interval = clean_interval(data.get("interval"))
            if not interval:
                return json_response({"error": "invalid_interval"},
                                     status=400)
        else:
            interval = clean_interval(previous.get("interval")) \
                or DEFAULT_INTERVAL
        same_item = previous.get("identifier") == identifier
        updated = {
            "identifier": identifier, "accessKey": access_key,
            "secret": secret, "secretHint": secret_hint(secret),
            "interval": interval, "owner": owner, "repo": repo,
            "createdBy": previous.get("createdBy") or clean_string(
                data.get("ownerAccount", ""), 64).lower(),
            "history": list(previous.get("history") or []),
            "status": dict(previous.get("status") or {})
            if same_item else {},
            "requestedAt": int(previous.get("requestedAt") or 0),
            "revision": "archive:%d" % _now(),
        }
        verification = await _verify(updated)
        if verification.get("error"):
            return json_response(
                {"error": "verification_failed",
                 "message": verification["error"]}, status=400)
        _apply_verification(updated, verification)
        _note_history(updated, "settings", True,
                      ("Connected to %s" % identifier) if not same_item
                      else "Settings saved")
        await _store(env, repo_bi, updated,
                     active=data.get("active", True) is not False)
        return await _response(env, owner, repo, repo_bi, {"saved": True})

    if not record:
        return json_response({"error": "not_configured"}, status=404)

    if action == "toggle":
        await _store(env, repo_bi, record,
                     active=data.get("active", True) is not False)
        return await _response(env, owner, repo, repo_bi)

    if action == "verify":
        _apply_verification(record, await _verify(record))
        await _store(env, repo_bi, record)
        return await _response(env, owner, repo, repo_bi)

    if action == "disconnect":
        await d1_run(env, "DELETE FROM repo_archive_syncs WHERE repo_bi=?",
                     repo_bi)
        return await _response(env, owner, repo, repo_bi,
                               {"disconnected": True})

    if action == "sync":
        if await _repo_is_private(env, owner, repo):
            return json_response({"error": "private_repository"},
                                 status=400)
        now = _now()
        if now - int(record.get("requestedAt") or 0) < SYNC_COOLDOWN_MS:
            return json_response({"error": "sync_too_soon"}, status=429)
        record["requestedAt"] = now
        record["revision"] = "archive:%d" % now
        _note_history(record, "sync", True, "Upload requested")
        await _store(env, repo_bi, record)
        return await _response(env, owner, repo, repo_bi,
                               {"requested": True})

    return json_response({"error": "bad_action"}, status=400)
