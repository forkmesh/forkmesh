"""Internal product dashboard served at /product-analysis.

Loaded on demand (entry.py holds a ``_LazyModule`` proxy) so Cloudflare's
startup-memory validation never pays for it. Everything here is read-only
reporting for the founders: how many accounts exist, which acquisition
channel they came through, whether they created a mirror, and whether
anything is being cloned. There is no configuration surface and no way to
change data from this page.

Access
------
The route is protected by a username and password held in the Worker's
``PRODUCT_ANALYSIS_USER`` / ``PRODUCT_ANALYSIS_PASS`` secrets, never in the
repository. With either unset the route does not exist: the handler returns
``None`` and the caller serves its ordinary 404. A correct login mints a
signed, HttpOnly, path-scoped cookie whose HMAC key is derived from the
password, so rotating the password ends every session at once. Failed logins
spend the same per-source budget as account logins (``login_attempts``), and
a locked source receives 429 before the password is ever compared.

A signed-in platform administrator (a live account session whose account is
``is_admin``, checked by entry.py's ``_check_admin_page_auth``) is also let
in without the founder password, because the page is one section of the
unified admin. Their session is the account's own; this route mints nothing
for them. For everyone else the secrets remain the whole access model.

Appearance
----------
The page renders inside the shared admin shell (admin_shell.py): the same
sidebar, palette and components as every other admin section, with the
shell's one theme cookie. A founder signed in with the password rather than
an admin account sees only this section in the sidebar, so the page never
reveals the console path to them.

Data correctness
----------------
Every table is drained completely with keyset pagination and the collected
row count is checked against ``COUNT(*)`` before anything is rendered; a
mismatch renders an error where the number would have been. ``signup_origin``
is backfilled once from real signals and 'unknown' stays 'unknown' rather
than being guessed into a bucket. Day buckets use
``PRODUCT_ANALYSIS_TIMEZONE`` (UTC), matching every timestamp the Worker
stores.

The runtime primitives this module needs from entry.py arrive through
``_bind_runtime``: ``Date``, ``Response``, ``d1_all``, ``d1_first``,
``d1_run``, ``decrypt_row``, ``blind_index``, ``ensure_schema``,
``method_name``, ``_transient_client_address``, ``_login_locked_until``,
``_login_record_fail``, ``_login_clear``, ``PRODUCT_ANALYSIS_PATH``,
``_admin_shell`` and, when present, ``_check_admin_page_auth`` and
``_account_cookie_name``.
"""

import calendar
import hmac
import time
from urllib.parse import parse_qs, quote, urlparse

from hidden_unicode import clean_query as clean_hidden_query


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


PRODUCT_ANALYSIS_TIMEZONE = "UTC"
PRODUCT_ANALYSIS_SESSION_COOKIE = "forkmesh_product_analysis"
PRODUCT_ANALYSIS_SESSION_TTL_MS = 12 * 60 * 60 * 1000
PRODUCT_ANALYSIS_SESSION_CONTEXT = "forkmesh-product-analysis-session-v1"
PRODUCT_ANALYSIS_LOGIN_CONTEXT = "product-analysis-login:"
PRODUCT_ANALYSIS_GLOBAL_LOGIN_KEY = "product-analysis-login-global:v1"
PRODUCT_ANALYSIS_GLOBAL_MAX_FAILS = 200
PRODUCT_ANALYSIS_GLOBAL_WINDOW_MS = 60 * 60 * 1000
# Rows per D1 read while draining a table. Deliberately small so the tests can
# prove that a report larger than one page is still complete.
PRODUCT_ANALYSIS_SCAN_PAGE = 200
PRODUCT_ANALYSIS_TABLE_PAGE = 50
PRODUCT_ANALYSIS_MIN_CHART_POINTS = 7
PRODUCT_ANALYSIS_MAX_FORM_BYTES = 4096
# Bump the version to re-run the one-time signup_origin pass after the
# derivation rules change (for example once the job-flow windows are known).
PRODUCT_ANALYSIS_BACKFILL_MARKER = "product_analysis_signup_origin_backfill:v1"
# (start_ms, end_ms) UTC windows during which account creation was part of a
# job application. None are known yet, so no account is classified as job
# flow; the founders supply the windows and www/docs/admin-dashboard.md
# records them.
PRODUCT_ANALYSIS_JOB_FLOW_WINDOWS = ()
WINDOW_DAYS = 30
SIGNUP_CHART_DAYS = 90
ACTIVATION_DAYS = 7
DAY_MS = 86_400_000
SIGNUP_ORIGINS = ("job_flow", "referral", "organic", "unknown")
SIGNUP_ORIGIN_LABELS = {
    "job_flow": "Job flow",
    "referral": "Referral programme",
    "organic": "Organic",
    "unknown": "Unknown",
}
VIEWS = (
    ("overview", "Overview"),
    ("sources", "Signup sources"),
    ("users", "Users"),
    ("repositories", "Repositories"),
    ("acquisition", "Acquisition"),
)
VIEW_KEYS = tuple(key for key, _label in VIEWS)
# Views whose renderer takes nothing from the report. build_report drains and
# decrypts every users and repositories row, which is nearly all the CPU a
# request to this route spends, so a page that displays none of that must not
# pay for it: GET /product-analysis?view=acquisition exceeded the Worker CPU
# limit building a report it then discarded. A renderer named here is called
# with report=None, so adding a data read to one fails loudly in the tests
# instead of quietly reporting zeroes -- drop the view from this set in the
# same change that gives it figures to show.
VIEWS_WITHOUT_REPORT = frozenset({"acquisition"})
MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


class ProductAnalysisDataError(Exception):
    """A read did not return the rows D1 reports for that source."""


# --- small pure helpers -----------------------------------------------------

