import base64
import re


MEDIA_PART_BYTES = 512 * 1024
MEDIA_PART_BODY_MAX_BYTES = 720 * 1024
MEDIA_CREATE_BODY_MAX_BYTES = 4 * 1024
MEDIA_KINDS = {
    "video": {"max_bytes": 12 * 1024 * 1024, "max_duration_ms": 61 * 1000},
    "voice": {"max_bytes": 4 * 1024 * 1024, "max_duration_ms": 121 * 1000},
}
MEDIA_SEAL_OVERHEAD_BYTES = 1024
MEDIA_TTL_MS = 7 * 24 * 60 * 60 * 1000
MEDIA_UPLOAD_TTL_MS = 60 * 60 * 1000
MEDIA_TOMBSTONE_TTL_MS = 30 * 24 * 60 * 60 * 1000
MEDIA_PENDING_MAX_COUNT = 20
MEDIA_PENDING_MAX_BYTES = 64 * 1024 * 1024
MEDIA_STATUS_IDS_MAX = 50
MEDIA_SWEEP_BATCH = 200
MEDIA_LIVE_STATES = ("uploading", "ready")
MEDIA_STATES = ("uploading", "ready", "opened", "expired", "withdrawn")
ID = r"[0-9a-f]{32}"
COLLECTION_RE = re.compile(
    r"^/api/chat/direct-messages/(" + ID + r")/media/?$")
ITEM_RE = re.compile(
    r"^/api/chat/direct-messages/(" + ID + r")/media/(" + ID + r")/?$")
PART_RE = re.compile(
    r"^/api/chat/direct-messages/(" + ID + r")/media/(" + ID
    + r")/parts/([0-9]{1,3})/?$")
ACTION_RE = re.compile(
    r"^/api/chat/direct-messages/(" + ID + r")/media/(" + ID
    + r")/(finish|consume)/?$")
ID_RE = re.compile(r"^" + ID + r"$")
PATH_RE = re.compile(r"^/api/chat/direct-messages/" + ID + r"/media(/|$)")


def matches(path):
    return bool(PATH_RE.match(str(path or "")))


def _response(runtime, data, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        data,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _error(runtime, code, status, allow=""):
    return _response(runtime, {"error": code}, status=status, allow=allow)


def _as_int(value, default=-1):
    if isinstance(value, bool):
        return default
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.isdigit():
        return int(value)
    return default


def _max_parts(kind):
    limit = MEDIA_KINDS[kind]["max_bytes"] + MEDIA_SEAL_OVERHEAD_BYTES
    return (limit + MEDIA_PART_BYTES - 1) // MEDIA_PART_BYTES


async def _conversation(runtime, account_bi, conversation_id):
    rows = await runtime.d1_all(
        "SELECT p.participant_bi,c.key_version "
        "FROM chat_direct_participants p "
        "JOIN chat_direct_conversations c "
        "ON c.conversation_id=p.conversation_id "
        "WHERE p.conversation_id=?",
        conversation_id,
    )
    members = [str(row.get("participant_bi") or "") for row in rows or []]
    if len(members) != 2 or account_bi not in members:
        return None
    other = members[1] if members[0] == account_bi else members[0]
    return {
        "other": other,
        "key_version": int((rows[0] or {}).get("key_version") or 1),
    }


async def _media_row(runtime, conversation_id, media_id):
    return await runtime.d1_first(
        "SELECT media_id,conversation_id,sender_bi,recipient_bi,state,size,"
        "parts,created_at,expires_at,opened_at,data "
        "FROM chat_direct_media WHERE media_id=? AND conversation_id=?",
        media_id,
        conversation_id,
    )


def _role(row, account_bi):
    if str(row.get("sender_bi") or "") == account_bi:
        return "sender"
    if str(row.get("recipient_bi") or "") == account_bi:
        return "recipient"
    return ""


def _status(row, account_bi, now):
    state = str(row.get("state") or "")
    expires_at = int(row.get("expires_at") or 0)
    if state in MEDIA_LIVE_STATES and expires_at and expires_at <= now:
        state = "expired"
    return {
        "id": str(row.get("media_id") or ""),
        "state": state,
        "role": _role(row, account_bi),
        "size": int(row.get("size") or 0),
        "parts": int(row.get("parts") or 0),
        "partBytes": MEDIA_PART_BYTES,
        "sentAt": int(row.get("created_at") or 0),
        "openedAt": int(row.get("opened_at") or 0),
        "expiresAt": expires_at,
    }


async def _delete_parts(runtime, media_id):
    await runtime.d1_run(
        "DELETE FROM chat_direct_media_parts WHERE media_id=?", media_id)


async def _announce(runtime, conversation_id, key_version, account_bi,
                    media_id, state):
    try:
        await runtime.announce_media(
            conversation_id, key_version, account_bi, media_id, state)
    except Exception:
        pass


async def _pending_usage(runtime, account_bi, now):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n,COALESCE(SUM(size),0) AS bytes "
        "FROM chat_direct_media WHERE sender_bi=? "
        "AND state IN ('uploading','ready') AND expires_at>?",
        account_bi,
        now,
    )
    return int((row or {}).get("n") or 0), int((row or {}).get("bytes") or 0)


