import base64
import re


MEDIA_PART_BYTES = 512 * 1024
MEDIA_PART_BODY_MAX_BYTES = 720 * 1024
MEDIA_CREATE_BODY_MAX_BYTES = 4 * 1024
MEDIA_MAX_BYTES = 25 * 1024 * 1024
MEDIA_MAX_PARTS = (MEDIA_MAX_BYTES + MEDIA_PART_BYTES - 1) // MEDIA_PART_BYTES
MEDIA_RANGE_MAX_BYTES = 4 * 1024 * 1024
MEDIA_DAY_MS = 24 * 60 * 60 * 1000
MEDIA_DAILY_MAX_COUNT = 20
MEDIA_DAILY_MAX_BYTES = 200 * 1024 * 1024
MEDIA_UPLOAD_TTL_MS = 60 * 60 * 1000
MEDIA_SWEEP_BATCH = 200
MEDIA_NAME_MAX = 120
MEDIA_TYPES = {
    "video/mp4": "mp4",
    "video/webm": "webm",
    "video/quicktime": "mov",
}
MEDIA_EXTENSION_TYPES = {
    "mp4": "video/mp4",
    "m4v": "video/mp4",
    "webm": "video/webm",
    "mov": "video/quicktime",
}
QUICKTIME_BOXES = (b"ftyp", b"moov", b"mdat", b"wide", b"free", b"skip")
ID = r"[0-9a-f]{32}"
REPO = r"^/api/repo/([^/]+)/([^/]+)/media"
COLLECTION_RE = re.compile(REPO + r"/?$")
ITEM_RE = re.compile(REPO + r"/(" + ID + r")(?:\.(mp4|m4v|webm|mov))?$")
PART_RE = re.compile(REPO + r"/(" + ID + r")/parts/([0-9]{1,3})/?$")
FINISH_RE = re.compile(REPO + r"/(" + ID + r")/finish/?$")
PATH_RE = re.compile(REPO + r"(/|$)")
RANGE_RE = re.compile(r"^bytes=([0-9]*)-([0-9]*)$")
NAME_UNSAFE_RE = re.compile(r"[\x00-\x1f\x7f\[\]()<>\\/\"]")


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


def clean_name(value, mime):
    name = NAME_UNSAFE_RE.sub("_", str(value or "")).strip()[:MEDIA_NAME_MAX]
    return name or "video." + MEDIA_TYPES.get(mime, "mp4")


def sniff_matches(mime, head):
    head = bytes(head or b"")
    if mime == "video/webm":
        return head[:4] == b"\x1a\x45\xdf\xa3"
    if mime == "video/mp4":
        return head[4:8] == b"ftyp"
    if mime == "video/quicktime":
        return head[4:8] in QUICKTIME_BOXES
    return False


def media_url(owner, repo, media_id, mime):
    return "/api/repo/%s/%s/media/%s.%s" % (
        owner, repo, media_id, MEDIA_TYPES.get(mime, "mp4"))


def byte_range(header, size):
    value = str(header or "").strip()
    match = RANGE_RE.match(value)
    if size <= 0 or not match or not (match.group(1) or match.group(2)):
        return "full", 0, max(size - 1, 0)
    if match.group(1):
        start = int(match.group(1))
        if start >= size:
            return "unsatisfiable", 0, 0
        end = int(match.group(2)) if match.group(2) else size - 1
        if end < start:
            return "full", 0, size - 1
    else:
        suffix = int(match.group(2))
        if suffix <= 0:
            return "unsatisfiable", 0, 0
        start = max(0, size - suffix)
        end = size - 1
    if start >= size:
        return "unsatisfiable", 0, 0
    end = min(end, size - 1, start + MEDIA_RANGE_MAX_BYTES - 1)
    return "partial", start, end


async def _row(runtime, media_id):
    return await runtime.d1_first(
        "SELECT media_id,repo_bi,uploader_bi,state,mime,size,parts,"
        "created_at,data FROM repo_uploads WHERE media_id=?",
        media_id,
    )


def _status(row, owner, repo):
    mime = str(row.get("mime") or "")
    media_id = str(row.get("media_id") or "")
    return {
        "id": media_id,
        "state": str(row.get("state") or ""),
        "mime": mime,
        "size": int(row.get("size") or 0),
        "parts": int(row.get("parts") or 0),
        "partBytes": MEDIA_PART_BYTES,
        "url": media_url(owner, repo, media_id, mime),
    }


async def _daily_usage(runtime, uploader_bi, now):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n,COALESCE(SUM(size),0) AS bytes "
        "FROM repo_uploads WHERE uploader_bi=? AND created_at>?",
        uploader_bi,
        now - MEDIA_DAY_MS,
    )
    return int((row or {}).get("n") or 0), int((row or {}).get("bytes") or 0)


