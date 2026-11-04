import re
from urllib.parse import quote


PREFIX = "/api/internet-archive"
PLUGIN_ID = "internet-archive"
REQUEST_PROOF = "forkmesh-internet-archive-request-v1"
USER_AGENT = "ForkMesh-archive-sync/1.0 (+https://forkmesh.com/docs)"
S3_ORIGIN = "https://s3.us.archive.org"
METADATA_ORIGIN = "https://archive.org/metadata/"
DETAILS_ORIGIN = "https://archive.org/details/"
DOWNLOAD_ORIGIN = "https://archive.org/download/"
COLLECTION = "open_source_software"
MAX_BODY_BYTES = 8 * 1024
MAX_HISTORY = 20
MAX_UPLOADS = 30
MAX_REFS = 100
MAX_LISTED = 500
SYNC_COOLDOWN_MS = 10 * 60 * 1000
WITNESS_CACHE_MS = 2 * 60 * 1000
WITNESS_CACHE_SIZE = 256
INGEST_GRACE_MS = 6 * 60 * 60 * 1000
DAY_MS = 24 * 60 * 60 * 1000
INTERVALS = {
    "daily": {"label": "Every day", "ms": DAY_MS},
    "weekly": {"label": "Every week", "ms": 7 * DAY_MS},
    "monthly": {"label": "Every month", "ms": 30 * DAY_MS},
}
DEFAULT_INTERVAL = "weekly"
AGREEING = frozenset({"match", "head"})

_IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{4,99}$")
_KEY_RE = re.compile(r"^[A-Za-z0-9]{8,64}$")
_FILE_RE = re.compile(r"[^A-Za-z0-9_.-]")
_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,99}$", re.I)
_OBJECT_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
_SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_REF_RE = re.compile(r"^refs/(?:heads|tags)/[\x21-\x7e]{1,250}$")
_WITNESS_CACHE = {}


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


def clean_name(value):
    value = str(value or "").strip()
    return value if _NAME_RE.fullmatch(value) and ".." not in value else ""


def clean_ref(value):
    if not isinstance(value, dict):
        return None
    name = str(value.get("name") or "")
    commit = str(value.get("commit") or "").lower()
    if not _REF_RE.fullmatch(name) or not _OBJECT_RE.fullmatch(commit):
        return None
    return {"name": name, "commit": commit}


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


def download_url(identifier, repo):
    return "%s%s/%s" % (DOWNLOAD_ORIGIN, identifier, bundle_name(repo))


def catalog():
    return {
        "intervals": [{"key": key, "label": spec["label"]}
                      for key, spec in INTERVALS.items()],
        "defaultInterval": DEFAULT_INTERVAL,
        "keysUrl": "https://archive.org/account/s3.php",
    }


def _upload_view(upload):
    upload = upload if isinstance(upload, dict) else {}
    view = {
        "at": int(upload.get("at") or 0),
        "ok": bool(upload.get("ok")),
        "error": str(upload.get("error") or ""),
        "bytes": max(0, int(upload.get("bytes") or 0)),
        "digest": str(upload.get("digest") or ""),
        "revision": str(upload.get("revision") or ""),
        "sha1": str(upload.get("sha1") or ""),
        "routingDigest": str(upload.get("routingDigest") or ""),
        "head": clean_ref(upload.get("head")),
        "refCount": max(0, int(upload.get("refCount") or 0)),
    }
    if upload.get("lastOkAt"):
        view["lastOkAt"] = int(upload.get("lastOkAt") or 0)
    return view


def public_view(row, record):
    record = record if isinstance(record, dict) else {}
    status = record.get("status") if isinstance(
        record.get("status"), dict) else {}
    identifier = str(record.get("identifier") or "")
    return {
        "owner": str(record.get("owner") or ""),
        "repo": str(record.get("repo") or ""),
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
            "upload": _upload_view(status.get("upload"))
            if status.get("upload") else {},
        },
        "history": list(record.get("history") or [])[:MAX_HISTORY],
        "uploads": [_upload_view(item) for item in
                    list(record.get("uploads") or [])[:MAX_UPLOADS]],
    }


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(payload, status=status,
                            cache_control="no-store, max-age=0, must-revalidate",
                            extra_headers=headers)


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ()
    if not clean.startswith(PREFIX + "/"):
        return None
    parts = tuple(part for part in clean[len(PREFIX) + 1:].split("/") if part)
    if any(len(part) > 100 for part in parts):
        return None
    return parts