async def _create(runtime, account_bi, conversation_id, info, data):
    kind = str((data or {}).get("kind") or "")
    if kind not in MEDIA_KINDS:
        return _error(runtime, "invalid_kind", 400)
    size = _as_int((data or {}).get("size"))
    parts = _as_int((data or {}).get("parts"))
    duration_ms = _as_int((data or {}).get("durationMs"), 0)
    limits = MEDIA_KINDS[kind]
    if size < 1 or size > limits["max_bytes"] + MEDIA_SEAL_OVERHEAD_BYTES:
        return _error(runtime, "media_too_large", 413)
    if parts != (size + MEDIA_PART_BYTES - 1) // MEDIA_PART_BYTES:
        return _error(runtime, "invalid_parts", 400)
    if duration_ms < 0 or duration_ms > limits["max_duration_ms"]:
        return _error(runtime, "media_too_long", 400)
    now = runtime.now()
    await sweep(runtime, now)
    count, pending_bytes = await _pending_usage(runtime, account_bi, now)
    if count >= MEDIA_PENDING_MAX_COUNT:
        return _error(runtime, "media_quota_count", 429)
    if pending_bytes + size > MEDIA_PENDING_MAX_BYTES:
        return _error(runtime, "media_quota_bytes", 429)
    media_id = runtime.new_id()
    sealed = await runtime.seal({"kind": kind, "durationMs": duration_ms})
    expires_at = now + MEDIA_TTL_MS
    await runtime.d1_run(
        "INSERT INTO chat_direct_media "
        "(media_id,conversation_id,sender_bi,recipient_bi,state,size,parts,"
        "created_at,expires_at,opened_at,data) "
        "VALUES (?,?,?,?,'uploading',?,?,?,?,0,?)",
        media_id,
        conversation_id,
        account_bi,
        info["other"],
        size,
        parts,
        now,
        expires_at,
        sealed,
    )
    row = {
        "media_id": media_id,
        "sender_bi": account_bi,
        "recipient_bi": info["other"],
        "state": "uploading",
        "size": size,
        "parts": parts,
        "created_at": now,
        "expires_at": expires_at,
        "opened_at": 0,
    }
    return _response(
        runtime, {"ok": True, "media": _status(row, account_bi, now)},
        status=201)


async def _status_list(runtime, account_bi, conversation_id):
    raw = str(runtime.query_params().get("ids") or "")
    ids = []
    for value in raw.split(","):
        value = value.strip().lower()
        if value and ID_RE.match(value) and value not in ids:
            ids.append(value)
    if len(ids) > MEDIA_STATUS_IDS_MAX:
        return _error(runtime, "too_many_ids", 400)
    if not ids:
        return _response(runtime, {"media": []})
    rows = await runtime.d1_all(
        "SELECT media_id,sender_bi,recipient_bi,state,size,parts,created_at,"
        "expires_at,opened_at FROM chat_direct_media "
        "WHERE conversation_id=? AND media_id IN ("
        + ",".join("?" * len(ids)) + ")",
        conversation_id,
        *ids,
    )
    now = runtime.now()
    return _response(runtime, {
        "media": [_status(row, account_bi, now) for row in rows or []],
    })


async def _upload_part(runtime, account_bi, row, index, data):
    if _role(row, account_bi) != "sender":
        return _error(runtime, "sender_only", 403)
    now = runtime.now()
    if str(row.get("state")) != "uploading" or \
            int(row.get("expires_at") or 0) <= now:
        return _error(runtime, "upload_closed", 409)
    parts = int(row.get("parts") or 0)
    if index >= parts:
        return _error(runtime, "invalid_part", 400)
    encoded = (data or {}).get("data")
    if not isinstance(encoded, str) or not encoded:
        return _error(runtime, "invalid_part", 400)
    try:
        raw = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError):
        return _error(runtime, "invalid_part", 400)
    size = int(row.get("size") or 0)
    expected = MEDIA_PART_BYTES if index < parts - 1 else (
        size - MEDIA_PART_BYTES * (parts - 1))
    if len(raw) != expected:
        return _error(runtime, "invalid_part_size", 400)
    sealed = await runtime.seal({"d": encoded})
    await runtime.d1_run(
        "INSERT INTO chat_direct_media_parts "
        "(media_id,part_index,size,data,created_at) VALUES (?,?,?,?,?) "
        "ON CONFLICT(media_id,part_index) DO UPDATE SET "
        "size=excluded.size,data=excluded.data,created_at=excluded.created_at",
        str(row.get("media_id")),
        index,
        len(raw),
        sealed,
        now,
    )
    return _response(runtime, {"ok": True, "part": index})