async def _create(runtime, account_bi, record, owner, repo, data):
    mime = str((data or {}).get("mime") or "").strip().lower()
    if mime not in MEDIA_TYPES:
        return _error(runtime, "unsupported_media_type", 415)
    size = _as_int((data or {}).get("size"))
    parts = _as_int((data or {}).get("parts"))
    if size < 1 or size > MEDIA_MAX_BYTES:
        return _error(runtime, "media_too_large", 413)
    if parts != (size + MEDIA_PART_BYTES - 1) // MEDIA_PART_BYTES:
        return _error(runtime, "invalid_parts", 400)
    if record.get("email_verified") is not True:
        return _error(runtime, "email_unverified", 403)
    repo_bi = await runtime.public_repository(owner, repo)
    if not repo_bi:
        return _error(runtime, "repository_unavailable", 404)
    now = runtime.now()
    await sweep(runtime, now)
    count, used = await _daily_usage(runtime, account_bi, now)
    if count >= MEDIA_DAILY_MAX_COUNT:
        return _error(runtime, "media_quota_count", 429)
    if used + size > MEDIA_DAILY_MAX_BYTES:
        return _error(runtime, "media_quota_bytes", 429)
    media_id = runtime.new_id()
    sealed = await runtime.seal({
        "owner": owner,
        "repo": repo,
        "uploader": str(record.get("name") or ""),
        "name": clean_name((data or {}).get("name"), mime),
    })
    await runtime.d1_run(
        "INSERT INTO repo_uploads "
        "(media_id,repo_bi,uploader_bi,state,mime,size,parts,created_at,"
        "ready_at,data) VALUES (?,?,?,'uploading',?,?,?,?,0,?)",
        media_id,
        repo_bi,
        account_bi,
        mime,
        size,
        parts,
        now,
        sealed,
    )
    row = {"media_id": media_id, "state": "uploading", "mime": mime,
           "size": size, "parts": parts}
    return _response(
        runtime, {"ok": True, "media": _status(row, owner, repo)}, status=201)


def _uploading(row, now):
    return (
        str(row.get("state") or "") == "uploading"
        and int(row.get("created_at") or 0) + MEDIA_UPLOAD_TTL_MS > now
    )


async def _upload_part(runtime, account_bi, row, index, data):
    if str(row.get("uploader_bi") or "") != account_bi:
        return _error(runtime, "uploader_only", 403)
    if not _uploading(row, runtime.now()):
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
    if index == 0 and not sniff_matches(str(row.get("mime") or ""), raw[:16]):
        return _error(runtime, "media_type_mismatch", 415)
    await runtime.d1_run(
        "INSERT INTO repo_upload_parts (media_id,part_index,size,data) "
        "VALUES (?,?,?,?) ON CONFLICT(media_id,part_index) DO UPDATE SET "
        "size=excluded.size,data=excluded.data",
        str(row.get("media_id")),
        index,
        len(raw),
        base64.b64encode(raw).decode("ascii"),
    )
    return _response(runtime, {"ok": True, "part": index})


async def _finish(runtime, account_bi, row, owner, repo):
    if str(row.get("uploader_bi") or "") != account_bi:
        return _error(runtime, "uploader_only", 403)
    if str(row.get("state") or "") == "ready":
        return _response(runtime, {"ok": True, "media": _status(row, owner, repo)})
    now = runtime.now()
    if not _uploading(row, now):
        return _error(runtime, "upload_closed", 409)
    media_id = str(row.get("media_id"))
    stored = await runtime.d1_first(
        "SELECT COUNT(*) AS n,COALESCE(SUM(size),0) AS bytes "
        "FROM repo_upload_parts WHERE media_id=?",
        media_id,
    )
    if (
        int((stored or {}).get("n") or 0) != int(row.get("parts") or 0)
        or int((stored or {}).get("bytes") or 0) != int(row.get("size") or 0)
    ):
        return _error(runtime, "upload_incomplete", 409)
    await runtime.d1_run(
        "UPDATE repo_uploads SET state='ready',ready_at=? "
        "WHERE media_id=? AND state='uploading'",
        now,
        media_id,
    )
    row = dict(row)
    row["state"] = "ready"
    return _response(runtime, {"ok": True, "media": _status(row, owner, repo)})


async def _remove(runtime, account_bi, row, owner, repo):
    if str(row.get("uploader_bi") or "") != account_bi:
        return _error(runtime, "uploader_only", 403)
    media_id = str(row.get("media_id"))
    await runtime.d1_run(
        "DELETE FROM repo_upload_parts WHERE media_id=?", media_id)
    await runtime.d1_run(
        "UPDATE repo_uploads SET state='removed' WHERE media_id=?", media_id)
    row = dict(row)
    row["state"] = "removed"
    return _response(runtime, {"ok": True, "media": _status(row, owner, repo)})


