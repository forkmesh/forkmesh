"""In-app bug reports: bounded private writes, read from the admin console.

The mobile client carries a small always-available report control, so this is
where a report from a phone lands.  Loaded on demand (entry.py holds a
``_LazyModule`` proxy) so Cloudflare's startup-memory validation does not pay
for this domain on every isolate.

Three properties matter more here than on the rest of the API:

* **It has to work signed out.**  A crash in the login flow is exactly the
  report worth having, and it is the report an authenticated-only endpoint
  cannot receive.  A valid session is therefore used for *credit* when one is
  present and is never a precondition.  Since the reporter may be anonymous,
  the record deliberately carries no way to answer them unless they typed a
  contact themselves.
* **It must not become an open write.**  Anonymous POST plus D1 is a spam
  target, so every report passes a bounded body read, a per-reporter hourly
  quota, and hard length caps before anything is inserted.  The quota key is a
  blind index of the account when signed in and of the transient client
  address otherwise -- the raw address is never stored, exactly as in the
  signup anti-abuse path.
* **The report body is user copy.**  ``message`` and ``contact`` are whatever
  the reporter typed, which routinely includes an email address and a
  description of what they were doing.  Both live inside the encrypted ``data``
  blob.  Only operational columns a console lists or filters on -- platform,
  app version, source, status, timestamp -- stay in plaintext, so the report
  list sorts and paginates without decrypting anything.

Reports are read back only by platform administrators: the Report a bug
plugin lists them from ``GET /api/bug-reports/inbox`` behind a bearer session
and a live is_admin check, and they are triaged in the platform-admin
console's Bug reports section (admin_support.py).

Attachments are chunked into encrypted rows in ``bug_report_attachments``.
They never enter the repository issue inbox or a public API response.
"""

import base64
import re
from urllib.parse import parse_qs, urlparse


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


# One report is a bounded amount of prose plus a short contact string. The body
# limit is generous enough for a pasted stack trace and small enough that the
# encrypted row stays far inside D1's row ceiling.
MAX_BODY_BYTES = 12 * 1024 * 1024
MAX_MESSAGE = 4000
MAX_CONTACT = 200
MAX_SCREEN = 120
MAX_VERSION = 60
MAX_PLATFORM = 40
MAX_DEVICE = 120
# Below this a "report" is a stray tap, not something an operator can act on.
MIN_MESSAGE = 4
MAX_ATTACHMENTS = 5
MAX_ATTACHMENT_BYTES = 4 * 1024 * 1024
MAX_ATTACHMENT_BYTES_TOTAL = 8 * 1024 * 1024
ATTACHMENT_CHUNK_BYTES = 256 * 1024

# Per-reporter hourly quota. High enough that somebody genuinely hitting
# several bugs in one session is never turned away, low enough that a script
# cannot fill the table from one address.
RATE_LIMIT_WINDOW_MS = 60 * 60 * 1000
RATE_LIMIT_MAX = 10

# Reports the console has not been shown yet are what the badge counts, so the
# statuses are deliberately few and mean exactly one thing each.
STATUSES = ("new", "triaged", "closed")

INBOX_PAGE_LIMIT = 50

# Where the report came from. An unknown value is normalized rather than
# rejected: a future client should not have its reports dropped by an older
# Worker that has not heard of it yet.
SOURCES = ("mobile", "desktop", "web", "unknown")

# Anything the reporter can set is clamped to this severity vocabulary. It is
# a hint for triage order, never a promise about response time.
SEVERITIES = ("low", "normal", "high", "crash")

SEGMENT_RE = re.compile(r"[a-z0-9][a-z0-9._-]{0,79}")


def _text(value, maximum):
    return clean_string(value, maximum).strip()


def _package(value):
    if not isinstance(value, str):
        return ""
    candidate = _text(value, 81).lower()
    return candidate if SEGMENT_RE.fullmatch(candidate) else ""


def _choice(value, allowed, fallback):
    candidate = _text(value, 40).lower()
    return candidate if candidate in allowed else fallback


