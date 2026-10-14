"""Web sign-in of a runner's provider CLI (docs/RUNNERS.md, "Web sign-in").

A runner has no node account and no node event channel, so the website cannot
reach it the way it reaches a mirror's login terminal (node_terminal.py). This
module relays the same terminal over the runner's own signed polling instead:

* The account that registered the runner creates a login session
  for one provider. The runner learns about it from its next claim or poll
  response, which also tells it to poll every ``FAST_POLL_MS`` while a session
  is active, runs the provider's own login command on a pty as the job user,
  and posts the terminal output here in sequence-numbered chunks.
* The browser (or the desktop app, which uses the same endpoints with an
  ``Authorization: Bearer`` session) reads the chunks, and posts the code it
  was shown back as bounded ``input``; the runner fetches it and writes it to
  the pty. Nothing else can be typed: input is printable text and newlines.

What the app holds is the terminal in flight, never the credential: the
runner scrubs anything shaped like a token before it relays a line (and this
module scrubs again), keeps the credential in the job user's HOME, and only a
bounded tail of output chunks is stored. Input chunks are deleted as soon as
the runner has read them, and every chunk of a session is deleted
``ENDED_KEEP_MS`` after it ends. Chunk content is never logged or audited.

Browser/desktop endpoints (account session, cookie or bearer; cookie POSTs
must be same-origin -- ``_account_session_record`` enforces both):

``GET  /api/runners``                              runners the caller registered
``POST /api/runners/<name>/login``                 ``{provider}`` -> ``{id}``
``GET  /api/runners/<name>/login/<id>/output?after=<seq>``
``POST /api/runners/<name>/login/<id>/input``      ``{data}``
``POST /api/runners/<name>/login/<id>/cancel``
``POST /api/runners/<name>/login/<id>/resize``     ``{cols, rows}`` (terminals)
``POST /api/runners/<name>/accounts``              ``{provider, account, action}``

Runner-signed endpoints (``runner_leases._signed_caller``: node key, body
hash, single-use request id, 60 s window):

``POST /api/runners/poll``              availability + pending logins
``POST /api/runners/login/output``      ``{id, seq, data, state, inputAfter}``
``POST /api/runners/login/input-poll``  ``{id, after}``

``/api/runners/claim`` also accepts ``availability``/``activeLogins`` and
answers with ``logins`` and ``fastPollMs`` (``runner_extras``).
"""

import base64
import binascii
import json
import re

import runner_auth
import runner_leases as leases


PROVIDERS = leases.PROVIDERS
LOGIN_TTL_MS = 10 * 60 * 1000
FAST_POLL_MS = 1000
MAX_INPUT_BYTES = 4096
MAX_OUTPUT_BYTES = 8192
OUTPUT_KEEP_CHUNKS = 64
OUTPUT_PAGE = 64
MAX_PENDING_INPUT = 16
ENDED_KEEP_MS = 2 * 60 * 1000
SESSION_ROW_KEEP_MS = 24 * 60 * 60 * 1000
MAX_MESSAGE_CHARS = 240
ACTIVE = ("pending", "running")
RUNNER_FINAL = ("succeeded", "failed", "cancelled", "expired")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
ACTIVE_SQL = "('pending','running')"
ACCOUNT_ACTIONS = ("activate", "remove")
MAX_ACCOUNTS = 12
MAX_USAGE_WINDOWS = 8
MAX_PENDING_ACCOUNT_REQUESTS = 8
ACCOUNT_REQUEST_TTL_MS = 10 * 60 * 1000
WINDOW_ID_RE = re.compile(r"^[a-z0-9_]{1,40}$")
KINDS = ("login", "terminal")
TERMINAL_TTL_MS = 60 * 60 * 1000
TERMINAL_MAX_PENDING_INPUT = 64
TERMINAL_SIZE = (100, 40)
FEATURES = ("terminal",)
LOGIN_EXPIRED = "The sign-in took longer than 10 minutes."
TERMINAL_EXPIRED = "The terminal reached its 60-minute limit."

# Defence in depth; the runner already scrubs these before relaying a line.
_TOKEN_PATTERNS = (
    re.compile(rb"sk-ant-[A-Za-z0-9_-]{6,}"),
    re.compile(rb"sk-(?:proj-)?[A-Za-z0-9_-]{20,}"),
    re.compile(rb"eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}"),
)


def scrub(raw):
    for pattern in _TOKEN_PATTERNS:
        raw = pattern.sub(b"[redacted]", raw)
    return raw


def match(action):
    """The login route an ``/api/runners/<action>`` path names, or None."""
    parts = action.split("/") if action else []
    if not parts:
        return ("list",)
    if parts == ["poll"]:
        return ("poll",)
    if parts == ["attention"]:
        return ("attention",)
    if len(parts) == 2 and parts[0] == "login" and parts[1] in (
            "output", "input-poll"):
        return ("runner-" + parts[1],)
    if len(parts) == 2 and parts[1] == "login":
        return ("create", parts[0])
    if len(parts) == 2 and parts[1] == "accounts":
        return ("accounts", parts[0])
    if (len(parts) == 4 and parts[1] == "login"
            and parts[3] in ("output", "input", "cancel", "resize")):
        return (parts[3], parts[0], parts[2])
    return None