def _allowed_methods(parts):
    if parts == ("backups",):
        return ("GET",)
    if len(parts) == 3 and parts[0] == "repo":
        return ("POST",)
    if len(parts) == 4 and parts[0] == "repo" and parts[3] == "witness":
        return ("GET",)
    return ()


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    name = " ".join(str((record or {}).get("name") or "").split())[:64].lower()
    return str(account_bi or ""), name


async def _repo_bi(runtime, owner, repo):
    return await runtime.blind(str(owner).lower() + "/" + str(repo))


def _headers(record=None):
    headers = {"accept": "application/json", "user-agent": USER_AGENT}
    if record:
        headers["authorization"] = "LOW %s:%s" % (
            record["accessKey"], record["secret"])
    return headers


async def _http(runtime, url, headers):
    result = await runtime.outbound_json(url, "GET", headers)
    if not isinstance(result, dict):
        return 0, None
    status = int(result.get("status") or 0)
    if not result.get("ok"):
        return status, None
    return status, result.get("data")


async def _verify(runtime, record):
    out = {"verifiedAt": runtime.now(), "username": "", "itemExists": False,
           "error": ""}
    status, data = await _http(runtime, S3_ORIGIN + "/?check_auth=1",
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
        runtime, METADATA_ORIGIN + quote(record["identifier"], safe=""),
        _headers())
    if status == 200 and isinstance(item, dict):
        out["itemExists"] = bool(item.get("metadata"))
    return out


async def _load(runtime, repo_bi):
    row = await runtime.d1_first(
        "SELECT repo_bi, active, created_at, updated_at, data FROM"
        " repo_archive_syncs WHERE repo_bi=?", repo_bi)
    if not row:
        return None, None
    record = await runtime.open(row.get("data"))
    if not isinstance(record, dict) or not clean_identifier(
            record.get("identifier")):
        return row, None
    return row, record


async def _store(runtime, repo_bi, record, active=None):
    now = runtime.now()
    sealed = await runtime.seal(record)
    existing = await runtime.d1_first(
        "SELECT active FROM repo_archive_syncs WHERE repo_bi=?", repo_bi)
    if not existing:
        await runtime.d1_run(
            "INSERT INTO repo_archive_syncs (repo_bi, active, created_at,"
            " updated_at, data) VALUES (?,?,?,?,?)",
            repo_bi, 0 if active is False else 1, now, now, sealed)
        return
    await runtime.d1_run(
        "UPDATE repo_archive_syncs SET active=?, updated_at=?, data=?"
        " WHERE repo_bi=?",
        int(existing.get("active") or 0) if active is None
        else (1 if active else 0), now, sealed, repo_bi)


def _note_history(runtime, record, kind, ok, message):
    history = list(record.get("history") or [])
    history.insert(0, {"at": runtime.now(), "kind": kind, "ok": bool(ok),
                       "message": str(message or "")[:300]})
    record["history"] = history[:MAX_HISTORY]


def _apply_verification(record, verification):
    status = dict(record.get("status") or {})
    status.update({key: verification.get(key) for key in (
        "username", "verifiedAt", "itemExists", "error")})
    record["status"] = status


def _clean_report(report):
    digest = str(report.get("digest") or "").lower()[:64]
    sha1 = str(report.get("sha1") or "").lower()
    routing = str(report.get("routingDigest") or "").lower()
    refs = [ref for ref in (clean_ref(item) for item in
                            list(report.get("refs") or [])[:MAX_REFS]) if ref]
    current = {
        "at": int(report.get("at") or 0),
        "ok": bool(report.get("ok")),
        "error": str(report.get("error") or "")[:300],
        "bytes": max(0, int(report.get("bytes") or 0)),
        "digest": digest,
        "revision": str(report.get("revision") or "")[:80],
    }
    if current["ok"]:
        current.update({
            "sha1": sha1 if _SHA1_RE.fullmatch(sha1) else "",
            "routingDigest": routing if _SHA256_RE.fullmatch(routing) else "",
            "head": clean_ref(report.get("head")),
            "refCount": max(len(refs), min(100000, max(
                0, int(report.get("refCount") or 0)))),
        })
    return current, refs