def _attachments(value):
    if value is None:
        return []
    if not isinstance(value, list) or len(value) > MAX_ATTACHMENTS:
        raise ValueError("attachment_invalid")
    result, total = [], 0
    for item in value:
        if not isinstance(item, dict):
            raise ValueError("attachment_invalid")
        name = _text(item.get("name"), 160) or "attachment"
        content_type = _text(item.get("contentType"), 120).lower()
        encoded = item.get("data")
        if not isinstance(encoded, str):
            raise ValueError("attachment_invalid")
        try:
            data = base64.b64decode(encoded, validate=True)
        except Exception as error:
            raise ValueError("attachment_invalid") from error
        size = len(data)
        total += size
        if not data or size > MAX_ATTACHMENT_BYTES or total > MAX_ATTACHMENT_BYTES_TOTAL:
            raise ValueError("attachment_too_large")
        result.append({"id": new_report_id(), "name": name,
                       "contentType": content_type, "size": size, "data": data})
    return result


def normalize_report(data):
    """Return the stored shape for one submitted report, or ``None``.

    Pure and side-effect free so the accepted/rejected boundary is testable
    without a Worker runtime.  ``None`` means the submission carried no
    actionable message and must not reach D1.
    """
    if not isinstance(data, dict):
        return None
    message = _text(data.get("message"), MAX_MESSAGE)
    if len(message) < MIN_MESSAGE:
        return None
    attachments = _attachments(data.get("attachments"))
    return {
        "message": message,
        "contact": _text(data.get("contact"), MAX_CONTACT),
        # Where in the app the reporter was standing. Supplied by the client,
        # so it is a routing hint and not evidence of anything.
        "screen": _text(data.get("screen"), MAX_SCREEN),
        "severity": _choice(data.get("severity"), SEVERITIES, "normal"),
        "source": _choice(data.get("source"), SOURCES, "unknown"),
        "platform": _text(data.get("platform"), MAX_PLATFORM),
        "appVersion": _text(data.get("appVersion"), MAX_VERSION),
        "osVersion": _text(data.get("osVersion"), MAX_VERSION),
        "device": _text(data.get("device"), MAX_DEVICE),
        "package": _package(data.get("package")),
        "attachments": [{key: item[key] for key in ("id", "name", "contentType", "size")}
                        for item in attachments],
    }


def new_report_id():
    """128 bits of randomness; carries no account, address or timestamp."""
    rnd = js_crypto.getRandomValues(Uint8Array.new(16))
    return "".join("%02x" % int(rnd[index]) for index in range(16))


async def _reporter_key(env, request, account):
    """Blind index of the quota subject: the account, else the address.

    The raw address is read transiently and never retained, so the stored key
    supports counting one reporter's recent submissions without the row
    carrying anything that identifies where they were.
    """
    if account:
        return await blind_index(env, "bug-reporter:account:" + account)
    address = _transient_client_address(request)
    if not address:
        # No usable address means no per-address quota is possible. Fall back
        # to one shared bucket rather than to no bucket at all.
        return await blind_index(env, "bug-reporter:anonymous")
    return await blind_index(env, "bug-reporter:address:" + address)


async def recent_report_count(env, reporter_bi, now=None):
    """How many reports this quota subject filed inside the window."""
    now = int(Date.now()) if now is None else int(now)
    row = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM bug_reports "
        "WHERE reporter_bi=? AND created_at >= ?",
        reporter_bi, now - RATE_LIMIT_WINDOW_MS)
    return int((row or {}).get("n", 0) or 0)


async def store_report(env, record, reporter_bi, account):
    """Insert one normalized report; return its id and the sealed fields."""
    report_id = new_report_id()
    now = int(Date.now())
    account = account or ""
    stored = dict(record)
    stored["account"] = account
    stored["receivedAt"] = now
    await d1_run(
        env,
        """INSERT INTO bug_reports
               (report_id, reporter_bi, account, source, platform,
                app_version, severity, status, data, created_at)
           VALUES (?,?,?,?,?,?,?,'new',?,?)""",
        report_id, reporter_bi, account, record["source"], record["platform"],
        record["appVersion"], record["severity"],
        await encrypt_row(env, stored), now)
    return report_id, stored