def _error(runtime, code, status, message=""):
    return leases._error(runtime, code, status, message)


def _ok(runtime, body, status=200):
    return runtime.response({"ok": True, **body}, status)


async def handle(runtime, route):
    kind = route[0]
    method = runtime.method()
    want = "GET" if kind in ("list", "output", "attention") else "POST"
    if method != want:
        return _error(runtime, "method_not_allowed", 405)
    raw = b""
    if want == "POST":
        raw = await runtime.body_bytes(leases.MAX_BODY_BYTES)
        if raw is None:
            return _error(runtime, "request_too_large", 413)
    if kind in ("poll", "runner-output", "runner-input-poll"):
        path = leases.PREFIX + ("poll" if kind == "poll" else
                                "login/" + kind[len("runner-"):])
        runner = await leases._signed_caller(runtime, path, raw)
        if runner is None:
            return _error(runtime, "unauthorized", 401,
                          "The runner signature, time, or request id is invalid.")
        data = leases._json(raw)
        if data is None:
            return _error(runtime, "invalid_json", 400)
        if kind == "poll":
            return await _poll(runtime, runner, data)
        return await _runner_io(runtime, runner, data, kind == "runner-output")
    data = leases._json(raw) if raw else {}
    if data is None:
        return _error(runtime, "invalid_json", 400)
    actor = await leases._account(runtime, data)
    if not actor:
        return _error(runtime, "unauthorized", 401)
    if kind == "list":
        return await _list(runtime, actor)
    if kind == "attention":
        return await _attention(runtime, actor)
    name = route[1].strip().lower()
    if not leases.NAME_RE.fullmatch(name):
        return _error(runtime, "not_found", 404)
    row = await runtime.d1_first(
        "SELECT name,registered_by,scope,status FROM runner_nodes WHERE name=?",
        name)
    if not row or not _may_manage(actor, row):
        return _error(runtime, "not_found", 404)
    if kind == "create":
        return await _create(runtime, actor, row, data)
    if kind == "accounts":
        return await _account_request(runtime, actor, row, data)
    session_id = route[2]
    session = await _session(runtime, session_id, name) if ID_RE.fullmatch(
        session_id) else None
    if not session:
        return _error(runtime, "not_found", 404)
    if kind == "output":
        return await _output(runtime, session)
    if kind == "input":
        return await _input(runtime, session, data)
    if kind == "resize":
        return await _resize(runtime, session, data)
    return await _cancel(runtime, actor, session)


def _may_manage(actor, row):
    return bool(actor) and row.get("registered_by") == actor


# --- shared state helpers ---------------------------------------------------

async def _sweep(runtime, now):
    """Expire overdue sessions and drop chunks/rows of long-ended ones."""
    await runtime.d1_run(
        "UPDATE runner_login_sessions SET state='expired',ended_at=?,"
        "updated_at=?,message=CASE WHEN kind='terminal' THEN ? ELSE ? END "
        "WHERE state IN " + ACTIVE_SQL + " AND expires_at<?", now, now,
        TERMINAL_EXPIRED, LOGIN_EXPIRED, now)
    # Input of any ended session at once; its output after ENDED_KEEP_MS,
    # which is long enough for the page to show the final lines.
    await runtime.d1_run(
        "DELETE FROM runner_login_chunks WHERE session_id IN ("
        "SELECT id FROM runner_login_sessions WHERE ended_at>0) "
        "AND (direction='in' OR session_id IN (SELECT id FROM "
        "runner_login_sessions WHERE ended_at>0 AND ended_at<?))",
        now - ENDED_KEEP_MS)
    await runtime.d1_run(
        "DELETE FROM runner_login_sessions WHERE ended_at>0 AND ended_at<?",
        now - SESSION_ROW_KEEP_MS)
    await runtime.d1_run(
        "UPDATE runner_account_requests SET state='failed',updated_at=?,"
        "message='The runner did not apply it in time.' "
        "WHERE state='pending' AND created_at<?",
        now, now - ACCOUNT_REQUEST_TTL_MS)
    await runtime.d1_run(
        "DELETE FROM runner_account_requests WHERE state!='pending' AND "
        "updated_at<?", now - SESSION_ROW_KEEP_MS)


async def _session(runtime, session_id, runner):
    row = await runtime.d1_first(
        "SELECT * FROM runner_login_sessions WHERE id=? AND runner=?",
        session_id, runner)
    if row and row.get("state") in ACTIVE and int(
            row.get("expires_at") or 0) < runtime.now():
        await _end(runtime, row["id"], "expired", None,
                   TERMINAL_EXPIRED if _terminal(row) else LOGIN_EXPIRED)
        row = await runtime.d1_first(
            "SELECT * FROM runner_login_sessions WHERE id=?", session_id)
    return row