async def _finish(runtime, account_bi, row):
    if _role(row, account_bi) != "sender":
        return _error(runtime, "sender_only", 403)
    now = runtime.now()
    state = str(row.get("state") or "")
    if state == "ready":
        return _response(
            runtime, {"ok": True, "media": _status(row, account_bi, now)})
    if state != "uploading" or int(row.get("expires_at") or 0) <= now:
        return _error(runtime, "upload_closed", 409)
    media_id = str(row.get("media_id"))
    stored = await runtime.d1_first(
        "SELECT COUNT(*) AS n,COALESCE(SUM(size),0) AS bytes "
        "FROM chat_direct_media_parts WHERE media_id=?",
        media_id,
    )
    if (
        int((stored or {}).get("n") or 0) != int(row.get("parts") or 0)
        or int((stored or {}).get("bytes") or 0) != int(row.get("size") or 0)
    ):
        return _error(runtime, "upload_incomplete", 409)
    await runtime.d1_run(
        "UPDATE chat_direct_media SET state='ready' "
        "WHERE media_id=? AND state='uploading'",
        media_id,
    )
    row = dict(row)
    row["state"] = "ready"
    return _response(
        runtime, {"ok": True, "media": _status(row, account_bi, now)})


async def _download_part(runtime, account_bi, row, index):
    if _role(row, account_bi) != "recipient":
        return _error(runtime, "recipient_only", 403)
    now = runtime.now()
    status = _status(row, account_bi, now)
    if status["state"] != "ready":
        return _response(
            runtime, {"error": "media_gone", "state": status["state"]},
            status=410)
    if index >= int(row.get("parts") or 0):
        return _error(runtime, "invalid_part", 400)
    part = await runtime.d1_first(
        "SELECT data FROM chat_direct_media_parts "
        "WHERE media_id=? AND part_index=?",
        str(row.get("media_id")),
        index,
    )
    if not part:
        return _response(
            runtime, {"error": "media_gone", "state": "opened"}, status=410)
    opened = await runtime.open(part.get("data"))
    encoded = str((opened or {}).get("d") or "")
    if not encoded:
        return _error(runtime, "media_unreadable", 500)
    return _response(runtime, {
        "index": index,
        "parts": int(row.get("parts") or 0),
        "data": encoded,
    })


async def _consume(runtime, account_bi, row, info, conversation_id):
    if _role(row, account_bi) != "recipient":
        return _error(runtime, "recipient_only", 403)
    now = runtime.now()
    status = _status(row, account_bi, now)
    media_id = str(row.get("media_id"))
    if status["state"] == "opened":
        return _response(runtime, {"ok": True, "media": status})
    if status["state"] == "uploading":
        return _error(runtime, "media_not_ready", 409)
    if status["state"] != "ready":
        await _delete_parts(runtime, media_id)
        if str(row.get("state") or "") in MEDIA_LIVE_STATES:
            await runtime.d1_run(
                "UPDATE chat_direct_media SET state='expired' "
                "WHERE media_id=? AND state IN ('uploading','ready')",
                media_id,
            )
        return _response(
            runtime, {"error": "media_gone", "state": status["state"]},
            status=410)
    await _delete_parts(runtime, media_id)
    await runtime.d1_run(
        "UPDATE chat_direct_media SET state='opened',opened_at=? "
        "WHERE media_id=? AND state='ready'",
        now,
        media_id,
    )
    row = dict(row)
    row["state"] = "opened"
    row["opened_at"] = now
    await _announce(
        runtime, conversation_id, info["key_version"], account_bi,
        media_id, "opened")
    return _response(
        runtime, {"ok": True, "media": _status(row, account_bi, now)})


async def _withdraw(runtime, account_bi, row, info, conversation_id):
    if _role(row, account_bi) != "sender":
        return _error(runtime, "sender_only", 403)
    now = runtime.now()
    media_id = str(row.get("media_id"))
    await _delete_parts(runtime, media_id)
    status = _status(row, account_bi, now)
    if status["state"] in MEDIA_LIVE_STATES:
        await runtime.d1_run(
            "UPDATE chat_direct_media SET state='withdrawn' "
            "WHERE media_id=? AND state IN ('uploading','ready')",
            media_id,
        )
        row = dict(row)
        row["state"] = "withdrawn"
        await _announce(
            runtime, conversation_id, info["key_version"], account_bi,
            media_id, "withdrawn")
    return _response(
        runtime, {"ok": True, "media": _status(row, account_bi, now)})