async def store_attachments(env, report_id, attachments):
    now = int(Date.now())
    for item in attachments:
        attachment_id = item["id"]
        data = item["data"]
        for chunk_index, start in enumerate(range(0, len(data), ATTACHMENT_CHUNK_BYTES)):
            chunk = data[start:start + ATTACHMENT_CHUNK_BYTES]
            sealed = await encrypt_row(env, {
                "data": base64.b64encode(chunk).decode("ascii")})
            await d1_run(
                env,
                """INSERT INTO bug_report_attachments
                       (report_id, attachment_id, chunk_index, name, content_type,
                        size, data, created_at)
                   VALUES (?,?,?,?,?,?,?,?)""",
                report_id, attachment_id, chunk_index, item["name"],
                item["contentType"], item["size"], sealed, now)


def capability_document(env=None):
    """The self-describing GET body.

    A client that has only the origin should be able to discover the shape of
    a report without a round trip through the docs site, and an operator
    reading a proxy log should be able to see what this endpoint accepts.
    """
    return {
        "ok": True,
        "endpoint": "/api/bug-reports",
        "method": "POST",
        "authentication": "optional session bearer; anonymous is accepted",
        "fields": {
            "message": "required, %d-%d characters" % (
                MIN_MESSAGE, MAX_MESSAGE),
            "contact": "optional, %d characters" % MAX_CONTACT,
            "screen": "optional, where in the app the report was filed",
            "severity": list(SEVERITIES),
            "source": list(SOURCES),
            "platform": "optional client platform name",
            "appVersion": "optional client build identifier",
            "osVersion": "optional operating-system version",
            "device": "optional device model",
            "package": "optional marketplace package id the report is about",
            "attachments": "optional list of up to %d base64 files" % MAX_ATTACHMENTS,
        },
        "limits": {
            "bodyBytes": MAX_BODY_BYTES,
            "attachmentBytesEach": MAX_ATTACHMENT_BYTES,
            "attachmentBytesTotal": MAX_ATTACHMENT_BYTES_TOTAL,
            "perReporterPerHour": RATE_LIMIT_MAX,
        },
        "private": True,
    }


async def bug_reports_handler(env, request):
    """``/api/bug-reports`` -- submit one report, or describe the endpoint."""
    method = method_name(request)
    no_store = "no-store, max-age=0, must-revalidate"
    if method in ("GET", "HEAD"):
        return json_response(capability_document(env), cache_control=no_store)
    if method != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control=no_store,
            extra_headers={"allow": "GET, POST"})
    try:
        data = await bounded_json_request(request, max_bytes=MAX_BODY_BYTES)
    except RequestBodyTooLarge:
        return json_response(
            {"error": "report_too_large", "maxBytes": MAX_BODY_BYTES},
            status=413, cache_control=no_store)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control=no_store)
    try:
        record = normalize_report(data)
    except ValueError as error:
        return json_response({"error": str(error)}, status=413 if str(error) == "attachment_too_large" else 400,
                             cache_control=no_store)
    if record is None:
        return json_response(
            {"error": "empty_report",
             "detail": "message is required and must describe the problem"},
            status=400, cache_control=no_store)

    await ensure_schema(env)
    # A session credits the report to an account; its absence is ordinary and
    # must not fail the submission. `data` is passed so a client that sends
    # `sessionToken` in the body authenticates the same way as one that sends
    # the bearer header.
    try:
        # safe_segment answers None for an anonymous caller; the column is
        # NOT NULL, so that used to fail the insert with a 500.
        account = safe_segment(
            await _authed_account_name(env, request, data)) or ""
    except Exception:
        account = ""
    reporter_bi = await _reporter_key(env, request, account)
    if await recent_report_count(env, reporter_bi) >= RATE_LIMIT_MAX:
        return json_response(
            {"error": "rate_limited",
             "detail": "too many reports from here in the last hour",
             "retryAfterSeconds": int(RATE_LIMIT_WINDOW_MS / 1000)},
            status=429, cache_control=no_store,
            extra_headers={
                "retry-after": str(int(RATE_LIMIT_WINDOW_MS / 1000))})
    attachments = _attachments(data.get("attachments"))
    for raw, stored_attachment in zip(attachments, record["attachments"]):
        raw["id"] = stored_attachment["id"]
    report_id, stored = await store_report(
        env, record, reporter_bi, account)
    await store_attachments(env, report_id, attachments)
    return json_response(
        {"ok": True, "id": report_id, "status": "new",
         "credited": bool(account), "attachments": len(attachments)},
        status=201, cache_control=no_store)