async def _end(runtime, session_id, state, exit_code, message):
    now = runtime.now()
    result = await runtime.d1_run(
        "UPDATE runner_login_sessions SET state=?,ended_at=?,updated_at=?,"
        "exit_code=?,message=? WHERE id=? AND state IN " + ACTIVE_SQL,
        state, now, now, exit_code, message[:MAX_MESSAGE_CHARS], session_id)
    # A pasted code is worthless once the session is over; drop it now.
    await runtime.d1_run(
        "DELETE FROM runner_login_chunks WHERE session_id=? AND direction='in'",
        session_id)
    return runtime.changes(result) == 1


def _terminal(session):
    return (session or {}).get("kind") == "terminal"


def _public(session):
    body = {
        "id": session["id"],
        "runner": session["runner"],
        "provider": session["provider"],
        "kind": "terminal" if _terminal(session) else "login",
        "state": session["state"],
        "createdBy": session.get("created_by") or "",
        "createdAt": int(session.get("created_at") or 0),
        "expiresAt": int(session.get("expires_at") or 0),
        "endedAt": int(session.get("ended_at") or 0),
        "outputSeq": int(session.get("output_seq") or 0),
        "exitCode": session.get("exit_code"),
        "message": session.get("message") or "",
    }
    if _terminal(session):
        body["cols"] = int(session.get("term_cols") or 0)
        body["rows"] = int(session.get("term_rows") or 0)
    return body


def _features(value):
    items = value.split(",") if isinstance(value, str) else value
    if not isinstance(items, list):
        return []
    return [name for name in FEATURES if name in items[:16]]


def _size(data):
    cols = _whole(data.get("cols"), 0, 10000)
    rows = _whole(data.get("rows"), 0, 10000)
    if not cols or not rows:
        return TERMINAL_SIZE
    return min(max(cols, 20), 300), min(max(rows, 5), 120)


def _text(value, limit):
    text = value if isinstance(value, str) else ""
    return "".join(ch for ch in text if ch.isprintable())[:limit]


def _whole(value, low=0, high=2 ** 53):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return 0
    return int(min(max(value, low), high))


def _usage(value):
    if not isinstance(value, dict):
        return None
    windows = []
    items = value.get("windows")
    for item in items[:MAX_USAGE_WINDOWS] if isinstance(items, list) else []:
        if not isinstance(item, dict):
            continue
        window_id = str(item.get("id") or "")
        used = item.get("usedPercent")
        if not WINDOW_ID_RE.fullmatch(window_id) or isinstance(
                used, bool) or not isinstance(used, (int, float)):
            continue
        windows.append({
            "id": window_id,
            "label": _text(item.get("label"), 60) or window_id,
            "usedPercent": round(min(max(float(used), 0.0), 1000.0), 2),
            "resetsAt": _whole(item.get("resetsAt")),
            "windowMinutes": _whole(item.get("windowMinutes"), 0, 525600),
        })
    return {
        "windows": windows,
        "plan": _text(value.get("plan"), 40),
        "fetchedAt": _whole(value.get("fetchedAt")),
        "error": _text(value.get("error"), 160),
    }


def _accounts(value):
    accounts = []
    for item in value[:MAX_ACCOUNTS] if isinstance(value, list) else []:
        if not isinstance(item, dict):
            continue
        account_id = str(item.get("id") or "")
        if not ID_RE.fullmatch(account_id):
            continue
        entry = {
            "id": account_id,
            "label": _text(item.get("label"), 120),
            "email": _text(item.get("email"), 120),
            "plan": _text(item.get("plan"), 40),
            "active": item.get("active") is True,
            "addedAt": _whole(item.get("addedAt")),
        }
        usage = _usage(item.get("usage"))
        if usage is not None:
            entry["usage"] = usage
        accounts.append(entry)
    return accounts


def _availability(value, auth=None):
    """The runner's availability snapshot, reduced to known fields."""
    if not isinstance(value, dict):
        return None
    auth = auth if isinstance(auth, dict) else {}
    result = {}
    for provider in PROVIDERS:
        entry = value.get(provider)
        if not isinstance(entry, dict):
            continue
        state = str(entry.get("loginState") or "")[:32]
        result[provider] = {
            "binaryFound": entry.get("binaryFound") is True,
            "loginState": state if re.fullmatch(r"[a-z_-]*", state) else "",
            "source": str(entry.get("source") or "")[:40],
            "accounts": _accounts(entry.get("accounts")),
        }
        cleaned = runner_auth.clean(entry.get("auth") or auth.get(provider))
        if cleaned is not None:
            result[provider]["auth"] = cleaned
    return result


APP_AUTH_MARK = "job_failed_report"


async def stored_availability(runtime, name):
    row = await runtime.d1_first(
        "SELECT data FROM runner_availability WHERE runner=?", name)
    try:
        data = json.loads(str((row or {}).get("data") or "{}"))
    except ValueError:
        data = {}
    return data if isinstance(data, dict) else {}