async def archive_upload_keys(runtime):
    try:
        rows = await runtime.d1_all(
            "SELECT repo_bi FROM repo_archive_syncs WHERE active=1")
    except Exception:
        return set()
    return {str(row.get("repo_bi") or "") for row in rows or []}


async def archive_upload_config(runtime, owner, name, aliases=(), keys=None):
    try:
        if (await runtime.repository_facts(owner, name)).get("private"):
            return None
        for candidate in (owner,) + tuple(aliases or ()):
            repo_bi = await _repo_bi(runtime, candidate, name)
            if keys is not None and repo_bi not in keys:
                continue
            row, record = await _load(runtime, repo_bi)
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
                "source": "%s/%s/%s" % (runtime.public_origin().rstrip("/"),
                                        candidate, name),
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


async def record_archive_upload(runtime, owner, name, aliases=(), report=None):
    if not isinstance(report, dict):
        return False
    try:
        for candidate in (owner,) + tuple(aliases or ()):
            repo_bi = await _repo_bi(runtime, candidate, name)
            row, record = await _load(runtime, repo_bi)
            if not record:
                continue
            status = dict(record.get("status") or {})
            previous = dict(status.get("upload") or {})
            previous.pop("lastOkAt", None)
            current, refs = _clean_report(report)
            if previous == current:
                return False
            last = dict(status.get("upload") or {})
            same_event = int(last.get("at") or 0) == current["at"]
            if not current["ok"] and last.get("ok"):
                current["lastOkAt"] = int(last.get("at") or 0)
            elif not current["ok"]:
                current["lastOkAt"] = int(last.get("lastOkAt") or 0)
            status["upload"] = current
            record["status"] = status
            if current["ok"]:
                record["refs"] = refs
            uploads = [item for item in list(record.get("uploads") or [])
                       if int((item or {}).get("at") or 0) != current["at"]]
            uploads.insert(0, {key: value for key, value in current.items()
                               if key != "lastOkAt"})
            record["uploads"] = uploads[:MAX_UPLOADS]
            if not same_event:
                _note_history(
                    runtime, record, "upload", current["ok"],
                    "Uploaded %s (%d bytes)" % (bundle_name(name),
                                                current["bytes"])
                    if current["ok"] else current["error"])
            await _store(runtime, repo_bi, record)
            return True
    except Exception:
        return False
    return False


async def purge_account_data(runtime, account_bi, account_name=""):
    name = str(account_name or "").strip().lower()
    opener = getattr(runtime, "open", None)
    if not name or opener is None:
        return
    rows = await runtime.d1_all(
        "SELECT repo_bi, data FROM repo_archive_syncs LIMIT ?", MAX_LISTED)
    for row in rows or []:
        record = await opener(row.get("data"))
        if not isinstance(record, dict):
            continue
        if name in (str(record.get("createdBy") or "").lower(),
                    str(record.get("owner") or "").lower()):
            await runtime.d1_run(
                "DELETE FROM repo_archive_syncs WHERE repo_bi=?",
                row.get("repo_bi"))


async def _backups(runtime, actor):
    rows = await runtime.d1_all(
        "SELECT repo_bi, active, created_at, updated_at, data FROM"
        " repo_archive_syncs ORDER BY updated_at DESC LIMIT ?", MAX_LISTED)
    listed = []
    for row in rows or []:
        record = await runtime.open(row.get("data"))
        if not isinstance(record, dict) or not clean_identifier(
                record.get("identifier")):
            continue
        if actor not in (str(record.get("createdBy") or "").lower(),
                         str(record.get("owner") or "").lower()):
            continue
        view = public_view(row, record)
        listed.append({key: view[key] for key in (
            "owner", "repo", "identifier", "itemUrl", "active", "interval",
            "updatedAt")} | {"upload": view["status"]["upload"]})
    return _response(runtime, {"ok": True, "backups": listed,
                               "catalog": catalog()})


