"""In-app bug reports: one bounded write per report, read from the console.

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

Nothing here reads reports back, so the public API surface stays write-only
and no public route can be coaxed into listing what other people reported.
Reports are read and triaged only in the platform-admin console's Bug
reports section (admin_support.py), behind the secret console path, a live
is_admin check and the console CSRF token.

Every stored report is also filed as an issue in the ForkMesh repository
(issue #696), through the same ``issue_inbox`` queue the Discord watcher and
the web composer write to, so it reaches the repository whenever the owner's
node next syncs.  The issue is public, so it carries the reporter's
description and the build context but never ``contact``: that stays in the
encrypted row for the console.  Filing is best effort -- the stored report is
the durable acceptance, and an inbox hiccup must never turn it into an error.
"""

import re


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


# One report is a bounded amount of prose plus a short contact string. The body
# limit is generous enough for a pasted stack trace and small enough that the
# encrypted row stays far inside D1's row ceiling.
MAX_BODY_BYTES = 16 * 1024
MAX_MESSAGE = 4000
MAX_CONTACT = 200
MAX_SCREEN = 120
MAX_VERSION = 60
MAX_PLATFORM = 40
MAX_DEVICE = 120
# Below this a "report" is a stray tap, not something an operator can act on.
MIN_MESSAGE = 4

# Per-reporter hourly quota. High enough that somebody genuinely hitting
# several bugs in one session is never turned away, low enough that a script
# cannot fill the table from one address.
RATE_LIMIT_WINDOW_MS = 60 * 60 * 1000
RATE_LIMIT_MAX = 10

# Reports the console has not been shown yet are what the badge counts, so the
# statuses are deliberately few and mean exactly one thing each.
STATUSES = ("new", "triaged", "closed")

# Where the report came from. An unknown value is normalized rather than
# rejected: a future client should not have its reports dropped by an older
# Worker that has not heard of it yet.
SOURCES = ("mobile", "desktop", "web", "unknown")

# Anything the reporter can set is clamped to this severity vocabulary. It is
# a hint for triage order, never a promise about response time.
SEVERITIES = ("low", "normal", "high", "crash")

# The repository reports are filed into. ``BUG_REPORTS_REPO`` overrides it
# (``off`` disables filing); unset uses the flagship, so no manifest variable
# is spent on restating the default.
DEFAULT_ISSUE_REPO = "forkmesh/forkmesh"
ISSUE_REPO_DISABLED = ("off", "none", "disabled", "false", "0")
ISSUE_LABELS = ("bug", "in-app-report")
PLUGIN_LABEL = "plugin"
MAX_ISSUE_TITLE = 120
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


def issue_repo(env):
    """``(owner, repo)`` that reports are filed into, or ``None`` when off."""
    configured = getattr(env, "BUG_REPORTS_REPO", "") or ""
    configured = _text(
        configured if isinstance(configured, str) else "", 161).lower()
    if configured in ISSUE_REPO_DISABLED:
        return None
    repo = configured or DEFAULT_ISSUE_REPO
    if repo.count("/") != 1:
        return None
    owner, name = repo.split("/", 1)
    if not SEGMENT_RE.fullmatch(owner) or not SEGMENT_RE.fullmatch(name):
        return None
    return owner, name


def issue_title(record):
    """A one-line issue title drawn from the reporter's description."""
    text = re.sub(r"\s+", " ", record.get("message") or "").strip(" .:-")
    for separator in (". ", "! ", "? "):
        index = text.find(separator)
        if 12 <= index <= MAX_ISSUE_TITLE:
            text = text[:index]
            break
    if len(text) > MAX_ISSUE_TITLE:
        text = text[:MAX_ISSUE_TITLE - 3].rstrip() + "..."
    prefix = "Crash: " if record.get("severity") == "crash" else ""
    if record.get("package"):
        prefix = "[%s] %s" % (record["package"], prefix)
    return prefix + (text or "In-app bug report")


def issue_body(record, account, report_id):
    """The public issue body: the description, then where it came from.

    ``contact`` is deliberately absent -- it is typically an email address and
    the issue is public -- so the body only says that one was left.
    """
    context = [
        ("Severity", record.get("severity")),
        ("Plugin", record.get("package")),
        ("Screen", record.get("screen")),
        ("Source", record.get("source")),
        ("Platform", record.get("platform")),
        ("App version", record.get("appVersion")),
        ("OS version", record.get("osVersion")),
        ("Device", record.get("device")),
        ("Reporter", account or "signed out"),
        ("Report id", report_id),
    ]
    lines = [record.get("message") or "", "", "---",
             "_Filed from the in-app bug report button._", ""]
    lines.extend("- %s: %s" % (label, value)
                 for label, value in context if value)
    if record.get("contact"):
        lines.append("- The reporter left contact details in the private "
                     "report.")
    return "\n".join(lines)


def issue_labels(record):
    labels = list(ISSUE_LABELS)
    if record.get("package"):
        labels.append(PLUGIN_LABEL)
    return labels


async def file_issue(env, record, account, report_id, target):
    """Queue the report as an issue; the proposed issue number, or 0."""
    owner, repo = target
    try:
        ok, item = await _forkbot_enqueue_issue(
            env, owner, repo, issue_title(record),
            issue_body(record, account, report_id), account,
            source="app-report", labels=issue_labels(record),
            authenticated_requester=account)
    except Exception as exc:
        ok, item = False, str(exc)
    if ok:
        return int((item or {}).get("number") or 0)
    try:
        await log_error(
            env, 500, "POST", "/api/bug-reports",
            "Bug report %s could not be filed as an issue in %s/%s: %s"
            % (report_id, owner, repo, clean_string(item, 80)))
    except Exception:
        pass
    return 0


async def record_issue_number(env, stored, report_id, target, number):
    """Remember which issue a report became, for the console."""
    stored = dict(stored, issueRepo="/".join(target), issueNumber=int(number))
    try:
        await d1_run(
            env, "UPDATE bug_reports SET data=? WHERE report_id=?",
            await encrypt_row(env, stored), report_id)
    except Exception:
        pass


def capability_document(env=None):
    """The self-describing GET body.

    A client that has only the origin should be able to discover the shape of
    a report without a round trip through the docs site, and an operator
    reading a proxy log should be able to see what this endpoint accepts.
    """
    target = issue_repo(env)
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
        },
        "limits": {
            "bodyBytes": MAX_BODY_BYTES,
            "perReporterPerHour": RATE_LIMIT_MAX,
        },
        "filesIssueIn": "/".join(target) if target else "",
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
    record = normalize_report(data)
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
    report_id, stored = await store_report(
        env, record, reporter_bi, account)
    target = issue_repo(env)
    issue_number = await file_issue(
        env, record, account, report_id, target) if target else 0
    if issue_number > 0:
        await record_issue_number(
            env, stored, report_id, target, issue_number)
    return json_response(
        {"ok": True, "id": report_id, "status": "new",
         "credited": bool(account), "issueNumber": issue_number},
        status=201, cache_control=no_store)