async def _save_availability(runtime, name, availability, now):
    await runtime.d1_run(
        "INSERT INTO runner_availability (runner,data,updated_at) "
        "VALUES (?,?,?) ON CONFLICT(runner) DO UPDATE SET "
        "data=excluded.data,updated_at=excluded.updated_at",
        name, json.dumps(availability, sort_keys=True), now)


def _app_mark(entry):
    auth = entry.get("auth") if isinstance(entry, dict) else None
    return auth if isinstance(auth, dict) and auth.get(
        "reason") == APP_AUTH_MARK else None


async def _relogged(runtime, name, provider, since):
    row = await runtime.d1_first(
        "SELECT id FROM runner_login_sessions WHERE runner=? AND provider=? "
        "AND kind='login' AND state='succeeded' AND updated_at>=? LIMIT 1",
        name, provider, int(since or 0))
    return bool(row)


async def _keep_app_marks(runtime, name, old, new):
    for provider in PROVIDERS:
        mark = _app_mark(old.get(provider))
        after = new.get(provider)
        if mark is None or not isinstance(after, dict) or "auth" in after:
            continue
        if await _relogged(runtime, name, provider, mark.get("checkedAt")):
            continue
        after["loginState"] = "expired"
        after["auth"] = dict(mark)


async def blocked_providers(runtime, runner, data):
    incoming = _availability(data.get("availability"), data.get("auth")) or {}
    stored = await stored_availability(runtime, runner["name"])
    await _keep_app_marks(runtime, runner["name"], stored, incoming)
    merged = dict(stored)
    merged.update(incoming)
    return set(runner_auth.needs_login(merged))


async def _alert(runtime, runner, old, new, now):
    changes = runner_auth.transitions(old, new)
    if not changes:
        return 0
    alert = getattr(runtime, "alert_runner_auth", None)
    if alert is None:
        return 0
    try:
        return await runner_auth.notify(
            runtime, runner["name"], changes, now,
            registered_by=runner.get("registeredBy") or "",
            scope=runner.get("scope") or "")
    except Exception:
        return 0


async def note_job_result(runtime, runner, job_id, data):
    status = str(data.get("status") or "").lower()
    availability = data.get("availability")
    availability = availability if isinstance(availability, dict) else {}
    info = data.get("agentInfo")
    info = info if isinstance(info, dict) else {}
    provider = str(availability.get("provider") or info.get("provider") or "")
    if provider not in PROVIDERS:
        return False
    failed = status == "failed" and (
        str(data.get("failureKind") or "") == "auth_expired"
        or runner_auth.auth_failure_text(
            data.get("result"), data.get("securityReason"),
            info.get("lastError")))
    now = runtime.now()
    old = await stored_availability(runtime, runner["name"])
    entry = dict(old.get(provider) or {
        "binaryFound": True, "loginState": "", "source": "", "accounts": []})
    if failed:
        if runner_auth.state_of(entry) == "expired":
            return False
        entry["loginState"] = "expired"
        entry["auth"] = {
            "state": "expired", "checkedAt": now, "expiresAt": 0,
            "hasRefresh": False, "reason": APP_AUTH_MARK,
            "message": runner_auth.provider_label(provider)
            + " login on this runner expired and could not be refreshed.",
            "failedAt": 0, "failedJob": int(job_id or 0),
        }
    elif status == "completed" and _app_mark(entry) is not None:
        entry["loginState"] = "available"
        entry.pop("auth", None)
    else:
        return False
    new = dict(old)
    new[provider] = entry
    await _save_availability(runtime, runner["name"], new, now)
    await _alert(runtime, runner, old, new, now)
    return failed


def _public_account_request(row):
    return {
        "id": row["id"],
        "provider": row["provider"],
        "action": row["action"],
        "account": row["account"],
        "state": row["state"],
        "message": row.get("message") or "",
        "createdBy": row.get("created_by") or "",
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
    }


# --- browser / desktop ------------------------------------------------------