async def _settings_response(runtime, owner, repo, repo_bi, extra=None):
    row, record = await _load(runtime, repo_bi)
    facts = await runtime.repository_facts(owner, repo)
    body = {
        "ok": True,
        "owner": owner,
        "repo": repo,
        "archive": public_view(row, record) if record else None,
        "catalog": catalog(),
        "codeHosted": bool(facts.get("hosted")),
        "private": bool(facts.get("private")),
    }
    if extra:
        body.update(extra)
    return _response(runtime, body)


async def _settings(runtime, owner, repo, data, actor):
    action = str(data.get("action") or "get").strip().lower()[:16]
    repo_bi = await _repo_bi(runtime, owner, repo)
    row, record = await _load(runtime, repo_bi)

    if action == "get":
        return await _settings_response(runtime, owner, repo, repo_bi)

    if action == "save":
        if (await runtime.repository_facts(owner, repo)).get("private"):
            return _response(runtime, {"error": "private_repository"}, 400)
        identifier = clean_identifier(data.get("identifier"))
        if not identifier:
            return _response(runtime, {"error": "invalid_identifier"}, 400)
        access_key = clean_key(data.get("accessKey"))
        if not access_key:
            return _response(runtime, {"error": "invalid_access_key"}, 400)
        previous = record or {}
        secret = clean_key(data.get("secret"))
        if not secret and str(data.get("secret") or "").strip():
            return _response(runtime, {"error": "invalid_secret"}, 400)
        if not secret and previous.get("accessKey") == access_key:
            secret = previous.get("secret", "")
        if not secret:
            return _response(runtime, {"error": "secret_required"}, 400)
        if data.get("interval") not in (None, ""):
            interval = clean_interval(data.get("interval"))
            if not interval:
                return _response(runtime, {"error": "invalid_interval"}, 400)
        else:
            interval = clean_interval(previous.get("interval")) \
                or DEFAULT_INTERVAL
        same_item = previous.get("identifier") == identifier
        updated = {
            "identifier": identifier, "accessKey": access_key,
            "secret": secret, "secretHint": secret_hint(secret),
            "interval": interval, "owner": owner, "repo": repo,
            "createdBy": previous.get("createdBy") or actor,
            "history": list(previous.get("history") or []),
            "status": dict(previous.get("status") or {})
            if same_item else {},
            "uploads": list(previous.get("uploads") or [])
            if same_item else [],
            "refs": list(previous.get("refs") or []) if same_item else [],
            "requestedAt": int(previous.get("requestedAt") or 0),
            "revision": "archive:%d" % runtime.now(),
        }
        verification = await _verify(runtime, updated)
        if verification.get("error"):
            return _response(runtime, {"error": "verification_failed",
                                       "message": verification["error"]}, 400)
        _apply_verification(updated, verification)
        _note_history(runtime, updated, "settings", True,
                      ("Connected to %s" % identifier) if not same_item
                      else "Settings saved")
        await _store(runtime, repo_bi, updated,
                     active=data.get("active", True) is not False)
        return await _settings_response(runtime, owner, repo, repo_bi,
                                        {"saved": True})

    if not record:
        return _response(runtime, {"error": "not_configured"}, 404)

    if action == "toggle":
        await _store(runtime, repo_bi, record,
                     active=data.get("active", True) is not False)
        return await _settings_response(runtime, owner, repo, repo_bi)

    if action == "verify":
        _apply_verification(record, await _verify(runtime, record))
        await _store(runtime, repo_bi, record)
        return await _settings_response(runtime, owner, repo, repo_bi)

    if action == "disconnect":
        await runtime.d1_run(
            "DELETE FROM repo_archive_syncs WHERE repo_bi=?", repo_bi)
        _WITNESS_CACHE.pop((owner, repo), None)
        return await _settings_response(runtime, owner, repo, repo_bi,
                                        {"disconnected": True})

    if action == "sync":
        if (await runtime.repository_facts(owner, repo)).get("private"):
            return _response(runtime, {"error": "private_repository"}, 400)
        now = runtime.now()
        if now - int(record.get("requestedAt") or 0) < SYNC_COOLDOWN_MS:
            return _response(runtime, {"error": "sync_too_soon"}, 429)
        record["requestedAt"] = now
        record["revision"] = "archive:%d" % now
        _note_history(runtime, record, "sync", True, "Upload requested")
        await _store(runtime, repo_bi, record)
        return await _settings_response(runtime, owner, repo, repo_bi,
                                        {"requested": True})

    return _response(runtime, {"error": "bad_action"}, 400)