def _esc(value):
    return (
        str("" if value is None else value)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _route_path():
    return str(globals().get("PRODUCT_ANALYSIS_PATH") or "/product-analysis")


def day_key(ms):
    """UTC calendar day of a millisecond timestamp, as YYYY-MM-DD."""
    return time.strftime("%Y-%m-%d", time.gmtime(int(ms) // 1000))


def day_start_ms(key):
    """Millisecond timestamp of midnight UTC that starts the given day."""
    parsed = time.strptime(str(key), "%Y-%m-%d")
    return calendar.timegm(parsed) * 1000


def recent_days(now_ms, count):
    """The last ``count`` UTC day keys, oldest first, ending today."""
    today = day_start_ms(day_key(now_ms))
    return [day_key(today - offset * DAY_MS)
            for offset in range(count - 1, -1, -1)]


def short_day_label(key):
    parts = str(key).split("-")
    try:
        return MONTHS[int(parts[1]) - 1] + " " + str(int(parts[2]))
    except (IndexError, ValueError):
        return str(key)


def format_int(value):
    return "{:,}".format(int(value))


def format_bytes(value):
    size = max(0, _int(value))
    units = ("B", "KB", "MB", "GB", "TB")
    index = 0
    amount = float(size)
    while amount >= 1024 and index < len(units) - 1:
        amount /= 1024.0
        index += 1
    if index == 0:
        return "%d B" % size
    return "%.1f %s" % (amount, units[index])


def format_timestamp(ms):
    ms = _int(ms)
    if ms <= 0:
        return ""
    return day_key(ms)


def normalize_origin(value):
    value = str(value or "").strip().lower()
    return value if value in SIGNUP_ORIGINS else "unknown"


def account_kind(rec):
    rec = rec if isinstance(rec, dict) else {}
    kind = str(rec.get("kind") or "").strip().lower()
    if kind in ("user", "node"):
        return kind
    return "user" if rec.get("pass_hash") else "node"


def signup_time(rec):
    """Millisecond signup time: the signup marker first, then created_at."""
    rec = rec if isinstance(rec, dict) else {}
    signup = rec.get("signup") if isinstance(rec.get("signup"), dict) else {}
    for candidate in (signup.get("at"), rec.get("created_at")):
        value = _int(candidate)
        if value > 0:
            return value
    return 0


def parse_hosted_since(value):
    """Milliseconds from a catalog hostedSince value, or 0 when unusable.

    Desktop nodes publish the millisecond epoch of the first host; older or
    foreign publishers may send seconds or an ISO date. Anything else is 0,
    which the dashboard reports as "no creation date" instead of a guess.
    """
    raw = str(value or "").strip()
    if not raw:
        return 0
    if raw.isdigit():
        number = int(raw)
        if number <= 0:
            return 0
        return number * 1000 if number < 100_000_000_000 else number
    try:
        number = float(raw)
    except ValueError:
        number = None
    if number is not None:
        number = int(number)
        if number <= 0:
            return 0
        return number * 1000 if number < 100_000_000_000 else number
    try:
        return day_start_ms(raw[:10])
    except (ValueError, OverflowError):
        return 0


def mirror_group_key(rec):
    """Same grouping mirrors.py uses: shared root commit, else the name."""
    root = str((rec or {}).get("rootCommit") or "").strip().lower()
    if root:
        return "root:" + root
    return "name:" + str((rec or {}).get("name") or "").strip().lower()


def classify_signup_origin(rec, has_referral_attribution=False,
                           job_flow_windows=None):
    """Derive the acquisition channel of one account from real signals.

    referral: a referral_attributions row exists, or the signup marker names
        a referrer (the account arrived through a referral link, whatever
        the credit decision later was).
    job_flow: the signup fell inside a configured job-posting window. No
        window is configured yet, so nothing lands here.
    organic: the signup marker was captured and positively recorded no
        referrer. Accounts older than the marker (introduced 2026-08-12)
        cannot make this claim.
    unknown: everything else, never guessed.
    """
    rec = rec if isinstance(rec, dict) else {}
    signup = rec.get("signup") if isinstance(rec.get("signup"), dict) else {}
    referrer = str(signup.get("referrer") or "").strip()
    if has_referral_attribution or referrer:
        return "referral"
    windows = (PRODUCT_ANALYSIS_JOB_FLOW_WINDOWS
               if job_flow_windows is None else job_flow_windows)
    created = signup_time(rec)
    for start, end in windows:
        if created and int(start) <= created < int(end):
            return "job_flow"
    if signup.get("referralCaptured") is True and "referrer" in signup:
        return "organic"
    return "unknown"


def window_counts(timestamps, now_ms, days=WINDOW_DAYS):
    """(count in the last ``days`` days, count in the ``days`` before that)."""
    span = days * DAY_MS
    current_start = now_ms - span
    previous_start = now_ms - 2 * span
    current = previous = 0
    for value in timestamps:
        value = _int(value)
        if value <= 0:
            continue
        if current_start <= value < now_ms:
            current += 1
        elif previous_start <= value < current_start:
            previous += 1
    return current, previous


def change_sentence(current, previous, window_label="30 days",
                    cumulative=True):
    """Plain-English comparison of two equal windows, never a bare 0%."""
    current = int(current)
    previous = int(previous)
    if current == 0 and previous == 0:
        return "None in the last %s or the %s before." % (
            window_label, window_label)
    delta = current - previous
    if cumulative:
        head = "+%s in the last %s" % (format_int(current), window_label)
    else:
        head = "%s in the last %s" % (format_int(current), window_label)
    if delta == 0:
        return head + ", the same as the previous %s." % window_label
    if delta > 0:
        return head + ", %s more than the previous %s." % (
            format_int(delta), window_label)
    return head + ", %s fewer than the previous %s." % (
        format_int(-delta), window_label)


# --- credentials, sessions, throttling ---------------------------------------

def product_analysis_credentials(env):
    """The configured (user, password) pair, or None when the route is off."""
    user = str(getattr(env, "PRODUCT_ANALYSIS_USER", "") or "").strip()
    password = str(getattr(env, "PRODUCT_ANALYSIS_PASS", "") or "")
    if not user or not password:
        return None
    return user, password


def _credentials_match(credentials, user, password):
    expected_user, expected_password = credentials
    user_ok = hmac.compare_digest(
        str(user or "").encode("utf-8"), expected_user.encode("utf-8"))
    password_ok = hmac.compare_digest(
        str(password or "").encode("utf-8"),
        expected_password.encode("utf-8"))
    return bool(user_ok & password_ok)


def _session_secret(env, credentials):
    _user, password = credentials
    return (password + "|" + str(getattr(env, "DATA_KEY", "") or "")).encode(
        "utf-8")


def _session_signature(env, credentials, expires_ms):
    user, _password = credentials
    canonical = (PRODUCT_ANALYSIS_SESSION_CONTEXT + "\n" + user + "\n"
                 + str(int(expires_ms))).encode("utf-8")
    return hmac.new(_session_secret(env, credentials), canonical,
                    "sha256").hexdigest()


def session_cookie(env, credentials, now_ms):
    expires = int(now_ms) + PRODUCT_ANALYSIS_SESSION_TTL_MS
    return "%s=%d.%s; Path=%s; Max-Age=%d; HttpOnly; Secure; SameSite=Strict" % (
        PRODUCT_ANALYSIS_SESSION_COOKIE, expires,
        _session_signature(env, credentials, expires), _route_path(),
        PRODUCT_ANALYSIS_SESSION_TTL_MS // 1000)


def clear_session_cookie():
    return "%s=; Path=%s; Max-Age=0; HttpOnly; Secure; SameSite=Strict" % (
        PRODUCT_ANALYSIS_SESSION_COOKIE, _route_path())


def _cookie_value(request, name):
    try:
        header = request.headers.get("cookie") or ""
    except Exception:
        header = ""
    for part in str(header).split(";"):
        key, sep, value = part.strip().partition("=")
        if sep and key == name:
            return value.strip()
    return ""


def session_valid(env, request, credentials, now_ms):
    token = _cookie_value(request, PRODUCT_ANALYSIS_SESSION_COOKIE)
    expires_text, sep, signature = token.partition(".")
    if not sep or not expires_text.isdigit():
        return False
    expires = int(expires_text)
    if expires <= int(now_ms):
        return False
    if expires > int(now_ms) + PRODUCT_ANALYSIS_SESSION_TTL_MS:
        return False
    expected = _session_signature(env, credentials, expires)
    return hmac.compare_digest(signature, expected)


async def _login_budget_key(env, request):
    source = _client_source_bucket(_transient_client_address(request))
    source_bi = await blind_index(env, source) if source else ""
    return await blind_index(
        env, PRODUCT_ANALYSIS_LOGIN_CONTEXT + (source_bi or "local"))


async def _global_login_locked_until(env, key, now):
    row = await d1_first(
        env, "SELECT fails,first_fail_ts FROM login_attempts WHERE id_bi=?",
        key)
    fails = _int((row or {}).get("fails"))
    started = _int((row or {}).get("first_fail_ts"))
    until = started + PRODUCT_ANALYSIS_GLOBAL_WINDOW_MS
    if fails >= PRODUCT_ANALYSIS_GLOBAL_MAX_FAILS and until > int(now):
        return until
    return 0


async def _global_login_record_fail(env, key, now):
    window = PRODUCT_ANALYSIS_GLOBAL_WINDOW_MS
    await d1_run(
        env,
        "INSERT INTO login_attempts (id_bi,fails,first_fail_ts,locked_until) "
        "VALUES (?,1,?,0) ON CONFLICT(id_bi) DO UPDATE SET "
        "fails=CASE WHEN ?-login_attempts.first_fail_ts>=? "
        "THEN 1 ELSE login_attempts.fails+1 END,"
        "first_fail_ts=CASE WHEN ?-login_attempts.first_fail_ts>=? "
        "THEN ? ELSE login_attempts.first_fail_ts END,locked_until=0",
        key, int(now), int(now), window, int(now), window, int(now))


# --- responses ----------------------------------------------------------------

_PAGE_CSP = (
    "default-src 'none'; style-src 'unsafe-inline'; img-src 'self' data:; "
    "form-action 'self'; base-uri 'none'; frame-ancestors 'none'"
)


def _html_headers(extra=None):
    headers = {
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store, max-age=0, must-revalidate",
        "content-security-policy": _PAGE_CSP,
        "x-robots-tag": "noindex, nofollow",
        "x-frame-options": "DENY",
        "x-content-type-options": "nosniff",
        "referrer-policy": "no-referrer",
    }
    if extra:
        headers.update(extra)
    return headers


def _redirect(cookie, location=None):
    return Response("", status=303, headers={
        "location": location or _route_path(),
        "set-cookie": cookie,
        "cache-control": "no-store, max-age=0, must-revalidate",
        "x-robots-tag": "noindex, nofollow",
    })


def _login_response(now_ms, failed=False, locked_until=0, head=False,
                    theme="system"):
    locked_until = _int(locked_until)
    status = 200
    headers = {}
    if locked_until > int(now_ms):
        status = 429
        retry_seconds = max(1, (locked_until - int(now_ms) + 999) // 1000)
        headers["retry-after"] = str(retry_seconds)
        message = ("Too many failed attempts. Try again in about %d minute%s."
                   % (max(1, (retry_seconds + 59) // 60),
                      "" if retry_seconds <= 60 else "s"))
    elif failed:
        status = 401
        message = "That username and password did not match."
    else:
        message = ""
    body = "" if head else render_login_html(message, theme)
    return Response(body, status=status, headers=_html_headers(headers))


# --- request handling ---------------------------------------------------------

async def admin_viewer(env, request):
    """The platform administrator viewing this page, or ``""``.

    Uses the console's own live check (account session + ``is_admin``), so
    signing out or losing the role takes effect on the next request.
    """
    check = globals().get("_check_admin_page_auth")
    if check is None:
        return ""
    try:
        if not await check(env, request):
            return ""
        name = globals().get("_account_cookie_name")
        return str((await name(env, request)) if name else "") or "admin"
    except Exception:
        return ""


async def product_analysis_handler(env, request):
    """Serve /product-analysis. Returns None when the caller should 404."""
    url = urlparse(request.url)
    if url.path.rstrip("/") != _route_path():
        return None
    method = method_name(request)
    if method not in ("GET", "HEAD", "POST"):
        return None
    credentials = product_analysis_credentials(env)
    admin = await admin_viewer(env, request)
    if credentials is None and not admin:
        return None
    await ensure_schema(env)
    now = int(Date.now())
    theme = _admin_shell.theme_from_request(request)
    if method == "POST":
        if credentials is None:
            return None
        return await _handle_post(env, request, credentials, now, theme)
    head = method == "HEAD"
    founder = bool(credentials) and session_valid(
        env, request, credentials, now)
    if not admin and not founder:
        return _login_response(now, head=head, theme=theme)
    params = parse_qs(url.query, keep_blank_values=True)
    view = (params.get("view") or ["overview"])[0]
    if view not in VIEW_KEYS:
        return None
    # A HEAD response carries no body, so nothing would ever read its report.
    if head or view in VIEWS_WITHOUT_REPORT:
        report = None
    else:
        report = await build_report(env, now)
    body = "" if head else render_dashboard_html(
        view, report, params, theme, admin=admin,
        console=_admin_shell.console_base(env) if admin else "",
        badges=(await _admin_shell.nav_badges(env)) if admin else None,
        founder_session=founder,
        subpages=_admin_shell.subpage_base(env) if admin else "/admin")
    return Response(body, status=200, headers=_html_headers())


async def _handle_post(env, request, credentials, now, theme="system"):
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = clean_hidden_query(parse_qs(str(raw or "")[:PRODUCT_ANALYSIS_MAX_FORM_BYTES],
                    keep_blank_values=True))
    action = (form.get("action") or [""])[0]
    if action == "logout":
        return _redirect(clear_session_cookie())
    if action != "login":
        return None
    budget_key = await _login_budget_key(env, request)
    global_key = await blind_index(env, PRODUCT_ANALYSIS_GLOBAL_LOGIN_KEY)
    locked_until = max(
        _int(await _login_locked_until(env, budget_key)),
        await _global_login_locked_until(env, global_key, now))
    if locked_until > now:
        return _login_response(now, locked_until=locked_until, theme=theme)
    user = (form.get("username") or [""])[0]
    password = (form.get("password") or [""])[0]
    if _credentials_match(credentials, user, password):
        await _login_clear(env, budget_key)
        return _redirect(session_cookie(env, credentials, now))
    result = await _login_record_fail(env, budget_key)
    await _global_login_record_fail(env, global_key, now)
    locked_until = _int(result.get("lockedUntil")) if isinstance(
        result, dict) else _int(result)
    return _login_response(now, failed=True, locked_until=locked_until,
                           theme=theme)


# --- complete, verified reads ------------------------------------------------

async def scan_table(env, columns, table, key, where="", args=(),
                     key_field=None, page_size=None):
    """Drain ``table`` completely, then prove the drain matched COUNT(*).

    Keyset pagination on ``key`` (a unique, non-null column) walks the table
    ``page_size`` rows at a time until a short page arrives, so no default
    page size can silently cap an aggregate. The final count check turns a
    partial read into an error instead of a plausible number.
    """
    page_size = int(page_size or PRODUCT_ANALYSIS_SCAN_PAGE)
    key_field = key_field or key
    rows = []
    cursor = None
    while True:
        clauses = ["(" + where + ")"] if where else []
        params = list(args)
        if cursor is not None:
            clauses.append(key + ">?")
            params.append(cursor)
        sql = "SELECT " + columns + " FROM " + table
        if clauses:
            sql += " WHERE " + " AND ".join(clauses)
        sql += " ORDER BY " + key + " LIMIT ?"
        params.append(page_size)
        page = await d1_all(env, sql, *params) or []
        rows.extend(page)
        if len(page) < page_size:
            break
        cursor = page[-1].get(key_field)
        if cursor is None:
            raise ProductAnalysisDataError(
                table + ": a page arrived without its cursor column")
    count_sql = "SELECT COUNT(*) AS n FROM " + table
    if where:
        count_sql += " WHERE (" + where + ")"
    total_row = await d1_first(env, count_sql, *args)
    total = _int((total_row or {}).get("n"))
    if len(rows) != total:
        raise ProductAnalysisDataError(
            "%s: collected %d rows but D1 reports %d" % (
                table, len(rows), total))
    return rows


async def grouped_rows(env, sql, count_sql, args=(), label="query"):
    """One aggregate read whose group count is checked against a COUNT."""
    rows = await d1_all(env, sql, *args) or []
    total_row = await d1_first(env, count_sql, *args)
    total = _int((total_row or {}).get("n"))
    if len(rows) != total:
        raise ProductAnalysisDataError(
            "%s: collected %d groups but D1 reports %d" % (
                label, len(rows), total))
    return rows


async def collect_accounts(env):
    rows = await scan_table(
        env, "user_bi,username,is_admin,signup_origin,data", "users",
        "user_bi")
    accounts = []
    undecryptable = 0
    for row in rows:
        rec = await decrypt_row(env, row.get("data"))
        if not isinstance(rec, dict):
            undecryptable += 1
            continue
        if account_kind(rec) != "user":
            continue
        accounts.append({
            "user_bi": str(row.get("user_bi") or ""),
            "name": str(row.get("username") or rec.get("name") or ""
                        ).strip().lower(),
            "created_at": signup_time(rec),
            "origin": normalize_origin(row.get("signup_origin")),
            "status": str(rec.get("status") or "").strip().lower(),
            "node_names": [
                str(node).strip().lower()
                for node in (rec.get("nodes") or [])
                if isinstance(node, str) and node.strip()],
            "rec": rec,
            "mirrors": 0,
            "first_mirror_at": 0,
            "last_activity": 0,
        })
    return accounts, undecryptable


async def collect_repositories(env):
    rows = await scan_table(env, "key_bi,owner_bi,data", "repositories",
                            "key_bi")
    repositories = []
    undecryptable = 0
    for row in rows:
        rec = await decrypt_row(env, row.get("data"))
        if not isinstance(rec, dict):
            undecryptable += 1
            continue
        owner = str(rec.get("owner") or "").strip().lower()
        name = str(rec.get("name") or "").strip().lower()
        repositories.append({
            "key_bi": str(row.get("key_bi") or ""),
            "owner_bi": str(row.get("owner_bi") or ""),
            "owner": owner,
            "name": name,
            "visibility": (
                "public" if rec.get("visibility") == "public" else "private"),
            "size": max(0, _int(rec.get("sizeBytes"))),
            "created_at": parse_hosted_since(rec.get("hostedSince")),
            "group": mirror_group_key(rec),
            "mirror_count": 1,
            "clones_30d": 0,
            "clones_total": 0,
            "owner_name": owner,
        })
    return repositories, undecryptable


async def collect_nodes(env):
    rows = await scan_table(env, "node_bi,user_bi,name,last_seen", "nodes",
                            "node_bi")
    return {
        str(row.get("node_bi") or ""): {
            "user_bi": str(row.get("user_bi") or ""),
            "name": str(row.get("name") or "").strip().lower(),
            "last_seen": _int(row.get("last_seen")),
        }
        for row in rows
    }


async def collect_referral_attributions(env):
    rows = await scan_table(env, "referred_bi", "referral_attributions",
                            "referred_bi")
    return {str(row.get("referred_bi") or "") for row in rows}


async def collect_session_activity(env):
    rows = await grouped_rows(
        env,
        "SELECT account_bi, MAX(last_seen_at) AS last_seen_at "
        "FROM account_sessions GROUP BY account_bi",
        "SELECT COUNT(DISTINCT account_bi) AS n FROM account_sessions",
        label="account_sessions")
    return {str(row.get("account_bi") or ""): _int(row.get("last_seen_at"))
            for row in rows}


async def collect_presence(env):
    rows = await scan_table(env, "name_bi,ts", "account_presence", "name_bi")
    return {str(row.get("name_bi") or ""): _int(row.get("ts")) for row in rows}


async def collect_clone_totals(env):
    rows = await grouped_rows(
        env,
        "SELECT owner, repo, SUM(clones) AS clones FROM mirror_serve_counters "
        "GROUP BY owner, repo",
        "SELECT COUNT(*) AS n FROM "
        "(SELECT 1 FROM mirror_serve_counters GROUP BY owner, repo)",
        label="mirror_serve_counters")
    return {
        (str(row.get("owner") or "").lower(), str(row.get("repo") or "").lower()):
        _int(row.get("clones"))
        for row in rows
    }


async def collect_clone_daily(env, since_day):
    rows = await scan_table(
        env, "rowid AS rid,day,owner,repo,clones", "product_clone_daily",
        "rowid", where="day>=?", args=(since_day,), key_field="rid")
    first = await d1_first(
        env, "SELECT MIN(day) AS day FROM product_clone_daily")
    first_day = str((first or {}).get("day") or "")
    return rows, first_day


async def ensure_signup_origin_backfill(env, accounts, attributions):
    """Classify existing accounts once from signals SQL cannot read.

    Migration 0175 already stamped the referral rule that is expressible in
    SQL. This pass reads the referral marker inside each encrypted record and
    writes what it can prove. It never downgrades a known origin to unknown,
    and it records a schema_meta marker so it runs once per rule version.
    """
    row = await d1_first(
        env, "SELECT v FROM schema_meta WHERE k=?",
        PRODUCT_ANALYSIS_BACKFILL_MARKER)
    if row and str(row.get("v") or "") == "applied":
        return {"ran": False, "updated": 0, "classified": {}}
    updated = 0
    classified = {}
    for account in accounts:
        computed = classify_signup_origin(
            account.get("rec"), account["user_bi"] in attributions)
        classified[computed] = classified.get(computed, 0) + 1
        if computed != "unknown" and computed != account["origin"]:
            await d1_run(
                env, "UPDATE users SET signup_origin=? WHERE user_bi=?",
                computed, account["user_bi"])
            account["origin"] = computed
            updated += 1
    await d1_run(
        env,
        "INSERT INTO schema_meta (k,v) VALUES (?,?) "
        "ON CONFLICT(k) DO UPDATE SET v=excluded.v",
        PRODUCT_ANALYSIS_BACKFILL_MARKER, "applied")
    return {"ran": True, "updated": updated, "classified": classified}


def _describe_error(error):
    text = str(error).strip() or error.__class__.__name__
    if isinstance(error, ProductAnalysisDataError):
        return text
    return error.__class__.__name__ + ": " + text


async def build_report(env, now_ms):
    """Read every source, verify it, and derive what the views render.

    Each source fails independently: a broken read blanks only the figures
    that depend on it, with the failure recorded under ``errors`` so the page
    can say what failed and what to check.
    """
    report = {
        "now": int(now_ms),
        "errors": {},
        "notes": [],
        "backfill": None,
        "accounts": None,
        "repositories": None,
        "clone_daily": None,
        "clone_first_day": "",
        "clone_totals": None,
    }
    accounts = None
    attributions = None
    nodes = None
    sessions = None
    presence = None
    repositories = None

    try:
        accounts, bad = await collect_accounts(env)
        if bad:
            report["notes"].append(
                "%d account rows could not be decrypted and are left out."
                % bad)
    except Exception as error:
        report["errors"]["accounts"] = _describe_error(error)
    try:
        attributions = await collect_referral_attributions(env)
    except Exception as error:
        report["errors"]["referral_attributions"] = _describe_error(error)
    try:
        nodes = await collect_nodes(env)
    except Exception as error:
        report["errors"]["nodes"] = _describe_error(error)
    try:
        sessions = await collect_session_activity(env)
    except Exception as error:
        report["errors"]["account_sessions"] = _describe_error(error)
    try:
        presence = await collect_presence(env)
    except Exception as error:
        report["errors"]["account_presence"] = _describe_error(error)
    try:
        repositories, bad = await collect_repositories(env)
        if bad:
            report["notes"].append(
                "%d repository rows could not be decrypted and are left out."
                % bad)
    except Exception as error:
        report["errors"]["repositories"] = _describe_error(error)
    try:
        report["clone_totals"] = await collect_clone_totals(env)
    except Exception as error:
        report["errors"]["mirror_serve_counters"] = _describe_error(error)
    try:
        since = recent_days(now_ms, 2 * WINDOW_DAYS)[0]
        rows, first_day = await collect_clone_daily(env, since)
        report["clone_daily"] = rows
        report["clone_first_day"] = first_day
    except Exception as error:
        report["errors"]["product_clone_daily"] = _describe_error(error)

    if accounts is not None and attributions is not None:
        try:
            report["backfill"] = await ensure_signup_origin_backfill(
                env, accounts, attributions)
        except Exception as error:
            report["errors"]["signup_origin_backfill"] = _describe_error(error)

    if accounts is not None:
        owner_index = {}
        for account in accounts:
            account.pop("rec", None)
            owner_index[account["user_bi"]] = account
            for node_name in account["node_names"]:
                node_bi = await blind_index(env, node_name)
                owner_index.setdefault(node_bi, account)
        by_user_bi = {account["user_bi"]: account for account in accounts}
        if nodes is not None:
            for node_bi, node in nodes.items():
                owner = by_user_bi.get(node["user_bi"])
                if owner is None:
                    owner = owner_index.get(node_bi)
                if owner is None:
                    continue
                owner_index.setdefault(node_bi, owner)
                owner["last_activity"] = max(
                    owner["last_activity"], node["last_seen"])
        if sessions is not None:
            for account in accounts:
                account["last_activity"] = max(
                    account["last_activity"],
                    sessions.get(account["user_bi"], 0))
        if presence is not None:
            for account in accounts:
                account["last_activity"] = max(
                    account["last_activity"],
                    presence.get(account["user_bi"], 0))
        report["activity_available"] = (
            sessions is not None or presence is not None or nodes is not None)
        if repositories is not None:
            for repository in repositories:
                owner = owner_index.get(repository["owner_bi"])
                if owner is None:
                    continue
                repository["owner_name"] = owner["name"] or repository["owner"]
                owner["mirrors"] += 1
                created = repository["created_at"]
                if created and (not owner["first_mirror_at"]
                                or created < owner["first_mirror_at"]):
                    owner["first_mirror_at"] = created
    if repositories is not None:
        groups = {}
        for repository in repositories:
            groups[repository["group"]] = groups.get(repository["group"], 0) + 1
        for repository in repositories:
            repository["mirror_count"] = groups.get(repository["group"], 1)
        totals = report["clone_totals"] or {}
        for repository in repositories:
            repository["clones_total"] = totals.get(
                (repository["owner"], repository["name"]), 0)
        if report["clone_daily"] is not None:
            window = set(recent_days(now_ms, WINDOW_DAYS))
            per_repo = {}
            for row in report["clone_daily"]:
                if str(row.get("day") or "") in window:
                    pair = (str(row.get("owner") or "").lower(),
                            str(row.get("repo") or "").lower())
                    per_repo[pair] = per_repo.get(pair, 0) + _int(
                        row.get("clones"))
            for repository in repositories:
                repository["clones_30d"] = per_repo.get(
                    (repository["owner"], repository["name"]), 0)
    report["accounts"] = accounts
    report["repositories"] = repositories
    return report


# --- derived figures ------------------------------------------------------------

def clone_window_totals(report, days=WINDOW_DAYS):
    """Clones and distinct repositories per window from the daily table.

    Returns None when the daily table could not be read. ``coverage`` says
    whether the previous window has any recorded days at all, so the view
    can decline the comparison rather than print a zero.
    """
    rows = report.get("clone_daily")
    if rows is None:
        return None
    now = report["now"]
    current_days = set(recent_days(now, days))
    previous_days = set(recent_days(now, 2 * days)) - current_days
    current = {"clones": 0, "repos": set()}
    previous = {"clones": 0, "repos": set()}
    for row in rows:
        day = str(row.get("day") or "")
        pair = (str(row.get("owner") or "").lower(),
                str(row.get("repo") or "").lower())
        count = _int(row.get("clones"))
        bucket = None
        if day in current_days:
            bucket = current
        elif day in previous_days:
            bucket = previous
        if bucket is None or count <= 0:
            continue
        bucket["clones"] += count
        bucket["repos"].add(pair)
    first_day = report.get("clone_first_day") or ""
    return {
        "current_clones": current["clones"],
        "previous_clones": previous["clones"],
        "current_repos": len(current["repos"]),
        "previous_repos": len(previous["repos"]),
        "first_day": first_day,
        "has_data": bool(first_day),
        "previous_covered": bool(first_day) and first_day <= min(previous_days),
        "current_covered": bool(first_day) and first_day <= min(current_days),
    }


def overview_figures(report):
    now = report["now"]
    figures = []
    accounts = report.get("accounts")
    repositories = report.get("repositories")

    if accounts is None:
        error = report["errors"].get("accounts", "accounts unavailable")
        figures.append({"label": "Total accounts", "window": "All time",
                        "error": error})
        figures.append({"label": "Accounts with at least one mirror",
                        "window": "All time", "error": error})
    else:
        current, previous = window_counts(
            [account["created_at"] for account in accounts], now)
        figures.append({
            "label": "Total accounts",
            "value": format_int(len(accounts)),
            "window": "All time",
            "change": change_sentence(current, previous),
        })
        activated = [account for account in accounts if account["mirrors"]]
        current, previous = window_counts(
            [account["first_mirror_at"] for account in activated], now)
        undated = sum(1 for account in activated
                      if not account["first_mirror_at"])
        change = change_sentence(current, previous)
        if undated:
            change += (" %d of these accounts have a mirror without a "
                       "creation date and are not in the window counts."
                       % undated)
        figure = {
            "label": "Accounts with at least one mirror",
            "value": format_int(len(activated)),
            "window": "All time",
            "change": change,
        }
        if repositories is None:
            figure["error"] = report["errors"].get(
                "repositories", "repositories unavailable")
            figure.pop("value", None)
        figures.append(figure)
        figures.append(activation_figure(report))
        figures.append(retention_figure(report))

    if repositories is None:
        figures.append({"label": "Mirrors created", "window": "All time",
                        "error": report["errors"].get(
                            "repositories", "repositories unavailable")})
    else:
        current, previous = window_counts(
            [repository["created_at"] for repository in repositories], now)
        undated = sum(1 for repository in repositories
                      if not repository["created_at"])
        change = change_sentence(current, previous)
        if undated:
            change += (" %d mirrors carry no creation date and are not in "
                       "the window counts." % undated)
        figures.append({
            "label": "Mirrors created",
            "value": format_int(len(repositories)),
            "window": "All time",
            "change": change,
        })

    clones = clone_window_totals(report)
    if clones is None:
        error = report["errors"].get("product_clone_daily",
                                     "clone counting unavailable")
        figures.append({"label": "Repositories cloned",
                        "window": "Last 30 days", "error": error})
        figures.append({"label": "Anonymous clones",
                        "window": "Last 30 days", "error": error})
    elif not clones["has_data"]:
        note = ("No clone data yet. Per-day clone counting starts with the "
                "deploy that added this dashboard.")
        figures.append({"label": "Repositories cloned",
                        "window": "Last 30 days", "empty": note})
        figures.append({"label": "Anonymous clones",
                        "window": "Last 30 days", "empty": note})
    else:
        if clones["previous_covered"]:
            repo_change = change_sentence(
                clones["current_repos"], clones["previous_repos"],
                cumulative=False)
            clone_change = change_sentence(
                clones["current_clones"], clones["previous_clones"],
                cumulative=False)
        else:
            repo_change = clone_change = (
                "No comparison: counting started on %s, so the previous "
                "30 days have no data." % clones["first_day"])
        if not clones["current_covered"]:
            partial = (" This window is only covered from %s."
                       % clones["first_day"])
            repo_change += partial
            clone_change += partial
        figures.append({
            "label": "Repositories cloned",
            "value": format_int(clones["current_repos"]),
            "window": "Last 30 days, repositories with at least one clone",
            "change": repo_change,
        })
        figures.append({
            "label": "Anonymous clones",
            "value": format_int(clones["current_clones"]),
            "window": "Last 30 days, public HTTPS mirror route",
            "change": clone_change,
        })

    totals = report.get("clone_totals")
    if totals is None:
        figures.append({"label": "Clones", "window": "All time",
                        "error": report["errors"].get(
                            "mirror_serve_counters",
                            "serve counters unavailable")})
    else:
        figures.append({
            "label": "Clones",
            "value": format_int(sum(totals.values())),
            "window": "All time, cumulative mirror serve counters",
            "change": "Cumulative counter; it has no previous period.",
        })
    return figures


def activation_counts(accounts, start_ms, end_ms):
    cohort = [account for account in accounts
              if start_ms <= _int(account["created_at"]) < end_ms]
    limit = ACTIVATION_DAYS * DAY_MS
    activated = sum(
        1 for account in cohort
        if account["mirrors"] and account["first_mirror_at"]
        and account["first_mirror_at"] - account["created_at"] <= limit)
    return activated, len(cohort)


def activation_figure(report):
    label = "New accounts activated"
    window = ("Accounts created in the last 30 days that published a first "
              "mirror within %d days of signup" % ACTIVATION_DAYS)
    if report.get("repositories") is None:
        return {"label": label, "window": window,
                "error": report["errors"].get(
                    "repositories", "repositories unavailable")}
    now = report["now"]
    span = WINDOW_DAYS * DAY_MS
    current, cohort = activation_counts(report["accounts"], now - span, now)
    previous, previous_cohort = activation_counts(
        report["accounts"], now - 2 * span, now - span)
    change = ("%s of %s new accounts; %s of %s in the previous 30 days."
              % (format_int(current), format_int(cohort),
                 format_int(previous), format_int(previous_cohort)))
    recent = sum(1 for account in report["accounts"]
                 if now - ACTIVATION_DAYS * DAY_MS
                 <= _int(account["created_at"]) < now)
    if recent:
        change += (" %s of the new accounts are younger than %d days and "
                   "can still activate." % (format_int(recent),
                                            ACTIVATION_DAYS))
    return {"label": label, "value": format_int(current), "window": window,
            "change": change}


def retention_figure(report):
    label = "Returning accounts"
    window = ("Accounts created more than 30 days ago with activity in the "
              "last 30 days")
    if not report.get("activity_available"):
        return {"label": label, "window": window,
                "error": "account activity unavailable"}
    now = report["now"]
    start = now - WINDOW_DAYS * DAY_MS
    mature = [account for account in report["accounts"]
              if 0 < _int(account["created_at"]) < start]
    returning = sum(1 for account in mature
                    if _int(account["last_activity"]) >= start)
    return {"label": label, "value": format_int(returning), "window": window,
            "change": ("%s of %s accounts older than 30 days. Only each "
                       "account's latest activity is stored, so there is no "
                       "previous period." % (format_int(returning),
                                             format_int(len(mature))))}


def daily_series(days, timestamps):
    """Count timestamps per UTC day for the given day keys."""
    index = {key: position for position, key in enumerate(days)}
    counts = [0] * len(days)
    for value in timestamps:
        value = _int(value)
        if value <= 0:
            continue
        position = index.get(day_key(value))
        if position is not None:
            counts[position] += 1
    return counts


def signup_series(report):
    accounts = report.get("accounts")
    if accounts is None:
        return None
    days = recent_days(report["now"], SIGNUP_CHART_DAYS)
    organic = daily_series(
        days, [account["created_at"] for account in accounts
               if account["origin"] == "organic"])
    other = daily_series(
        days, [account["created_at"] for account in accounts
               if account["origin"] != "organic"])
    return {"days": days, "series": [
        {"label": "Organic", "values": organic, "style": "accent"},
        {"label": "Other origins (job flow, referral, unknown)",
         "values": other, "style": "second"},
    ]}


def mirror_series(report):
    repositories = report.get("repositories")
    if repositories is None:
        return None
    days = recent_days(report["now"], SIGNUP_CHART_DAYS)
    values = daily_series(
        days, [repository["created_at"] for repository in repositories])
    return {"days": days, "series": [
        {"label": "Mirrors created", "values": values, "style": "accent"}]}


def clone_series(report):
    rows = report.get("clone_daily")
    if rows is None:
        return None
    days = recent_days(report["now"], WINDOW_DAYS)
    index = {key: position for position, key in enumerate(days)}
    values = [0] * len(days)
    for row in rows:
        position = index.get(str(row.get("day") or ""))
        if position is not None:
            values[position] += max(0, _int(row.get("clones")))
    return {"days": days, "series": [
        {"label": "Clones", "values": values, "style": "accent"}]}


def source_breakdown(report):
    accounts = report.get("accounts")
    if accounts is None:
        return None
    total = len(accounts)
    rows = []
    for origin in SIGNUP_ORIGINS:
        members = [account for account in accounts
                   if account["origin"] == origin]
        activated = sum(1 for account in members if account["mirrors"])
        rows.append({
            "origin": origin,
            "label": SIGNUP_ORIGIN_LABELS[origin],
            "count": len(members),
            "share": (100.0 * len(members) / total) if total else 0.0,
            "activated": activated,
            "activated_share": (
                (100.0 * activated / len(members)) if members else 0.0),
        })
    return {"total": total, "rows": rows,
            "activated_total": sum(row["activated"] for row in rows)}


# --- rendering --------------------------------------------------------------

# --- rendering ---------------------------------------------------------------------
# One family, one accent, GitHub's light and dark palettes as solid colours,
# shadcn's proportions for the sidebar, cards, tables, buttons and inputs.
# Everything is server-rendered: the theme switch is a form, not a script.

# The shell supplies the palette and components; these rules only cover the
# dashboard's own SVG classes and legend swatches.
_STYLE = """
.axis{stroke:var(--border-soft)}
.tick{fill:var(--muted);font-size:11px}
.bar-label{fill:var(--fg);font-size:12px}
.legend i.accent{border-top:2px solid var(--accent)}
.legend i.second{border-top:2px dashed var(--second)}
.legend .sw{width:18px;height:12px;margin-right:6px;vertical-align:middle}
.figure .c{font-size:13px;color:var(--muted);margin-top:8px}
.figure .e{font-size:13px;color:var(--danger);margin-top:8px}
.figure .empty{padding:8px 0 0}
.error{border:1px solid var(--danger);border-left-width:4px;border-radius:8px;
  padding:10px 14px;margin:0 0 16px;background:var(--danger-bg)}
.lead{color:var(--muted);margin:0 0 16px;max-width:72ch}
.login{min-height:100vh;display:flex;flex-direction:column;align-items:center;
  justify-content:center;padding:24px 16px;gap:16px}
.login .card{width:100%;max-width:380px;padding:28px}
.login h1{font-size:18px;margin:0 0 2px}
.login label{display:block;margin:14px 0 6px;font-size:13px;font-weight:500}
.login input{width:100%}
.login .btn{margin-top:20px}
.login .e{color:var(--danger);margin:12px 0 0;font-size:13px}
.login .seg{width:150px}
"""


def _brand_html():
    return ("<div class=\"ab-brand\" style=\"padding:0 0 18px\">"
            "<img src=\"%s\" alt=\"\" width=\"32\" height=\"32\">"
            "<div><b>ForkMesh</b><span>Product analytics</span></div></div>"
            % _esc(_admin_shell.LOGO))


def render_login_html(message="", theme="system"):
    shell = _admin_shell
    body = (
        "<main class=\"login\"><div class=\"card\">" + _brand_html()
        + "<h1>Sign in</h1>"
        "<p class=\"muted\" style=\"margin:0\">Internal ForkMesh dashboard. "
        "Sign in to continue.</p>"
        "<form method=\"post\" action=\"" + _esc(_route_path()) + "\">"
        "<input type=\"hidden\" name=\"action\" value=\"login\">"
        "<label for=\"pa-user\">Username</label>"
        "<input id=\"pa-user\" type=\"text\" name=\"username\" "
        "autocomplete=\"username\" required autofocus>"
        "<label for=\"pa-pass\">Password</label>"
        "<input id=\"pa-pass\" type=\"password\" name=\"password\" "
        "autocomplete=\"current-password\" required>"
        "<button type=\"submit\" class=\"btn btn-primary btn-block\">Sign in"
        "</button>"
        + ("<p class=\"e\">" + _esc(message) + "</p>" if message else "")
        + "</form></div>" + shell.theme_control(theme, _route_path())
        + "</main>"
    )
    return shell.document("Sign in", body, theme,
                          head_extra="<style>" + _STYLE + "</style>")


def _raw_query(params, **overrides):
    """Rebuild a dashboard query string with the given overrides."""
    merged = {}
    for key in ("view", "sort", "dir", "page", "q"):
        values = params.get(key) if isinstance(params, dict) else None
        if values:
            merged[key] = str(values[0])
    for key, value in overrides.items():
        if value is None or value == "":
            merged.pop(key, None)
        else:
            merged[key] = str(value)
    return "?" + "&".join(
        quote(key) + "=" + quote(value) for key, value in merged.items())


def _query(params, **overrides):
    """The same query string, escaped for an HTML attribute."""
    return _raw_query(params, **overrides).replace("&", "&amp;")


def _page_head(title, description):
    """The view's lead line; the view name itself is the selected tab."""
    return "<p class=\"lead\">%s</p>" % _esc(description)


def _card(title, content, description="", flush=False, footer=""):
    """A bordered section: optional heading, then body padding or a flush table."""
    if flush:
        return _admin_shell.card(title=title, description=description,
                                 table=content, footer=footer)
    return _admin_shell.card(content, title=title, description=description,
                             footer=footer)


def _errors_html(report, keys=None):
    parts = []
    for key, message in sorted(report.get("errors", {}).items()):
        if keys is not None and key not in keys:
            continue
        parts.append(
            "<div class=\"error\"><strong>%s failed.</strong> %s "
            "Check D1 for replica lag or a missing migration, then reload."
            "</div>" % (_esc(key), _esc(message)))
    return "".join(parts)


def _notes_html(report):
    return "".join("<p class=\"note\">%s</p>" % _esc(note)
                   for note in report.get("notes", []))


def render_figures(figures):
    parts = []
    for figure in figures:
        block = "<div class=\"card kpi figure\">"
        if figure.get("error"):
            block += ("<div class=\"l\">%s</div><div class=\"w\">%s</div>"
                      "<div class=\"e\">Unavailable: %s</div>" % (
                          _esc(figure["label"]), _esc(figure["window"]),
                          _esc(figure["error"])))
        elif figure.get("empty"):
            block += ("<div class=\"l\">%s</div><div class=\"w\">%s</div>"
                      "<div class=\"empty\">%s</div>" % (
                          _esc(figure["label"]), _esc(figure["window"]),
                          _esc(figure["empty"])))
        else:
            block += ("<div class=\"l\">%s</div><div class=\"n\">%s</div>"
                      "<div class=\"w\">%s</div><div class=\"c\">%s</div>" % (
                          _esc(figure["label"]), _esc(figure["value"]),
                          _esc(figure["window"]),
                          _esc(figure.get("change", ""))))
        block += "</div>"
        parts.append(block)
    return "<div class=\"grid cols-3\">" + "".join(parts) + "</div>"


def _nice_ceiling(value):
    value = max(1, int(value))
    magnitude = 1
    while magnitude * 10 <= value:
        magnitude *= 10
    for step in (1, 2, 5, 10):
        if step * magnitude >= value:
            return step * magnitude
    return 10 * magnitude


def line_chart_svg(days, series, width=760, height=220):
    """Minimal line chart: one horizontal gridline set, no borders or fills.

    Each day gets an invisible column carrying a <title>, so hovering any
    point of the chart reads the exact date and values without scripts.
    """
    left, right, top, bottom = 44, 12, 12, 30
    plot_width = width - left - right
    plot_height = height - top - bottom
    count = max(1, len(days))
    peak = max([max(item["values"]) if item["values"] else 0
                for item in series] + [0])
    # Three steps above zero, each a whole number: counts never get a
    # gridline labelled "3.3".
    ceiling = 3 * _nice_ceiling((peak + 2) // 3)

    def x_at(position):
        if count == 1:
            return left + plot_width / 2.0
        return left + plot_width * position / float(count - 1)

    def y_at(value):
        return top + plot_height - plot_height * float(value) / ceiling

    parts = ["<svg viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\" "
             "role=\"img\" aria-label=\"%s\">" % (
                 width, height, width, height,
                 _esc("; ".join(item["label"] for item in series)))]
    for step in range(4):
        value = ceiling * step / 3.0
        y = y_at(value)
        parts.append(
            "<line class=\"axis\" x1=\"%d\" x2=\"%d\" y1=\"%.1f\" y2=\"%.1f\"/>"
            % (left, width - right, y, y))
        parts.append(
            "<text class=\"tick\" x=\"%d\" y=\"%.1f\" text-anchor=\"end\">%s"
            "</text>" % (left - 6, y + 4, format_int(int(round(value)))))
    tick_every = max(1, count // 6)
    for position, key in enumerate(days):
        if position % tick_every == 0 or position == count - 1:
            parts.append(
                "<text class=\"tick\" x=\"%.1f\" y=\"%d\" text-anchor=\"middle\">"
                "%s</text>" % (x_at(position), height - 10,
                               _esc(short_day_label(key))))
    for item in series:
        points = " ".join("%.1f,%.1f" % (x_at(position), y_at(value))
                          for position, value in enumerate(item["values"]))
        dash = (" stroke-dasharray=\"5 4\""
                if item.get("style") == "second" else "")
        parts.append(
            "<polyline class=\"s-%s\" fill=\"none\" stroke-width=\"2\"%s "
            "points=\"%s\"/>" % (item.get("style", "accent"), dash, points))
    column = plot_width / float(count)
    for position, key in enumerate(days):
        lines = [key] + ["%s: %s" % (item["label"], format_int(
            item["values"][position])) for item in series]
        parts.append(
            "<rect x=\"%.1f\" y=\"%d\" width=\"%.1f\" height=\"%d\" "
            "fill=\"transparent\"><title>%s</title></rect>" % (
                x_at(position) - column / 2.0, top, column, plot_height,
                _esc("\n".join(lines))))
    parts.append("</svg>")
    return "".join(parts)


def _legend(items):
    """Legend swatches: a solid or dashed rule, or the hatch pattern itself."""
    parts = []
    for style, label in items:
        if style == "hatch":
            swatch = ("<svg class=\"sw\" viewBox=\"0 0 18 12\" aria-hidden=\"true\">"
                      "<rect x=\"0.5\" y=\"0.5\" width=\"17\" height=\"11\" "
                      "fill=\"url(#pa-hatch)\" class=\"s-second\"/></svg>")
        else:
            swatch = "<i class=\"%s\"></i>" % style
        parts.append("<span>%s%s</span>" % (swatch, _esc(label)))
    return "<p class=\"legend\">" + "".join(parts) + "</p>"


def _series_table(days, series):
    header = "".join("<th class=\"num\">%s</th>" % _esc(item["label"])
                     for item in series)
    rows = []
    for position, key in enumerate(days):
        if not any(item["values"][position] for item in series):
            continue
        cells = "".join("<td class=\"num\">%s</td>" % format_int(
            item["values"][position]) for item in series)
        rows.append("<tr><td>%s</td>%s</tr>" % (_esc(key), cells))
    return ("<table><thead><tr><th>Day (%s)</th>%s</tr></thead><tbody>%s"
            "</tbody></table>" % (PRODUCT_ANALYSIS_TIMEZONE, header,
                                  "".join(rows)))


def render_series(chart, empty_text, what):
    """Chart, or a table when fewer than seven days carry data, or a note."""
    if chart is None:
        return ""
    days = chart["days"]
    series = chart["series"]
    populated = sum(
        1 for position in range(len(days))
        if any(item["values"][position] for item in series))
    if populated == 0:
        return "<p class=\"empty\">%s</p>" % _esc(empty_text)
    if populated < PRODUCT_ANALYSIS_MIN_CHART_POINTS:
        return (
            "<p class=\"note\">Only %d of the last %d days have %s, so the "
            "days are listed instead of drawn.</p>" % (
                populated, len(days), _esc(what))
            + _series_table(days, series))
    return ("<div class=\"chart\">" + line_chart_svg(days, series) + "</div>"
            + _legend([(item.get("style", "accent"), item["label"])
                       for item in series]))


def stacked_bar_svg(rows, width=760):
    """Horizontal stacked bars: mirror-creators in the accent, the rest hatched."""
    row_height = 34
    left = 150
    right = 130
    height = row_height * len(rows) + 8
    plot_width = width - left - right
    peak = max([row["count"] for row in rows] + [1])
    parts = [
        "<svg viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\" role=\"img\" "
        "aria-label=\"Accounts by signup origin, with mirror creators\">"
        "<defs><pattern id=\"pa-hatch\" width=\"6\" height=\"6\" "
        "patternUnits=\"userSpaceOnUse\" patternTransform=\"rotate(45)\">"
        "<line x1=\"0\" y1=\"0\" x2=\"0\" y2=\"6\" class=\"s-second\" "
        "stroke-width=\"2\"/></pattern></defs>" % (
            width, height, width, height)]
    for position, row in enumerate(rows):
        y = 4 + position * row_height
        bar_y = y + 8
        activated_width = plot_width * row["activated"] / float(peak)
        rest_width = plot_width * (row["count"] - row["activated"]) / float(peak)
        parts.append(
            "<text class=\"bar-label\" x=\"%d\" y=\"%d\" text-anchor=\"end\">%s"
            "</text>" % (left - 10, bar_y + 14, _esc(row["label"])))
        if activated_width > 0:
            parts.append(
                "<rect class=\"f-accent\" x=\"%d\" y=\"%d\" width=\"%.1f\" "
                "height=\"18\"><title>%s</title></rect>" % (
                    left, bar_y, activated_width,
                    _esc("%s: %s with at least one mirror" % (
                        row["label"], format_int(row["activated"])))))
        if rest_width > 0:
            parts.append(
                "<rect fill=\"url(#pa-hatch)\" x=\"%.1f\" y=\"%d\" "
                "width=\"%.1f\" height=\"18\"><title>%s</title></rect>"
                "<rect fill=\"none\" class=\"s-second\" x=\"%.1f\" y=\"%d\" "
                "width=\"%.1f\" height=\"18\"/>" % (
                    left + activated_width, bar_y, rest_width,
                    _esc("%s: %s without a mirror" % (
                        row["label"],
                        format_int(row["count"] - row["activated"]))),
                    left + activated_width, bar_y, rest_width))
        parts.append(
            "<text class=\"tick\" x=\"%.1f\" y=\"%d\">%s of %s</text>" % (
                left + activated_width + rest_width + 8, bar_y + 14,
                format_int(row["activated"]), format_int(row["count"])))
    parts.append("</svg>")
    return "".join(parts)


def _sort_link(params, key, label, current_sort, current_dir, default_dir):
    direction = default_dir
    marker = ""
    css = ""
    if current_sort == key:
        css = " class=\"on\""
        direction = "asc" if current_dir == "desc" else "desc"
        marker = " ↓" if current_dir == "desc" else " ↑"
    return "<a%s href=\"%s\">%s%s</a>" % (
        css, _query(params, sort=key, dir=direction, page=None),
        _esc(label), marker)


def _paginate(rows, params):
    page_size = PRODUCT_ANALYSIS_TABLE_PAGE
    total_pages = max(1, (len(rows) + page_size - 1) // page_size)
    page = min(total_pages, max(1, _int((params.get("page") or ["1"])[0], 1)))
    start = (page - 1) * page_size
    return rows[start:start + page_size], page, total_pages


def _pager_html(params, page, total_pages, total_rows, noun):
    links = []
    if page > 1:
        links.append("<a class=\"btn btn-sm\" href=\"%s\">Previous page</a>"
                     % _query(params, page=page - 1))
    if page < total_pages:
        links.append("<a class=\"btn btn-sm\" href=\"%s\">Next page</a>"
                     % _query(params, page=page + 1))
    return ("<div class=\"pager\"><span>%s %s, page %d of %d</span>"
            "<span class=\"actions\">%s</span></div>" % (
                format_int(total_rows), _esc(noun), page, total_pages,
                "".join(links)))


def render_overview(report, params):
    body = [_page_head(
        "Overview",
        "All times are %s. Windows end now and compare with the equal window "
        "before them." % PRODUCT_ANALYSIS_TIMEZONE)]
    body.append(_errors_html(report))
    body.append(_notes_html(report))
    body.append(render_figures(overview_figures(report)))
    chart = signup_series(report)
    content = ("" if chart is not None
               else _errors_html(report, keys={"accounts"}))
    content += render_series(
        chart, "No accounts were created in the last %d days; signups "
        "would appear here per day." % SIGNUP_CHART_DAYS, "signups")
    body.append(_card(
        "Signups per day, last %d days" % SIGNUP_CHART_DAYS, content,
        "Organic against every other origin. Job flow is not separated yet: "
        "until a rule exists those accounts sit in the other series as "
        "unknown."))
    chart = mirror_series(report)
    content = ("" if chart is not None
               else _errors_html(report, keys={"repositories"}))
    content += render_series(
        chart, "No mirrors with a creation date in the last %d days; mirror "
        "creation would appear here per day." % SIGNUP_CHART_DAYS,
        "mirror creations")
    body.append(_card(
        "Mirrors created per day, last %d days" % SIGNUP_CHART_DAYS, content))
    return "".join(body)


def render_sources(report, params):
    body = [_page_head(
        "Signup sources",
        "Where accounts came from, and which origins produced accounts that "
        "created a mirror. All time.")]
    body.append(_errors_html(report, keys={"accounts", "repositories",
                                           "referral_attributions",
                                           "signup_origin_backfill"}))
    breakdown = source_breakdown(report)
    if breakdown is None:
        return "".join(body)
    backfill = report.get("backfill") or {}
    if backfill.get("ran"):
        body.append(
            "<p class=\"note\">The one-time origin backfill ran on this "
            "load and updated %d accounts.</p>" % backfill.get("updated", 0))
    rows = ["<table><thead><tr><th>Origin</th><th class=\"num\">Accounts</th>"
            "<th class=\"num\">Share</th>"
            "<th class=\"num\">With at least one mirror</th>"
            "<th class=\"num\">Of that origin</th></tr></thead><tbody>"]
    for row in breakdown["rows"]:
        rows.append(
            "<tr><td>%s</td><td class=\"num\">%s</td><td class=\"num\">%.1f%%"
            "</td><td class=\"num\">%s</td><td class=\"num\">%.1f%%</td></tr>"
            % (_esc(row["label"]), format_int(row["count"]), row["share"],
               format_int(row["activated"]), row["activated_share"]))
    total = breakdown["total"]
    rows.append(
        "<tr class=\"total\"><td>All accounts</td><td class=\"num\">%s</td>"
        "<td class=\"num\">100%%</td><td class=\"num\">%s</td>"
        "<td class=\"num\">%.1f%%</td></tr></tbody></table>" % (
            format_int(total), format_int(breakdown["activated_total"]),
            (100.0 * breakdown["activated_total"] / total) if total else 0.0))
    body.append(_card("", "".join(rows), flush=True))
    if total == 0:
        chart = ("<p class=\"empty\">No accounts yet; each origin would "
                 "appear here as a bar.</p>")
    else:
        chart = ("<div class=\"chart\">" + stacked_bar_svg(breakdown["rows"])
                 + "</div>" + _legend([
                     ("accent", "Created at least one mirror"),
                     ("hatch", "No mirror yet")]))
    body.append(_card("Accounts by origin, mirror creators marked", chart))
    body.append(
        "<p class=\"note\">Origins come from users.signup_origin, derived once "
        "by the backfill and stamped at signup since. Referral: arrived with a "
        "referral link, whatever the credit decision. Organic: the signup "
        "recorded no referrer (marker introduced 2026-08-12). Job flow: no "
        "rule yet, so nothing is classified there. Unknown: everything else. "
        "Rules are recorded in www/docs/admin-dashboard.md.</p>")
    return "".join(body)


def render_users(report, params):
    body = [_page_head("Users", "Every user account, all time. Read-only.")]
    body.append(_errors_html(report, keys={"accounts", "repositories", "nodes",
                                           "account_sessions",
                                           "account_presence"}))
    accounts = report.get("accounts")
    if accounts is None:
        return "".join(body)
    query = (params.get("q") or [""])[0].strip().lower()[:80]
    sort = (params.get("sort") or ["created"])[0]
    direction = (params.get("dir") or ["desc"])[0]
    direction = "asc" if direction == "asc" else "desc"
    keys = {
        "name": lambda account: account["name"],
        "created": lambda account: account["created_at"],
        "origin": lambda account: SIGNUP_ORIGIN_LABELS[account["origin"]],
        "mirrors": lambda account: account["mirrors"],
        "activity": lambda account: account["last_activity"],
    }
    if sort not in keys:
        sort = "created"
    rows = [account for account in accounts
            if not query or query in account["name"]]
    rows.sort(key=lambda account: (keys[sort](account), account["name"]),
              reverse=(direction == "desc"))
    page_rows, page, total_pages = _paginate(rows, params)
    search = (
        "<form class=\"toolbar\" method=\"get\" action=\"%s\" "
        "style=\"padding:0 0 16px\">"
        "<input type=\"hidden\" name=\"view\" value=\"users\">"
        "<label for=\"pa-q\" class=\"hint\">Identifier contains</label>"
        "<input id=\"pa-q\" type=\"text\" name=\"q\" value=\"%s\" "
        "maxlength=\"80\" style=\"width:260px;max-width:100%%\">"
        "<button type=\"submit\" class=\"btn\">Search</button>"
        "%s</form>" % (
            _esc(_route_path()), _esc(query),
            ("<a class=\"btn\" href=\"?view=users\">Clear</a>" if query else "")))
    body.append(search)
    if not rows:
        body.append(_card("", "<p class=\"empty\">%s</p>" % (
            "No account identifier contains that text." if query
            else "No user accounts yet; each would appear here as a row.")))
        return "".join(body)
    pager = _pager_html(params, page, total_pages, len(rows), "accounts")
    header = "".join([
        "<th>%s</th>" % _sort_link(params, "name", "Identifier", sort,
                                   direction, "asc"),
        "<th>%s</th>" % _sort_link(params, "created", "Signup date", sort,
                                   direction, "desc"),
        "<th>%s</th>" % _sort_link(params, "origin", "Signup origin", sort,
                                   direction, "asc"),
        "<th class=\"num\">%s</th>" % _sort_link(
            params, "mirrors", "Mirrors", sort, direction, "desc"),
        "<th>%s</th>" % _sort_link(params, "activity", "Last activity", sort,
                                   direction, "desc"),
    ])
    table = ["<table><thead><tr>" + header + "</tr></thead><tbody>"]
    for account in page_rows:
        table.append(
            "<tr><td>%s</td><td>%s</td><td>%s</td><td class=\"num\">%s</td>"
            "<td>%s</td></tr>" % (
                _esc(account["name"] or "(no identifier)"),
                _esc(format_timestamp(account["created_at"]) or "unknown"),
                _esc(SIGNUP_ORIGIN_LABELS[account["origin"]]),
                format_int(account["mirrors"]),
                _esc(format_timestamp(account["last_activity"])
                     or "no activity recorded")))
    table.append("</tbody></table>")
    body.append(_card("", "".join(table), flush=True, footer=pager))
    body.append(
        "<p class=\"note\">Last activity is the newest of a web session, a "
        "chat presence ping and any owned node's last check-in. Mirrors count "
        "catalog records owned by the account or one of its nodes.</p>")
    return "".join(body)


def render_repositories(report, params):
    body = [_page_head(
        "Repositories",
        "Every catalog record, all time, with clones from the last %d days."
        % WINDOW_DAYS)]
    body.append(_errors_html(report, keys={"repositories", "accounts",
                                           "mirror_serve_counters",
                                           "product_clone_daily"}))
    repositories = report.get("repositories")
    if repositories is None:
        return "".join(body)
    sort = (params.get("sort") or ["created"])[0]
    direction = (params.get("dir") or ["desc"])[0]
    direction = "asc" if direction == "asc" else "desc"
    keys = {
        "name": lambda repo: repo["owner"] + "/" + repo["name"],
        "owner": lambda repo: repo["owner_name"],
        "visibility": lambda repo: repo["visibility"],
        "size": lambda repo: repo["size"],
        "mirrors": lambda repo: repo["mirror_count"],
        "clones": lambda repo: repo["clones_30d"],
        "created": lambda repo: repo["created_at"],
    }
    if sort not in keys:
        sort = "created"
    rows = list(repositories)
    rows.sort(key=lambda repo: (keys[sort](repo), repo["owner"], repo["name"]),
              reverse=(direction == "desc"))
    page_rows, page, total_pages = _paginate(rows, params)
    if not rows:
        body.append(_card("", "<p class=\"empty\">No repositories in the "
                          "catalog yet; each would appear here as a row.</p>"))
    else:
        pager = _pager_html(params, page, total_pages, len(rows),
                            "repositories")
        header = "".join([
            "<th>%s</th>" % _sort_link(params, "name", "Repository", sort,
                                       direction, "asc"),
            "<th>%s</th>" % _sort_link(params, "owner", "Owner", sort,
                                       direction, "asc"),
            "<th>%s</th>" % _sort_link(params, "visibility", "Visibility",
                                       sort, direction, "asc"),
            "<th class=\"num\">%s</th>" % _sort_link(
                params, "size", "Size", sort, direction, "desc"),
            "<th class=\"num\">%s</th>" % _sort_link(
                params, "mirrors", "Mirrors", sort, direction, "desc"),
            "<th class=\"num\">%s</th>" % _sort_link(
                params, "clones", "Clones, last %d days" % WINDOW_DAYS, sort,
                direction, "desc"),
            "<th>%s</th>" % _sort_link(params, "created", "Created", sort,
                                       direction, "desc"),
        ])
        table = ["<table><thead><tr>" + header + "</tr></thead><tbody>"]
        clone_data = report.get("clone_daily") is not None
        for repo in page_rows:
            table.append(
                "<tr><td>%s</td><td>%s</td><td>%s</td><td class=\"num\">%s"
                "</td><td class=\"num\">%s</td><td class=\"num\">%s</td>"
                "<td>%s</td></tr>" % (
                    _esc(repo["owner"] + "/" + repo["name"]),
                    _esc(repo["owner_name"]), _esc(repo["visibility"]),
                    _esc(format_bytes(repo["size"])),
                    format_int(repo["mirror_count"]),
                    format_int(repo["clones_30d"]) if clone_data else "n/a",
                    _esc(format_timestamp(repo["created_at"]) or "unknown")))
        table.append("</tbody></table>")
        body.append(_card("", "".join(table), flush=True, footer=pager))
        body.append(
            "<p class=\"note\">Mirrors counts every catalog record sharing "
            "this repository's root commit (or name, when no root is "
            "published), the same grouping the network page uses. Size is "
            "what the publishing node reported.</p>")
    chart = clone_series(report)
    if chart is None:
        content = _errors_html(report, keys={"product_clone_daily"})
    elif not report.get("clone_first_day"):
        content = ("<p class=\"empty\">No clone data yet. Per-day clone "
                   "counting starts with the deploy that added this "
                   "dashboard; clones served through the public HTTPS "
                   "mirror route would appear here per day.</p>")
    else:
        content = render_series(
            chart, "No clones were served through the public HTTPS mirror "
            "route in the last %d days." % WINDOW_DAYS, "clones")
        first_day = report.get("clone_first_day")
        if first_day and first_day > chart["days"][0]:
            content += ("<p class=\"note\">Counting started on %s; earlier "
                        "days in this window have no data.</p>"
                        % _esc(first_day))
    body.append(_card(
        "Clones per day, last %d days, all repositories" % WINDOW_DAYS,
        content))
    return "".join(body)


def render_acquisition(report, params):
    return _page_head("Acquisition", "Pending.") + _card(
        "",
        "<p class=\"empty\">UTM attribution is not part of this report, so "
        "there is nothing to count here. Sessions, clone-URL copies and "
        "signups by utm_source and utm_campaign, with bots excluded and the "
        "excluded count stated, live in the Acquisition section for platform "
        "administrators. Clone-URL copies are an intent signal, not a clone "
        "count.</p>")


_RENDERERS = {
    "overview": render_overview,
    "sources": render_sources,
    "users": render_users,
    "repositories": render_repositories,
    "acquisition": render_acquisition,
}


def render_dashboard_html(view, report, params, theme="system", admin="",
                          console="", badges=None, founder_session=True,
                          subpages="/admin"):
    shell = _admin_shell
    renderer = _RENDERERS.get(view, render_overview)
    title = dict(VIEWS).get(view, "Overview")
    tab_items = [(key, label, _query(params, view=key, sort=None, dir=None,
                                     page=None, q=None), None)
                 for key, label in VIEWS]
    content = (
        shell.page_head(
            "Product analytics",
            "Accounts, signup origins, mirrors and clones, read in full from "
            "the database on every load.", crumb="Growth")
        + shell.tabs(tab_items, view)
        + renderer(report, params))
    foot = ""
    if founder_session:
        foot = (
            "<form method=\"post\" action=\"" + _esc(_route_path()) + "\">"
            "<input type=\"hidden\" name=\"action\" value=\"logout\">"
            "<button type=\"submit\" class=\"btn btn-sm btn-block\">"
            + shell.icon("logout") + "Sign out</button></form>")
    return shell.render_page(
        "%s · Product analytics" % title, content, active="analytics",
        console=console, theme=theme, badges=badges, account=admin,
        subpages=subpages,
        next_path=_route_path() + _raw_query(params or {}),
        foot_extra=foot, allowed=None if admin else {"analytics"},
        head_extra="<style>" + _STYLE + "</style>")