async def _list(runtime, actor):
    now = runtime.now()
    await _sweep(runtime, now)
    rows = await runtime.d1_all(
        "SELECT name,registered_by,scope,status,allowed_owners,"
        "last_seen_at FROM runner_nodes WHERE registered_by=? ORDER BY name",
        actor)
    runners = []
    for row in rows:
        name = row["name"]
        snapshot = await runtime.d1_first(
            "SELECT data,updated_at,features FROM runner_availability "
            "WHERE runner=?", name)
        try:
            availability = json.loads((snapshot or {}).get("data") or "{}")
        except ValueError:
            availability = {}
        sessions = await runtime.d1_all(
            "SELECT * FROM runner_login_sessions WHERE runner=? AND "
            "kind='login' AND (state IN " + ACTIVE_SQL + " OR ended_at>=?) "
            "ORDER BY created_at DESC LIMIT 4", name, now - ENDED_KEEP_MS)
        terminals = await runtime.d1_all(
            "SELECT * FROM runner_login_sessions WHERE runner=? AND "
            "kind='terminal' AND state IN " + ACTIVE_SQL +
            " ORDER BY created_at DESC LIMIT 4", name)
        requests = await runtime.d1_all(
            "SELECT * FROM runner_account_requests WHERE runner=? AND ("
            "state='pending' OR updated_at>=?) ORDER BY created_at DESC "
            "LIMIT 8", name, now - ENDED_KEEP_MS)
        try:
            owners = json.loads(row.get("allowed_owners") or "[]")
        except ValueError:
            owners = []
        active = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM runner_job_leases WHERE runner=? "
            "AND lease_id!='' AND expires_at>=?", name, now)
        runners.append({
            "name": name,
            "scope": row.get("scope"),
            "status": row.get("status"),
            "registeredBy": row.get("registered_by") or "",
            "allowedOwners": owners if isinstance(owners, list) else [],
            "lastSeenAt": int(row.get("last_seen_at") or 0),
            "activeLeases": int((active or {}).get("n") or 0),
            "availability": availability if isinstance(
                availability, dict) else {},
            "availabilityAt": int((snapshot or {}).get("updated_at") or 0),
            "logins": [_public(s) for s in sessions],
            "terminals": [_public(s) for s in terminals],
            "features": _features((snapshot or {}).get("features")),
            "accountRequests": [_public_account_request(r) for r in requests],
        })
    return _ok(runtime, {"runners": runners, "providers": list(PROVIDERS),
                         "now": now})


ATTENTION_POLL_MS = 5 * 60 * 1000
ATTENTION_MAX_AGE_S = 60
ONLINE_MS = 10 * 60 * 1000


def connect_href(runner, provider):
    return runner_auth.runners_href(runner, provider) + "&connect=1"


def attention_items(rows, sessions, now):
    open_logins = {}
    for session in sessions or []:
        key = (session.get("runner"), session.get("provider"))
        open_logins.setdefault(key, str(session.get("id") or ""))
    items = []
    for row in rows or []:
        name = str(row.get("name") or "")
        try:
            availability = json.loads(str(row.get("data") or "{}"))
        except ValueError:
            availability = {}
        if not isinstance(availability, dict):
            continue
        last_seen = int(row.get("last_seen_at") or 0)
        for provider in PROVIDERS:
            entry = availability.get(provider)
            if not isinstance(entry, dict) or entry.get(
                    "binaryFound") is False:
                continue
            state = runner_auth.state_of(entry)
            if state not in runner_auth.ALERT_STATES:
                continue
            auth = entry.get("auth") if isinstance(
                entry.get("auth"), dict) else {}
            short = runner_auth.provider_short(provider)
            items.append({
                "runner": name,
                "provider": provider,
                "providerLabel": short,
                "state": state,
                "scope": str(row.get("scope") or ""),
                "canConnect": True,
                "online": bool(last_seen and now - last_seen <= ONLINE_MS),
                "lastSeenAt": last_seen,
                "expiresAt": int(auth.get("expiresAt") or 0),
                "title": ("%s needs %s sign-in" % (name, short)
                          if state == "expired" else
                          "%s: %s sign-in expires soon" % (name, short)),
                "connectUrl": connect_href(name, provider),
                "loginId": open_logins.get((name, provider), ""),
            })
    items.sort(key=lambda item: (
        0 if item["state"] == "expired" else 1, item["runner"],
        item["provider"]))
    return items


async def _attention(runtime, actor):
    now = runtime.now()
    sql = ("SELECT n.name,n.scope,n.last_seen_at,a.data FROM runner_nodes n "
           "LEFT JOIN runner_availability a ON a.runner=n.name "
           "WHERE n.status='active' AND n.registered_by=? ORDER BY n.name")
    rows = await runtime.d1_all(sql, actor)
    sessions = []
    if rows:
        sessions = await runtime.d1_all(
            "SELECT id,runner,provider FROM runner_login_sessions WHERE "
            "kind='login' AND state IN " + ACTIVE_SQL + " AND expires_at>=? "
            "ORDER BY created_at DESC", now)
    items = attention_items(rows, sessions, now)
    body = {
        "ok": True,
        "items": items,
        "count": len(items),
        "expired": sum(1 for item in items if item["state"] == "expired"),
        "runners": len(rows or []),
        "checkedAt": now,
        "pollMs": ATTENTION_POLL_MS,
    }
    cached = getattr(runtime, "cached_response", None)
    if cached is None:
        return runtime.response(body)
    return cached(body, ATTENTION_MAX_AGE_S)