def archived_states(record):
    states = []
    uploads = list(record.get("uploads") or [])
    last = (record.get("status") or {}).get("upload")
    if isinstance(last, dict) and last.get("ok") and not any(
            int((item or {}).get("at") or 0) == int(last.get("at") or 0)
            for item in uploads):
        uploads.append(last)
    for upload in uploads:
        view = _upload_view(upload)
        if view["ok"]:
            states.append(view)
    states.sort(key=lambda item: -item["at"])
    return states


def witness_state(states, item, now):
    latest = states[0] if states else None
    if latest is None:
        return {"state": "none"}
    if item is None:
        return {"state": "unreachable"}
    files = item.get("files") if isinstance(item.get("files"), list) else []
    held = next((entry for entry in files if isinstance(entry, dict)
                 and entry.get("name") == latest.get("file")), None)
    fresh = now - latest["at"] < INGEST_GRACE_MS
    if held is None:
        return {"state": "pending" if fresh else "missing"}
    sha1 = str(held.get("sha1") or "").lower()
    file_view = {"sha1": sha1, "size": _int(held.get("size")),
                 "mtime": _int(held.get("mtime")) * 1000}
    if not latest["sha1"]:
        return {"state": "unrecorded", "file": file_view}
    if sha1 == latest["sha1"]:
        return {"state": "verified", "file": file_view, "at": latest["at"]}
    older = next((state for state in states[1:] if state["sha1"] == sha1),
                 None)
    if older is not None:
        return {"state": "pending" if fresh else "older", "file": file_view,
                "at": older["at"]}
    return {"state": "pending" if fresh else "changed", "file": file_view}


def _int(value):
    try:
        return max(0, int(float(value or 0)))
    except (TypeError, ValueError):
        return 0


def copy_verdict(states, copy):
    latest = states[0] if states else None
    digest = str(copy.get("directRefsSha256") or "").lower()
    commit = str(copy.get("commit") or "").lower()
    branch = str(copy.get("branch") or "")
    if latest is None:
        return {"verdict": "unknown", "reason": "no_archive"}
    if not digest and not commit:
        return {"verdict": "unknown", "reason": "no_refs"}

    def head_matches(state):
        head = state.get("head") or {}
        return bool(commit and head.get("commit") == commit and (
            not branch or head.get("name") == "refs/heads/" + branch))

    if digest and latest["routingDigest"] and digest == latest["routingDigest"]:
        return {"verdict": "match", "at": latest["at"]}
    if head_matches(latest):
        return {"verdict": "head", "at": latest["at"]}
    for state in states[1:]:
        if (digest and digest == state["routingDigest"]) or head_matches(state):
            return {"verdict": "earlier", "at": state["at"]}
    if int(copy.get("lastSync") or 0) > latest["at"]:
        return {"verdict": "newer", "at": latest["at"]}
    return {"verdict": "differs", "at": latest["at"]}


def _copy_view(states, copy, role):
    verdict = copy_verdict(states, copy)
    return {
        "role": role,
        "node": str(copy.get("node") or copy.get("machineName") or "")[:80],
        "status": "online" if copy.get("status") == "online" else "offline",
        "cloneAvailable": bool(copy.get("cloneAvailable")),
        "commit": str(copy.get("commit") or "")[:64],
        "branch": str(copy.get("branch") or "")[:120],
        "lastSync": int(copy.get("lastSync") or 0),
        "mirrorUrl": str(copy.get("mirrorUrl") or "")[:300],
        "refsDigest": str(copy.get("directRefsSha256") or "")[:64],
        "directVerified": bool(copy.get("directVerified")),
        **verdict,
    }


