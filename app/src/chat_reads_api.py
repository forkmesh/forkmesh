import hashlib
import re


READS_BODY_MAX_BYTES = 8 * 1024
READS_POST_MAX = 50
READS_LIST_MAX = 200
READS_KEEP_MAX = 500
READS_FUTURE_SLACK_MS = 24 * 60 * 60 * 1000
READS_TOPIC = "chat-read"
COLLECTION_RE = re.compile(r"^/api/chat/reads/?$")
CONVERSATION_RE = re.compile(
    r"^(#[a-z0-9][a-z0-9_-]{0,63}|private:[0-9a-f]{32}|direct:[0-9a-f]{32})$")


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


def clean_marks(raw, now):
    if not isinstance(raw, dict):
        return None
    marks = {}
    for key, value in raw.items():
        conversation = str(key or "").strip().lower()
        if not CONVERSATION_RE.fullmatch(conversation):
            return None
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return None
        ts = int(value)
        if ts <= 0 or ts > int(now) + READS_FUTURE_SLACK_MS:
            return None
        marks[conversation] = max(ts, marks.get(conversation, 0))
    if not marks or len(marks) > READS_POST_MAX:
        return None
    return marks


def marks_digest(marks):
    lines = [
        str(conversation) + " " + str(int(ts))
        for conversation, ts in sorted((marks or {}).items())
    ]
    return hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()


async def _list_marks(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT conversation,read_ts,updated_at FROM chat_read_marks "
        "WHERE account_bi=? ORDER BY updated_at DESC LIMIT ?",
        account_bi,
        READS_LIST_MAX,
    )
    marks = {}
    read_at = {}
    for row in rows or []:
        conversation = str(row.get("conversation") or "")
        if CONVERSATION_RE.fullmatch(conversation):
            marks[conversation] = int(row.get("read_ts") or 0)
            read_at[conversation] = int(row.get("updated_at") or 0)
    return _response(runtime, {"marks": marks, "readAt": read_at})


async def _save_marks(runtime, account_bi, actor, data):
    now = runtime.now()
    marks = clean_marks((data or {}).get("marks"), now)
    if marks is None:
        return _response(runtime, {"error": "invalid_marks"}, status=400)
    advanced = False
    for conversation, ts in marks.items():
        row = await runtime.d1_first(
            "SELECT read_ts FROM chat_read_marks "
            "WHERE account_bi=? AND conversation=?",
            account_bi,
            conversation,
        )
        if row and int(row.get("read_ts") or 0) >= ts:
            continue
        await runtime.d1_run(
            "INSERT INTO chat_read_marks "
            "(account_bi,conversation,read_ts,updated_at) VALUES (?,?,?,?) "
            "ON CONFLICT(account_bi,conversation) DO UPDATE SET "
            "read_ts=MAX(chat_read_marks.read_ts,excluded.read_ts),"
            "updated_at=excluded.updated_at",
            account_bi,
            conversation,
            ts,
            now,
        )
        advanced = True
    if advanced:
        await runtime.d1_run(
            "DELETE FROM chat_read_marks WHERE account_bi=? "
            "AND conversation NOT IN (SELECT conversation FROM chat_read_marks "
            "WHERE account_bi=? ORDER BY updated_at DESC LIMIT ?)",
            account_bi,
            account_bi,
            READS_KEEP_MAX,
        )
        try:
            await runtime.notify_account(actor, READS_TOPIC)
        except Exception:
            pass
    return _response(runtime, {"ok": True, "advanced": advanced})


async def handle(runtime, path):
    await runtime.ensure_schema()
    if not COLLECTION_RE.fullmatch(str(path or "")):
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    if method not in {"GET", "POST"}:
        return _response(
            runtime,
            {"error": "method_not_allowed"},
            status=405,
            allow="GET, POST",
        )
    data = None
    if method == "POST":
        data, error = await runtime.json_body(READS_BODY_MAX_BYTES)
        if error:
            return _response(
                runtime,
                {"error": error},
                status=413 if error == "payload_too_large" else 400,
            )
    account_bi, account = await runtime.session(data)
    actor = str((account or {}).get("name") or "").strip().lower()
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    if method == "GET":
        return await _list_marks(runtime, account_bi)
    return await _save_marks(runtime, account_bi, actor, data)