async def _create(runtime, actor, row, data):
    provider = str(data.get("provider") or "").strip().lower()
    if provider not in PROVIDERS:
        return _error(runtime, "invalid_provider", 400)
    kind = str(data.get("kind") or "login").strip().lower()
    if kind not in KINDS:
        return _error(runtime, "invalid_kind", 400,
                      "The kind must be login or terminal.")
    if row.get("status") != "active":
        return _error(runtime, "runner_inactive", 409,
                      "That runner is revoked.")
    terminal = kind == "terminal"
    cols, rows = _size(data) if terminal else (0, 0)
    if terminal:
        snapshot = await runtime.d1_first(
            "SELECT features FROM runner_availability WHERE runner=?",
            row["name"])
        if "terminal" not in _features((snapshot or {}).get("features")):
            return _error(runtime, "terminal_unsupported", 409,
                          "Update this runner to open a terminal on it.")
    now = runtime.now()
    await _sweep(runtime, now)
    session_id = runtime.new_id()
    result = await runtime.d1_run(
        "INSERT OR IGNORE INTO runner_login_sessions (id,runner,provider,"
        "state,created_by,created_at,updated_at,expires_at,kind,term_cols,"
        "term_rows) VALUES (?,?,?,'pending',?,?,?,?,?,?,?)",
        session_id, row["name"], provider, actor, now, now,
        now + (TERMINAL_TTL_MS if terminal else LOGIN_TTL_MS), kind, cols,
        rows)
    if runtime.changes(result) != 1:
        existing = await runtime.d1_first(
            "SELECT * FROM runner_login_sessions WHERE runner=? AND "
            "provider=? AND kind=? AND state IN " + ACTIVE_SQL, row["name"],
            provider, kind)
        body = {"ok": False, "error": kind + "_active",
                "message": ("A terminal for that provider is already open."
                            if terminal else
                            "A sign-in for that provider is already open.")}
        if existing:
            body["session"] = _public(existing)
            body["id"] = existing["id"]
        return runtime.response(body, 409)
    await runtime.audit(actor, "runner.%s.start" % kind,
                        row["name"] + ":" + provider)
    session = await runtime.d1_first(
        "SELECT * FROM runner_login_sessions WHERE id=?", session_id)
    return _ok(runtime, {"id": session_id, "session": _public(session)}, 201)


async def _account_request(runtime, actor, row, data):
    provider = str(data.get("provider") or "").strip().lower()
    if provider not in PROVIDERS:
        return _error(runtime, "invalid_provider", 400)
    action = str(data.get("action") or "").strip().lower()
    if action not in ACCOUNT_ACTIONS:
        return _error(runtime, "invalid_action", 400,
                      "The action must be activate or remove.")
    account = str(data.get("account") or "").strip().lower()
    if not ID_RE.fullmatch(account):
        return _error(runtime, "invalid_account", 400)
    if row.get("status") != "active":
        return _error(runtime, "runner_inactive", 409,
                      "That runner is revoked.")
    snapshot = await runtime.d1_first(
        "SELECT data FROM runner_availability WHERE runner=?", row["name"])
    try:
        availability = json.loads((snapshot or {}).get("data") or "{}")
    except ValueError:
        availability = {}
    known = availability.get(provider) if isinstance(
        availability, dict) else None
    accounts = known.get("accounts") if isinstance(known, dict) else None
    if not any(isinstance(item, dict) and item.get("id") == account
               for item in accounts or []):
        return _error(runtime, "unknown_account", 404,
                      "The runner does not report that account.")
    now = runtime.now()
    await _sweep(runtime, now)
    pending = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM runner_account_requests WHERE runner=? "
        "AND state='pending'", row["name"])
    if int((pending or {}).get("n") or 0) >= MAX_PENDING_ACCOUNT_REQUESTS:
        return _error(runtime, "request_backlog", 429,
                      "The runner has not applied the earlier changes yet.")
    request_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO runner_account_requests (id,runner,provider,action,"
        "account,state,created_by,created_at,updated_at) "
        "VALUES (?,?,?,?,?,'pending',?,?,?)",
        request_id, row["name"], provider, action, account, actor, now, now)
    await runtime.audit(actor, "runner.account." + action,
                        row["name"] + ":" + provider + ":" + account)
    created = await runtime.d1_first(
        "SELECT * FROM runner_account_requests WHERE id=?", request_id)
    return _ok(runtime, {"id": request_id,
                         "request": _public_account_request(created)}, 202)


async def _account_results(runtime, runner, value, now):
    for item in value[:32] if isinstance(value, list) else []:
        if not isinstance(item, dict):
            continue
        request_id = str(item.get("id") or "")
        if not ID_RE.fullmatch(request_id):
            continue
        message = scrub(_text(item.get("message"), MAX_MESSAGE_CHARS).encode(
            "utf-8", "replace")).decode("utf-8", "replace")
        await runtime.d1_run(
            "UPDATE runner_account_requests SET state=?,message=?,"
            "updated_at=? WHERE id=? AND runner=? AND state='pending'",
            "done" if item.get("ok") is True else "failed", message, now,
            request_id, runner["name"])