async def sweep(runtime, now=None):
    now = int(runtime.now() if now is None else now)
    rows = await runtime.d1_all(
        "SELECT m.media_id,m.conversation_id,m.state,c.key_version "
        "FROM chat_direct_media m "
        "LEFT JOIN chat_direct_conversations c "
        "ON c.conversation_id=m.conversation_id "
        "WHERE m.state IN ('uploading','ready') AND "
        "(m.expires_at<=? OR (m.state='uploading' AND m.created_at<=?)) "
        "ORDER BY m.expires_at LIMIT ?",
        now,
        now - MEDIA_UPLOAD_TTL_MS,
        MEDIA_SWEEP_BATCH,
    )
    expired = 0
    for row in rows or []:
        media_id = str(row.get("media_id") or "")
        await _delete_parts(runtime, media_id)
        await runtime.d1_run(
            "UPDATE chat_direct_media SET state='expired' "
            "WHERE media_id=? AND state IN ('uploading','ready')",
            media_id,
        )
        expired += 1
        if str(row.get("state")) == "ready" and row.get("key_version"):
            await _announce(
                runtime, str(row.get("conversation_id") or ""),
                int(row.get("key_version") or 1), "", media_id, "expired")
    await runtime.d1_run(
        "DELETE FROM chat_direct_media_parts WHERE NOT EXISTS ("
        "SELECT 1 FROM chat_direct_media m "
        "WHERE m.media_id=chat_direct_media_parts.media_id "
        "AND m.state IN ('uploading','ready'))",
    )
    await runtime.d1_run(
        "DELETE FROM chat_direct_media WHERE created_at<=? "
        "AND state NOT IN ('uploading','ready')",
        now - MEDIA_TOMBSTONE_TTL_MS,
    )
    return expired


async def delete_conversation(runtime, conversation_id):
    await runtime.d1_run(
        "DELETE FROM chat_direct_media_parts WHERE media_id IN ("
        "SELECT media_id FROM chat_direct_media WHERE conversation_id=?)",
        conversation_id,
    )
    await runtime.d1_run(
        "DELETE FROM chat_direct_media WHERE conversation_id=?",
        conversation_id,
    )


async def handle(runtime, path):
    await runtime.ensure_schema()
    path = str(path or "")
    collection = COLLECTION_RE.fullmatch(path)
    item = ITEM_RE.fullmatch(path)
    part = PART_RE.fullmatch(path)
    action = ACTION_RE.fullmatch(path)
    match = collection or item or part or action
    if not match:
        return _error(runtime, "not_found", 404)
    if collection:
        allowed = "GET, POST"
    elif item:
        allowed = "GET, DELETE"
    elif part:
        allowed = "GET, PUT"
    else:
        allowed = "POST"
    method = runtime.method()
    if method not in {value.strip() for value in allowed.split(",")}:
        return _error(runtime, "method_not_allowed", 405, allow=allowed)

    data = None
    if method in ("POST", "PUT"):
        limit = (
            MEDIA_PART_BODY_MAX_BYTES if part else MEDIA_CREATE_BODY_MAX_BYTES)
        data, error = await runtime.json_body(limit)
        if error:
            return _error(
                runtime, error,
                413 if error == "payload_too_large" else 400)
    account_bi, account = await runtime.session(data)
    if not account_bi or not account:
        return _error(runtime, "invalid_session", 401)

    conversation_id = match.group(1)
    info = await _conversation(runtime, account_bi, conversation_id)
    if not info:
        return _error(runtime, "not_found", 404)

    if collection:
        if method == "POST":
            return await _create(
                runtime, account_bi, conversation_id, info, data)
        return await _status_list(runtime, account_bi, conversation_id)

    row = await _media_row(runtime, conversation_id, match.group(2))
    if not row or not _role(row, account_bi):
        return _error(runtime, "not_found", 404)
    if item and method == "GET":
        return _response(
            runtime, {"media": _status(row, account_bi, runtime.now())})
    if item:
        return await _withdraw(
            runtime, account_bi, row, info, conversation_id)
    if part:
        index = int(match.group(3))
        if method == "PUT":
            return await _upload_part(runtime, account_bi, row, index, data)
        return await _download_part(runtime, account_bi, row, index)
    if match.group(3) == "finish":
        return await _finish(runtime, account_bi, row)
    return await _consume(runtime, account_bi, row, info, conversation_id)