async def _read_bytes(runtime, media_id, start, end):
    first = start // MEDIA_PART_BYTES
    last = end // MEDIA_PART_BYTES
    rows = await runtime.d1_all(
        "SELECT part_index,data FROM repo_upload_parts "
        "WHERE media_id=? AND part_index>=? AND part_index<=? "
        "ORDER BY part_index",
        media_id,
        first,
        last,
    )
    if len(rows or []) != last - first + 1:
        return None
    raw = bytearray()
    for expected, part in enumerate(rows, start=first):
        if _as_int(part.get("part_index")) != expected:
            return None
        raw.extend(base64.b64decode(str(part.get("data") or "")))
    offset = start - first * MEDIA_PART_BYTES
    return bytes(raw[offset:offset + end - start + 1])


def _media_headers(row, media_id, extension):
    return {
        "content-type": str(row.get("mime") or "application/octet-stream"),
        "content-disposition": 'inline; filename="%s.%s"' % (
            media_id, extension),
        "cache-control": "public, max-age=31536000, immutable",
        "etag": '"media-%s"' % media_id,
        "accept-ranges": "bytes",
        "x-content-type-options": "nosniff",
        "content-security-policy": "default-src 'none'; sandbox",
        "cross-origin-resource-policy": "same-site",
        "referrer-policy": "no-referrer",
    }


async def _serve(runtime, row):
    if str(row.get("state") or "") != "ready":
        return _error(runtime, "not_found", 404)
    opened = await runtime.open(row.get("data")) or {}
    if not await runtime.public_repository(
            str(opened.get("owner") or ""), str(opened.get("repo") or "")):
        return _error(runtime, "not_found", 404)
    media_id = str(row.get("media_id"))
    size = int(row.get("size") or 0)
    headers = _media_headers(
        row, media_id, MEDIA_TYPES.get(str(row.get("mime") or ""), "mp4"))
    kind, start, end = byte_range(runtime.header("range"), size)
    if kind == "unsatisfiable":
        headers["content-range"] = "bytes */%d" % size
        return runtime.binary(b"", 416, headers)
    head_only = runtime.method() == "HEAD"
    body = b"" if head_only else await _read_bytes(runtime, media_id, start, end)
    if body is None:
        return _error(runtime, "media_unreadable", 500)
    headers["content-length"] = str(end - start + 1)
    if kind == "partial":
        headers["content-range"] = "bytes %d-%d/%d" % (start, end, size)
        return runtime.binary(body, 206, headers)
    return runtime.binary(body, 200, headers)


async def sweep(runtime, now=None):
    now = int(runtime.now() if now is None else now)
    rows = await runtime.d1_all(
        "SELECT media_id FROM repo_uploads WHERE state='uploading' "
        "AND created_at<=? LIMIT ?",
        now - MEDIA_UPLOAD_TTL_MS,
        MEDIA_SWEEP_BATCH,
    )
    for row in rows or []:
        media_id = str(row.get("media_id") or "")
        await runtime.d1_run(
            "DELETE FROM repo_upload_parts WHERE media_id=?", media_id)
        await runtime.d1_run(
            "UPDATE repo_uploads SET state='removed' "
            "WHERE media_id=? AND state='uploading'",
            media_id,
        )
    return len(rows or [])


async def handle(runtime, path):
    await runtime.ensure_schema()
    path = str(path or "")
    collection = COLLECTION_RE.fullmatch(path)
    item = ITEM_RE.fullmatch(path)
    part = PART_RE.fullmatch(path)
    finish = FINISH_RE.fullmatch(path)
    match = collection or item or part or finish
    if not match:
        return _error(runtime, "not_found", 404)
    owner = runtime.segment(match.group(1))
    repo = runtime.segment(match.group(2))
    if not owner or not repo:
        return _error(runtime, "not_found", 404)
    if collection or finish:
        allowed = "POST"
    elif part:
        allowed = "PUT"
    else:
        allowed = "GET, HEAD, DELETE"
    method = runtime.method()
    if method not in {value.strip() for value in allowed.split(",")}:
        return _error(runtime, "method_not_allowed", 405, allow=allowed)

    if item and method in ("GET", "HEAD"):
        row = await _row(runtime, match.group(3))
        if not row:
            return _error(runtime, "not_found", 404)
        return await _serve(runtime, row)

    data = None
    if method in ("POST", "PUT"):
        limit = MEDIA_PART_BODY_MAX_BYTES if part else MEDIA_CREATE_BODY_MAX_BYTES
        data, error = await runtime.json_body(limit)
        if error:
            return _error(
                runtime, error, 413 if error == "payload_too_large" else 400)
    account_bi, record = await runtime.session(data)
    if not account_bi or not record:
        return _error(runtime, "invalid_session", 401)
    if collection:
        return await _create(runtime, account_bi, record, owner, repo, data)
    row = await _row(runtime, match.group(3))
    if not row:
        return _error(runtime, "not_found", 404)
    if part:
        return await _upload_part(
            runtime, account_bi, row, int(match.group(4)), data)
    if finish:
        return await _finish(runtime, account_bi, row, owner, repo)
    return await _remove(runtime, account_bi, row, owner, repo)