def compare_copies(states, mirrors):
    mirrors = mirrors if isinstance(mirrors, dict) else {}
    copies = []
    source = mirrors.get("sourceOfTruth")
    if isinstance(source, dict) and (source.get("node") or source.get("commit")):
        copies.append(_copy_view(states, source, "source"))
    seen = {copies[0]["node"]} if copies else set()
    for mirror in list(mirrors.get("mirrors") or [])[:64]:
        if not isinstance(mirror, dict):
            continue
        view = _copy_view(states, mirror, "mirror")
        if view["node"] in seen:
            continue
        seen.add(view["node"])
        copies.append(view)
    source_online = bool(copies and copies[0]["role"] == "source"
                         and copies[0]["status"] == "online")
    serving = [copy for copy in copies if copy["role"] == "mirror"
               and copy["status"] == "online"]
    counts = {}
    for copy in copies:
        counts[copy["verdict"]] = counts.get(copy["verdict"], 0) + 1
    return copies, {
        "sourceOnline": source_online,
        "sourceKnown": bool(copies and copies[0]["role"] == "source"),
        "copies": len(copies),
        "servingMirrors": len(serving),
        "agreeingMirrors": sum(1 for copy in serving
                               if copy["verdict"] in AGREEING),
        "verdicts": counts,
    }


async def _archive_item(runtime, identifier, now):
    cached = _WITNESS_CACHE.get(identifier)
    if cached and cached[0] > now:
        return cached[1]
    status, item = await _http(
        runtime, METADATA_ORIGIN + quote(identifier, safe=""), _headers())
    item = item if status == 200 and isinstance(item, dict) else None
    if len(_WITNESS_CACHE) >= WITNESS_CACHE_SIZE:
        _WITNESS_CACHE.clear()
    _WITNESS_CACHE[identifier] = (now + WITNESS_CACHE_MS, item)
    return item


async def _witness(runtime, owner, repo):
    facts = await runtime.repository_facts(owner, repo)
    if facts.get("private") or not facts.get("exists", True):
        return _response(runtime, {"error": "not_found"}, 404)
    repo_bi = await _repo_bi(runtime, owner, repo)
    row, record = await _load(runtime, repo_bi)
    if not record:
        return _response(runtime, {"ok": True, "owner": owner, "repo": repo,
                                   "archive": None})
    now = runtime.now()
    states = archived_states(record)
    for state in states:
        state["file"] = bundle_name(record.get("repo") or repo)
    item = await _archive_item(runtime, record["identifier"], now) \
        if states else {}
    witness = witness_state(states, item, now)
    mirrors = await runtime.repository_mirrors(owner, repo)
    copies, summary = compare_copies(states, mirrors)
    latest = states[0] if states else None
    return _response(runtime, {
        "ok": True,
        "owner": owner,
        "repo": repo,
        "checkedAt": now,
        "archive": {
            "identifier": record["identifier"],
            "itemUrl": item_url(record["identifier"]),
            "downloadUrl": download_url(record["identifier"],
                                        record.get("repo") or repo),
            "file": bundle_name(record.get("repo") or repo),
            "active": int((row or {}).get("active") or 0) == 1,
            "latest": latest,
            "refs": [ref for ref in (clean_ref(item) for item in
                                     list(record.get("refs") or [])) if ref],
            "uploads": [{key: state[key] for key in (
                "at", "bytes", "sha1", "routingDigest", "head", "refCount")}
                for state in states],
            "failures": [{"at": item["at"], "error": item["error"]}
                         for item in (_upload_view(entry) for entry in
                                      list(record.get("uploads") or []))
                         if not item["ok"]],
        },
        "witness": witness,
        "copies": copies,
        "summary": summary,
    })


async def handle(runtime, path):
    parts = _route(path)
    if parts is None:
        return _response(runtime, {"error": "not_found"}, 404)
    allowed = _allowed_methods(parts)
    if not allowed:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405,
                         ", ".join(allowed))
    if method != "GET" and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method != "GET":
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(runtime, {"error": error},
                             413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    if parts == ("backups",):
        return await _backups(runtime, actor)
    owner, repo = clean_name(parts[1]).lower(), clean_name(parts[2])
    if not owner or not repo:
        return _response(runtime, {"error": "not_found"}, 404)
    if len(parts) == 4:
        return await _witness(runtime, owner, repo)
    if not await runtime.account_plugin_installed(account_bi):
        return _response(runtime, {"error": "internet_archive_not_installed"},
                         409)
    if not await runtime.repository_administrator(owner, repo, data):
        return _response(runtime, {"error": "not_authorized"}, 403)
    return await _settings(runtime, owner, repo, data, actor)