async def _output(runtime, session):
    try:
        after = max(0, int(runtime.query("after") or 0))
    except (TypeError, ValueError):
        after = 0
    rows = await runtime.d1_all(
        "SELECT seq,data FROM runner_login_chunks WHERE session_id=? AND "
        "direction='out' AND seq>? ORDER BY seq LIMIT %d" % OUTPUT_PAGE,
        session["id"], after)
    chunks = [{"seq": int(r["seq"]), "data": r["data"]} for r in rows]
    first = chunks[0]["seq"] if chunks else 0
    return _ok(runtime, {
        **_public(session),
        "chunks": chunks,
        "nextSeq": chunks[-1]["seq"] if chunks else after,
        # Only the newest OUTPUT_KEEP_CHUNKS are stored; a slow reader that
        # fell behind is told so rather than shown a silently spliced screen.
        "truncated": bool(chunks) and first > after + 1,
        "more": len(chunks) == OUTPUT_PAGE,
    })


def _input_text(value, terminal=False):
    if not isinstance(value, str) or not value:
        return None
    try:
        raw = value.encode("utf-8")
    except UnicodeEncodeError:
        return None
    if len(raw) > MAX_INPUT_BYTES:
        return None
    if terminal:
        return None if "\x00" in value else raw
    if not all(ch.isprintable() or ch in "\r\n\t" for ch in value):
        return None
    return raw


async def _input(runtime, session, data):
    terminal = _terminal(session)
    raw = _input_text(data.get("data"), terminal)
    if raw is None:
        return _error(runtime, "invalid_input", 400,
                      "Input must be 1 to 4096 bytes of text." if terminal
                      else "Input must be 1 to 4096 bytes of printable text.")
    if session["state"] not in ACTIVE:
        return _error(runtime, "login_ended", 409)
    pending = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM runner_login_chunks WHERE session_id=? "
        "AND direction='in'", session["id"])
    limit = TERMINAL_MAX_PENDING_INPUT if terminal else MAX_PENDING_INPUT
    if int((pending or {}).get("n") or 0) >= limit:
        return _error(runtime, "input_backlog", 429,
                      "The runner has not read the earlier input yet.")
    seq = int(session.get("input_seq") or 0) + 1
    result = await runtime.d1_run(
        "UPDATE runner_login_sessions SET input_seq=?,updated_at=? "
        "WHERE id=? AND input_seq=? AND state IN " + ACTIVE_SQL,
        seq, runtime.now(), session["id"], seq - 1)
    if runtime.changes(result) != 1:
        return _error(runtime, "conflict", 409, "Try again.")
    await runtime.d1_run(
        "INSERT INTO runner_login_chunks (session_id,direction,seq,data,"
        "created_at) VALUES (?,'in',?,?,?)", session["id"], seq,
        base64.b64encode(raw).decode("ascii"), runtime.now())
    return _ok(runtime, {"seq": seq})


async def _resize(runtime, session, data):
    if not _terminal(session):
        return _error(runtime, "not_a_terminal", 400)
    if session["state"] not in ACTIVE:
        return _error(runtime, "login_ended", 409)
    cols, rows = _size(data)
    await runtime.d1_run(
        "UPDATE runner_login_sessions SET term_cols=?,term_rows=?,"
        "updated_at=? WHERE id=? AND state IN " + ACTIVE_SQL, cols, rows,
        runtime.now(), session["id"])
    fresh = await runtime.d1_first(
        "SELECT * FROM runner_login_sessions WHERE id=?", session["id"])
    return _ok(runtime, _public(fresh))


async def _cancel(runtime, actor, session):
    if session["state"] in ACTIVE:
        await _end(runtime, session["id"], "cancelled", None,
                   "Cancelled by " + actor + ".")
        await runtime.audit(
            actor, "runner.%s.cancel" % (
                "terminal" if _terminal(session) else "login"),
            session["runner"] + ":" + session["provider"])
    fresh = await runtime.d1_first(
        "SELECT * FROM runner_login_sessions WHERE id=?", session["id"])
    return _ok(runtime, _public(fresh))


# --- runner side ------------------------------------------------------------

async def runner_extras(runtime, runner, data):
    """Availability bookkeeping and login hand-off for a claim or poll."""
    now = runtime.now()
    availability = _availability(data.get("availability"), data.get("auth"))
    if availability is not None:
        old = await stored_availability(runtime, runner["name"])
        await _keep_app_marks(runtime, runner["name"], old, availability)
        await _save_availability(runtime, runner["name"], availability, now)
        await _alert(runtime, runner, old, availability, now)
    features = _features(data.get("features"))
    await runtime.d1_run(
        "UPDATE runner_availability SET features=? WHERE runner=? AND "
        "features!=?", ",".join(features), runner["name"],
        ",".join(features))
    await _sweep(runtime, now)
    await _account_results(runtime, runner, data.get("accountResults"), now)
    account_requests = await runtime.d1_all(
        "SELECT id,provider,action,account FROM runner_account_requests "
        "WHERE runner=? AND state='pending' ORDER BY created_at,rowid "
        "LIMIT %d" % MAX_PENDING_ACCOUNT_REQUESTS, runner["name"])
    running = data.get("activeLogins")
    running = {str(x) for x in running[:8]} if isinstance(
        running, list) else set()
    sessions = await runtime.d1_all(
        "SELECT id,provider,state,expires_at,kind,term_cols,term_rows FROM "
        "runner_login_sessions WHERE runner=? AND state IN " + ACTIVE_SQL +
        " ORDER BY created_at", runner["name"])
    logins = []
    for session in sessions:
        if session["id"] in running:
            continue
        if _terminal(session) and "terminal" not in features:
            continue
        if session["state"] == "pending":
            result = await runtime.d1_run(
                "UPDATE runner_login_sessions SET state='running',"
                "updated_at=? WHERE id=? AND state='pending'",
                now, session["id"])
            if runtime.changes(result) != 1:
                continue
        # A running session the runner does not list was lost (a restart, or
        # a response that never arrived): hand it over again.
        login = {
            "id": session["id"],
            "provider": session["provider"],
            "kind": "terminal" if _terminal(session) else "login",
            "expiresAt": int(session["expires_at"]),
            "ttlMs": max(0, int(session["expires_at"]) - now),
        }
        if _terminal(session):
            login["cols"] = int(session.get("term_cols") or 0)
            login["rows"] = int(session.get("term_rows") or 0)
        logins.append(login)
    return {"logins": logins,
            "accountRequests": [{
                "id": r["id"], "provider": r["provider"],
                "action": r["action"], "account": r["account"],
            } for r in account_requests],
            "fastPollMs": FAST_POLL_MS if sessions or account_requests else 0}