async def _inbox_admin(env, request):
    try:
        authorization = str(request.headers.get("authorization") or "")
    except Exception:
        return ""
    if not authorization.lower().startswith("bearer "):
        return ""
    try:
        actor = safe_segment(await _authed_account_name(env, request)) or ""
    except Exception:
        return ""
    return actor if actor and await _is_admin(env, actor) else ""


async def _sealed_record(env, value):
    if not value:
        return {}
    try:
        record = await decrypt_row(env, value)
    except Exception:
        return {}
    return record if isinstance(record, dict) else {}


async def inbox_report(env, row):
    record = await _sealed_record(env, row.get("data"))
    attachments = [
        {"id": str(item.get("id") or ""), "name": str(item.get("name") or "attachment"),
         "contentType": str(item.get("contentType") or ""),
         "size": int(item.get("size") or 0)}
        for item in (record.get("attachments") or []) if isinstance(item, dict)]
    return {
        "id": str(row.get("report_id") or ""),
        "status": str(row.get("status") or "new"),
        "severity": str(row.get("severity") or "normal"),
        "source": str(row.get("source") or ""),
        "platform": str(row.get("platform") or ""),
        "appVersion": str(row.get("app_version") or ""),
        "createdAt": int(row.get("created_at") or 0),
        "account": str(row.get("account") or record.get("account") or ""),
        "message": str(record.get("message") or ""),
        "contact": str(record.get("contact") or ""),
        "screen": str(record.get("screen") or ""),
        "package": str(record.get("package") or ""),
        "osVersion": str(record.get("osVersion") or ""),
        "device": str(record.get("device") or ""),
        "attachments": attachments,
    }


async def bug_reports_inbox_handler(env, request):
    no_store = "no-store, max-age=0, must-revalidate"
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control=no_store, extra_headers={"allow": "GET"})
    if not await _inbox_admin(env, request):
        return json_response(
            {"error": "admin_required"}, status=403, cache_control=no_store)
    params = parse_qs(urlparse(str(request.url or "")).query)

    def param(name):
        return _text((params.get(name) or [""])[0], 40).lower()

    status = param("status")
    status = status if status in STATUSES else ""
    try:
        limit = max(1, min(int(param("limit") or INBOX_PAGE_LIMIT),
                           INBOX_PAGE_LIMIT))
    except ValueError:
        limit = INBOX_PAGE_LIMIT
    try:
        offset = max(0, int(param("offset") or 0))
    except ValueError:
        offset = 0
    await ensure_schema(env)
    counts = {name: 0 for name in STATUSES}
    for row in await d1_all(
            env, "SELECT status, COUNT(*) AS n FROM bug_reports "
            "GROUP BY status") or []:
        row = dict(row)
        name = str(row.get("status") or "")
        if name in counts:
            counts[name] = int(row.get("n") or 0)
    clause, args = (" WHERE status=?", [status]) if status else ("", [])
    rows = await d1_all(
        env,
        "SELECT report_id, account, source, platform, app_version, severity, "
        "status, data, created_at FROM bug_reports" + clause
        + " ORDER BY created_at DESC, report_id DESC LIMIT ? OFFSET ?",
        *args, limit + 1, offset) or []
    reports = [await inbox_report(env, dict(row)) for row in rows[:limit]]
    return json_response(
        {"ok": True, "status": status or "all", "counts": counts,
         "reports": reports, "offset": offset,
         "nextOffset": offset + limit if len(rows) > limit else None},
        cache_control=no_store)