async def _poll(runtime, runner, data):
    await runtime.d1_run(
        "UPDATE runner_nodes SET last_seen_at=? WHERE name=?",
        runtime.now(), runner["name"])
    return _ok(runtime, await runner_extras(runtime, runner, data))


async def _runner_io(runtime, runner, data, with_output):
    session_id = str(data.get("id") or "")
    session = await _session(runtime, session_id, runner["name"]) if (
        ID_RE.fullmatch(session_id)) else None
    if not session:
        return _error(runtime, "not_found", 404)
    if with_output:
        try:
            seq = int(data.get("seq") or 0)
        except (TypeError, ValueError):
            return _error(runtime, "invalid_seq", 400)
        encoded = data.get("data") or ""
        if seq > 0 and encoded:
            try:
                chunk = base64.b64decode(str(encoded), validate=True)
            except (binascii.Error, ValueError):
                return _error(runtime, "invalid_data", 400)
            if not chunk or len(chunk) > MAX_OUTPUT_BYTES:
                return _error(runtime, "invalid_data", 400)
            current = int(session.get("output_seq") or 0)
            if seq > current + 1:
                return _error(runtime, "out_of_order", 409)
            if seq == current + 1 and session["state"] == "running":
                result = await runtime.d1_run(
                    "UPDATE runner_login_sessions SET output_seq=?,"
                    "updated_at=? WHERE id=? AND output_seq=? AND "
                    "state='running'", seq, runtime.now(), session_id,
                    current)
                if runtime.changes(result) == 1:
                    await runtime.d1_run(
                        "INSERT OR IGNORE INTO runner_login_chunks (session_id,"
                        "direction,seq,data,created_at) VALUES (?,'out',?,?,?)",
                        session_id, seq,
                        base64.b64encode(scrub(chunk)).decode("ascii"),
                        runtime.now())
                    await runtime.d1_run(
                        "DELETE FROM runner_login_chunks WHERE session_id=? "
                        "AND direction='out' AND seq<=?", session_id,
                        seq - OUTPUT_KEEP_CHUNKS)
            # seq <= current: a retried post the app already stored.
        state = str(data.get("state") or "")
        if state in RUNNER_FINAL:
            try:
                code = int(data.get("exitCode"))
            except (TypeError, ValueError):
                code = None
            message = scrub(str(data.get("message") or "").encode(
                "utf-8", "replace")).decode("utf-8", "replace")
            await _end(runtime, session_id, state, code, message)
        session = await runtime.d1_first(
            "SELECT * FROM runner_login_sessions WHERE id=?", session_id)
    after_key = "inputAfter" if with_output else "after"
    try:
        after = max(0, int(data.get(after_key) or 0))
    except (TypeError, ValueError):
        after = 0
    # The runner has written everything up to `after` to the pty.
    if after:
        await runtime.d1_run(
            "DELETE FROM runner_login_chunks WHERE session_id=? AND "
            "direction='in' AND seq<=?", session_id, after)
    rows = await runtime.d1_all(
        "SELECT seq,data FROM runner_login_chunks WHERE session_id=? AND "
        "direction='in' AND seq>? ORDER BY seq LIMIT %d" % (
            TERMINAL_MAX_PENDING_INPUT if _terminal(session)
            else MAX_PENDING_INPUT),
        session_id, after) if session["state"] in ACTIVE else []
    body = {
        "state": session["state"],
        "expiresAt": int(session["expires_at"]),
        "ttlMs": max(0, int(session["expires_at"]) - runtime.now()),
        "input": [{"seq": int(r["seq"]), "data": r["data"]} for r in rows],
    }
    if _terminal(session):
        body["cols"] = int(session.get("term_cols") or 0)
        body["rows"] = int(session.get("term_rows") or 0)
    return _ok(runtime, body)
