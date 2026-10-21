"""The admin console's support inboxes: mobile bug reports and site feedback.

Reports sent from the mobile app's report button (``bug_reports``, written
by bug_reports.py) and the docs thumbs and World lobby notes (``feedback``)
were only reachable as raw rows in the generic database browser, with the
report text an encrypted blob and no way to mark a report handled. These two
sections give each its own page inside the shared admin shell.

Access is exactly the console's: the secret console path, an account
session whose account is ``is_admin`` (checked live by the caller before it
dispatches here), and the console CSRF token on every write. The only write
is a bug report's triage status, and every change is audited.

Reads stay bounded. Lists page 50 rows at a time over indexed columns and
decrypt only the rows on the page, plus the one report that is open.
"""

import base64
import re
from urllib.parse import quote

import agent_notes
from hidden_unicode import clean_query as clean_hidden_query


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


SUPPORT_VIEWS = ("bugs", "feedback", "agent-notes")
BUG_STATUSES = ("new", "triaged", "closed")
BUG_SEVERITIES = ("crash", "high", "normal", "low")
FEEDBACK_SOURCE_FILTERS = ("docs", "world", "web")
PAGE_SIZE = 50
MAX_FORM_BYTES = 4096
DAY_MS = 24 * 60 * 60 * 1000
NOTICES = {
    "status_saved": "Report status saved.",
    "status_unchanged": "That report already had this status.",
    "report_missing": "That report no longer exists.",
    "note_approved": "Note approved and published as a public note.",
    "note_rejected": "Note rejected. It stays hidden.",
    "note_reviewed": "That note was already reviewed.",
    "note_missing": "That note no longer exists.",
    "note_invalid": "Choose Approve or Reject.",
    "invalid_status": "Choose New, Triaged or Closed.",
    "csrf": ("Action blocked: invalid or missing CSRF token. Reload the page "
             "and try again."),
}


def _first(params, key):
    values = (params or {}).get(key) or [""]
    return str(values[0] or "")


def _page(params):
    raw = _first(params, "page")
    return max(1, int(raw)) if raw.isdigit() and len(raw) <= 6 else 1


def _clean_report_id(value):
    value = str(value or "").strip().lower()
    return value if len(value) == 32 and all(
        ch in "0123456789abcdef" for ch in value) else ""


def _console_path(env):
    path = str(getattr(env, "ADMIN_PATH", "") or "").strip("/")
    return "/" + path if path else ""


def _href(admin_query, **params):
    return _admin_href(admin_query, **params)


async def _rows(env, sql, *args):
    try:
        return [dict(row) for row in (await d1_all(env, sql, *args) or [])]
    except Exception:
        return None


async def _count(env, sql, *args):
    rows = await _rows(env, sql, *args)
    if not rows:
        return 0
    return int(rows[0].get("n") or 0)


def _html(body, status=200, extra=None):
    headers = {
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    }
    headers.update(extra or {})
    return Response(body, status=status, headers=headers)


def _redirect(location):
    return Response("", status=303, headers={
        "location": location, "cache-control": "no-store"})


async def support_handler(env, request, params, csrf_field="",
                          admin_query="", account=""):
    """``?view=bugs`` and ``?view=feedback`` on the console path."""
    view = _first(params, "view")
    if view not in SUPPORT_VIEWS:
        return None
    if method_name(request) == "POST":
        return await _handle_post(env, request, params, admin_query, account)
    if view == "bugs" and _first(params, "attachment"):
        return await _bug_attachment_response(env, params)
    shell = _admin_shell
    notice = _first(params, "notice")
    banner = ""
    if notice in NOTICES:
        tone = "ok" if notice in (
            "status_saved", "status_unchanged", "note_approved",
            "note_rejected", "note_reviewed") else "bad"
        banner = shell.banner(NOTICES[notice], tone)
    if view == "bugs":
        title, content = "Bug reports", await render_bug_reports(
            env, params, csrf_field, admin_query)
    elif view == "agent-notes":
        title, content = "Agent notes", await render_agent_notes(
            env, params, csrf_field, admin_query)
    else:
        title, content = "Docs & World feedback", await render_feedback(
            env, params, admin_query)
    url = urlparse(request.url)
    page = shell.render_page(
        title, banner + content, active=view,
        console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        subpages=shell.subpage_base(env),
        next_path=url.path + (("?" + url.query) if url.query else ""))
    return _html(page)


async def _handle_post(env, request, params, admin_query, account):
    """Save one bug report's triage status, then return to the report."""
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = clean_hidden_query(parse_qs(str(raw or "")[:MAX_FORM_BYTES], keep_blank_values=True))
    if _first(params, "view") == "agent-notes":
        return await _review_agent_note(env, form, params, admin_query,
                                        account)
    report_id = _clean_report_id(_first(form, "report"))
    base = _console_path(env)
    back = dict(view="bugs", report=report_id or None,
                status=_first(params, "status") or None)

    def location(notice):
        return base + _href(admin_query, **back, notice=notice).replace(
            "&amp;", "&")

    if not _admin_csrf_ok(env, form):
        return _redirect(location("csrf"))
    if _first(form, "action") != "set_bug_status":
        return _redirect(location("invalid_status"))
    status = _first(form, "bug_status")
    if status not in BUG_STATUSES:
        return _redirect(location("invalid_status"))
    row = None
    if report_id:
        rows = await _rows(
            env, "SELECT status FROM bug_reports WHERE report_id=? LIMIT 1",
            report_id)
        row = rows[0] if rows else None
    if not row:
        await _audit_sensitive_action(
            env, account, "admin.console_set_bug_status", "bug_report",
            report_id, "failed", {"reason": "report_missing"})
        return _redirect(location("report_missing"))
    previous = str(row.get("status") or "")
    if previous == status:
        return _redirect(location("status_unchanged"))
    await d1_run(
        env, "UPDATE bug_reports SET status=? WHERE report_id=?",
        status, report_id)
    await _audit_sensitive_action(
        env, account, "admin.console_set_bug_status", "bug_report",
        report_id, "success", {"from": previous, "to": status})
    return _redirect(location("status_saved"))


# --- bug reports ------------------------------------------------------------------

def _severity_html(severity):
    severity = severity if severity in BUG_SEVERITIES else "normal"
    return '<span class="sev %s">%s</span>' % (severity, severity)


def _status_pill(status):
    tone = {"new": "info", "triaged": "warn"}.get(status, "")
    return _admin_shell.pill(status or "new", tone)


async def _decrypt(env, value):
    if not value:
        return {}
    try:
        record = await decrypt_row(env, value)
    except Exception:
        return {}
    return record if isinstance(record, dict) else {}


async def _bug_attachment_response(env, params):
    report_id = _clean_report_id(_first(params, "report"))
    attachment_id = _clean_report_id(_first(params, "attachment"))
    if not report_id or not attachment_id:
        return _html("Attachment not found.", 404)
    rows = await _rows(
        env, "SELECT chunk_index, name, content_type, size, data FROM "
        "bug_report_attachments WHERE report_id=? AND attachment_id=? "
        "ORDER BY chunk_index", report_id, attachment_id)
    if not rows:
        return _html("Attachment not found.", 404)
    pieces = []
    for index, row in enumerate(rows):
        if int(row.get("chunk_index") if row.get("chunk_index") is not None else -1) != index:
            return _html("Attachment is incomplete.", 410)
        record = await _decrypt(env, row.get("data"))
        try:
            pieces.append(base64.b64decode(str(record.get("data") or ""),
                                           validate=True))
        except Exception:
            return _html("Attachment is unavailable.", 410)
    data = b"".join(pieces)
    if len(data) != int(rows[0].get("size") or 0):
        return _html("Attachment is incomplete.", 410)
    content_type = str(rows[0].get("content_type") or "")
    if not re.fullmatch(r"[a-z0-9][a-z0-9.+-]{0,63}/[a-z0-9][a-z0-9.+-]{0,63}", content_type):
        content_type = "application/octet-stream"
    name = re.sub(r"[\r\n\"\\\\]", "_", str(rows[0].get("name") or "attachment"))[:160]
    return Response(data, headers={
        "content-type": content_type,
        "content-disposition": 'attachment; filename="%s"' % name,
        "cache-control": "no-store",
        "x-content-type-options": "nosniff",
    })


async def render_bug_reports(env, params, csrf_field="", admin_query="",
                             now=None):
    shell = _admin_shell
    now = int(Date.now()) if now is None else int(now)
    status = _first(params, "status")
    status = status if status in BUG_STATUSES else "new"
    severity = _first(params, "severity")
    severity = severity if severity in BUG_SEVERITIES else ""
    platform = _first(params, "platform").strip().lower()[:40]
    page = _page(params)

    counts = await _rows(
        env, "SELECT status, COUNT(*) AS n FROM bug_reports GROUP BY status")
    if counts is None:
        return (shell.page_head("Bug reports", crumb="Support")
                + shell.card(shell.empty(
                    "The bug_reports table is not available on this database "
                    "yet. Apply migration 0173 and reload.")))
    by_status = {str(r.get("status")): int(r.get("n") or 0) for r in counts}
    total_reports = sum(by_status.values())
    crashes = await _count(
        env, "SELECT COUNT(*) AS n FROM bug_reports WHERE severity='crash' "
        "AND created_at>=?", now - 7 * DAY_MS)
    oldest = await _rows(
        env, "SELECT created_at FROM bug_reports WHERE status='new' "
        "ORDER BY created_at ASC LIMIT 1")
    signed_in = await _count(
        env, "SELECT COUNT(*) AS n FROM bug_reports WHERE account<>''")
    platforms = [str(r.get("platform") or "") for r in (await _rows(
        env, "SELECT DISTINCT platform FROM bug_reports WHERE platform<>'' "
        "ORDER BY platform LIMIT 20") or [])]

    where = ["status=?"]
    args = [status]
    if severity:
        where.append("severity=?")
        args.append(severity)
    if platform:
        where.append("platform=?")
        args.append(platform)
    clause = " WHERE " + " AND ".join(where)
    matching = await _count(
        env, "SELECT COUNT(*) AS n FROM bug_reports" + clause, *args)
    pages = max(1, -(-matching // PAGE_SIZE))
    page = min(page, pages)
    listed = await _rows(
        env,
        "SELECT report_id, account, source, platform, app_version, severity, "
        "status, data, created_at FROM bug_reports" + clause
        + " ORDER BY created_at DESC, report_id DESC LIMIT ? OFFSET ?",
        *args, PAGE_SIZE, (page - 1) * PAGE_SIZE) or []

    selected_id = _clean_report_id(_first(params, "report"))
    selected = None
    for row in listed:
        if row.get("report_id") == selected_id:
            selected = row
    if selected_id and selected is None:
        rows = await _rows(
            env,
            "SELECT report_id, account, source, platform, app_version, "
            "severity, status, data, created_at FROM bug_reports "
            "WHERE report_id=? LIMIT 1", selected_id)
        selected = rows[0] if rows else None
    if selected is None and listed:
        selected = listed[0]

    filters = dict(view="bugs", status=status, severity=severity or None,
                   platform=platform or None)

    oldest_ms = int((oldest[0] if oldest else {}).get("created_at") or 0)
    kpis = (
        '<div class="grid cols-4">'
        + shell.kpi("New", shell.format_int(by_status.get("new", 0)),
                    "Waiting for triage")
        + shell.kpi("Crashes, last 7 days", shell.format_int(crashes),
                    "Severity crash", icon_name="alert",
                    tone="bad" if crashes else "")
        + shell.kpi("Oldest untriaged",
                    shell.relative_label(oldest_ms, now).replace(" ago", "")
                    if oldest_ms else "—",
                    ("Received " + shell.utc_label(oldest_ms, False))
                    if oldest_ms else "Nothing is waiting")
        + shell.kpi("Signed in when reporting",
                    ("%d%%" % round(100 * signed_in / total_reports))
                    if total_reports else "—",
                    "The rest are anonymous" if total_reports else
                    "No reports yet")
        + '</div>')

    severity_options = "".join(
        '<option value="%s"%s>%s</option>' % (
            value, " selected" if value == severity else "", label)
        for value, label in (("", "All severities"),) + tuple(
            (item, item) for item in BUG_SEVERITIES))
    platform_options = "".join(
        '<option value="%s"%s>%s</option>' % (
            _html_escape(value), " selected" if value == platform else "",
            _html_escape(label))
        for value, label in [("", "All platforms")] + [
            (item, item) for item in platforms])
    admin_value = admin_query.split("=", 1)[1] if admin_query else ""
    filter_form = (
        '<form class="actions" method="get" action="">'
        '%s<input type="hidden" name="view" value="bugs">'
        '<input type="hidden" name="status" value="%s">'
        '<label class="sr" for="bug-severity">Severity</label>'
        '<select id="bug-severity" name="severity">%s</select>'
        '<label class="sr" for="bug-platform">Platform</label>'
        '<select id="bug-platform" name="platform">%s</select>'
        '<button class="btn" type="submit">Filter</button></form>' % (
            ('<input type="hidden" name="admin" value="%s">'
             % _html_escape(admin_value)) if admin_value else "",
            status, severity_options, platform_options))

    tabs = shell.tabs([
        (key, key.capitalize(),
         _href(admin_query, view="bugs", status=key,
               severity=severity or None, platform=platform or None),
         shell.format_int(by_status.get(key, 0)))
        for key in BUG_STATUSES], status)

    items = []
    for row in listed:
        record = await _decrypt(env, row.get("data"))
        row_severity = str(row.get("severity") or "normal")
        report_id = str(row.get("report_id") or "")
        current = selected is not None and report_id == selected.get(
            "report_id")
        items.append(
            '<a class="report%s" href="%s"%s><span class="stripe %s"></span>'
            '<div><div class="msg">%s</div><div class="meta">%s'
            '<span>%s</span><span class="mono">%s</span>'
            '<span style="margin-left:auto">%s</span></div></div></a>' % (
                " on" if current else "",
                _href(admin_query, report=report_id, **filters),
                ' aria-current="true"' if current else "",
                _html_escape(row_severity),
                _html_escape(record.get("message") or "(no message)"),
                _severity_html(row_severity),
                _html_escape(record.get("screen") or "—"),
                _html_escape(" ".join(filter(None, (
                    str(row.get("platform") or ""),
                    str(row.get("app_version") or "")))) or "—"),
                shell.time_html(row.get("created_at"), now)))
    if not items:
        items.append(shell.empty("No %s reports%s." % (
            status, " match these filters" if severity or platform else "")))

    pager = ""
    if pages > 1:
        pager = (
            '<div class="pager"><span>Page %d of %d</span><div class="actions">'
            '%s%s</div></div>' % (
                page, pages,
                ('<a class="btn btn-sm" href="%s">Previous</a>' % _href(
                    admin_query, page=page - 1, **filters)) if page > 1 else "",
                ('<a class="btn btn-sm" href="%s">Next</a>' % _href(
                    admin_query, page=page + 1, **filters))
                if page < pages else ""))

    detail = await _bug_detail(env, selected, csrf_field, admin_query, filters,
                               now)
    return (
        shell.page_head(
            "Bug reports",
            "Reports people send from the ForkMesh mobile app. Triage each "
            "one, then close it when it is fixed or not actionable.",
            crumb="Support", actions=filter_form)
        + kpis
        + '<section class="card">' + tabs
        + '<div class="inbox"><div class="inbox-list">' + "".join(items)
        + pager + '</div><div class="detail">' + detail
        + '</div></div></section>'
    )


async def _bug_detail(env, row, csrf_field, admin_query, filters, now):
    shell = _admin_shell
    if not row:
        return shell.empty("Select a report to read it.")
    record = await _decrypt(env, row.get("data"))
    report_id = str(row.get("report_id") or "")
    status = str(row.get("status") or "new")
    account = str(row.get("account") or record.get("account") or "")
    contact = str(record.get("contact") or "")
    attachments = record.get("attachments") or []
    attachment_text = ", ".join(
        '<a href="%s">%s (%s bytes)</a>' % (
            _href(admin_query, view="bugs", report=report_id,
                  attachment=item.get("id") or ""),
            _html_escape(item.get("name") or "attachment"),
            _html_escape(str(item.get("size") or 0)))
        for item in attachments if isinstance(item, dict))
    buttons = "".join(
        '<button type="submit" name="bug_status" value="%s" '
        'aria-pressed="%s">%s</button>' % (
            value, "true" if value == status else "false",
            value.capitalize())
        for value in BUG_STATUSES)
    facts = (
        ("Screen", _html_escape(record.get("screen") or "—")),
        ("Platform", _html_escape(" · ".join(filter(None, (
            str(record.get("osVersion") or ""),
            str(row.get("platform") or "")))) or "—")),
        ("App version", '<span class="mono">%s</span>' % _html_escape(
            row.get("app_version") or "—")),
        ("Device", _html_escape(record.get("device") or "—")),
        ("Account", ('<a href="%s">%s</a>' % (
            _href(admin_query, view="users", user=account),
            _html_escape(account))) if account else
         '<span class="muted">anonymous</span>'),
        ("Contact", _html_escape(contact) if contact else
         '<span class="muted">none left</span>'),
        ("Source", _html_escape(row.get("source") or "—")),
        ("Attachments", attachment_text or '<span class="muted">none</span>'),
        ("Received", "%s · %s" % (
            shell.time_html(row.get("created_at"), now),
            _html_escape(shell.utc_label(row.get("created_at"))))),
    )
    return (
        '<div class="form-row" style="justify-content:space-between">'
        '<div class="form-row">%s%s<span class="mono muted">#%s</span></div>'
        '</div>'
        '<div class="quote">%s</div>'
        '<dl class="kv">%s</dl>'
        '<form class="form-row" method="post" action="%s" '
        'style="justify-content:space-between;margin-top:18px">%s'
        '<input type="hidden" name="action" value="set_bug_status">'
        '<input type="hidden" name="report" value="%s">'
        '<span class="hint">Set the status</span>'
        '<div class="seg" role="group" aria-label="Status">%s</div></form>'
    ) % (
        _severity_html(str(row.get("severity") or "normal")),
        _status_pill(status),
        _html_escape(report_id),
        _html_escape(record.get("message") or "(no message)"),
        "".join("<dt>%s</dt><dd>%s</dd>" % (label, value)
                for label, value in facts),
        _href(admin_query, view="bugs", report=report_id,
              status=filters.get("status")),
        csrf_field,
        _html_escape(report_id),
        buttons,
    )


# --- docs and World feedback --------------------------------------------------------

def _vote_pill(vote):
    shell = _admin_shell
    if vote == "like":
        return shell.pill("helpful", "ok", "thumbsup")
    if vote == "dislike":
        return shell.pill("not helpful", "bad", "thumbsdown")
    return shell.pill("note", "line", "message")


async def render_feedback(env, params, admin_query="", now=None):
    shell = _admin_shell
    now = int(Date.now()) if now is None else int(now)
    source = _first(params, "source")
    source = source if source in FEEDBACK_SOURCE_FILTERS else ""
    page = _page(params)

    totals = await _rows(
        env, "SELECT source, vote, COUNT(*) AS n FROM feedback "
        "GROUP BY source, vote")
    if totals is None:
        return (shell.page_head("Docs & World feedback", crumb="Support")
                + shell.card(shell.empty(
                    "The feedback table is not available on this database.")))
    tally = {}
    for row in totals:
        key = (str(row.get("source") or ""), str(row.get("vote") or ""))
        tally[key] = int(row.get("n") or 0)
    likes = tally.get(("docs", "like"), 0)
    dislikes = tally.get(("docs", "dislike"), 0)
    notes = sum(n for (src, vote), n in tally.items() if vote == "feedback")
    written = await _count(
        env, "SELECT COUNT(*) AS n FROM feedback WHERE message<>''")
    world = tally.get(("world", "feedback"), 0)
    web = tally.get(("web", "feedback"), 0)
    votes = likes + dislikes

    clause, args = "", []
    if source:
        clause, args = " WHERE source=?", [source]
    matching = sum(n for (src, _v), n in tally.items()
                   if not source or src == source)
    pages = max(1, -(-matching // PAGE_SIZE))
    page = min(page, pages)
    rows = await _rows(
        env, "SELECT id, ts, source, vote, path, message, user_agent "
        "FROM feedback" + clause + " ORDER BY ts DESC, id DESC "
        "LIMIT ? OFFSET ?", *args, PAGE_SIZE, (page - 1) * PAGE_SIZE) or []

    seg = '<div class="seg" role="group" aria-label="Source">%s</div>' % "".join(
        '<a href="%s"%s>%s</a>' % (
            _href(admin_query, view="feedback", source=key or None),
            ' aria-current="true"' if key == source else "", label)
        for key, label in (("", "All"), ("docs", "Docs"), ("world", "World"),
                           ("web", "Website")))

    kpis = (
        '<div class="grid cols-4">'
        + shell.kpi("Helpful", shell.format_int(likes), "Docs votes",
                    icon_name="thumbsup")
        + shell.kpi("Not helpful", shell.format_int(dislikes),
                    ("%d%% of docs votes" % round(100 * dislikes / votes))
                    if votes else "No docs votes yet",
                    icon_name="thumbsdown", tone="bad" if dislikes else "")
        + shell.kpi("Written feedback", shell.format_int(written),
                    "Messages from docs, World and website")
        + shell.kpi("Feedback notes", shell.format_int(world + web),
                    "Website and World; admins are pinged for each")
        + '</div>')
    body = []
    for row in rows:
        row_source = str(row.get("source") or "")
        message = str(row.get("message") or "")
        body.append(
            '<tr><td>%s</td><td>%s</td><td class="mono">%s</td>'
            '<td><span class="wrap-text">%s</span></td><td class="muted">%s'
            '</td><td class="r">%s</td></tr>' % (
                shell.pill(row_source or "—",
                           "done" if row_source in ("world", "web")
                           else "info"),
                _vote_pill(str(row.get("vote") or "")),
                _html_escape(row.get("path") or "—"),
                _html_escape(message) if message else
                '<span class="muted">—</span>',
                _html_escape(row.get("user_agent") or "—"),
                shell.time_html(row.get("ts"), now)))
    pager = (
        '<div class="pager"><span>%s</span><div class="actions">%s%s</div>'
        '</div>' % (
            ("Showing %d–%d of %d" % (
                (page - 1) * PAGE_SIZE + 1,
                min(matching, page * PAGE_SIZE), matching))
            if matching else "Nothing yet",
            ('<a class="btn btn-sm" href="%s">Previous</a>' % _href(
                admin_query, view="feedback", source=source or None,
                page=page - 1)) if page > 1 else "",
            ('<a class="btn btn-sm" href="%s">Next</a>' % _href(
                admin_query, view="feedback", source=source or None,
                page=page + 1)) if page < pages else ""))
    listing = shell.card(
        table=shell.table(
            "<th>Source</th><th>Vote</th><th>Page</th><th>Message</th>"
            "<th>Client</th><th class=\"r\">Received</th>",
            body, "No feedback recorded yet."),
        footer=pager)
    return (
        shell.page_head(
            "Docs & World feedback",
            "Thumbs up or down from the docs, and notes left at the World "
            "lobby kiosk. The newest 5,000 are kept.",
            crumb="Support", actions=seg)
        + kpis + listing
    )


# --- anonymous agent notes ------------------------------------------------------------

async def _review_agent_note(env, form, params, admin_query, account,
                             now=None):
    entry_id = _clean_report_id(_first(form, "entry"))
    base = _console_path(env)
    back = dict(view="agent-notes", entry=entry_id or None,
                status=_first(params, "status") or None)

    def location(notice):
        return base + _href(admin_query, **back, notice=notice).replace(
            "&amp;", "&")

    if not _admin_csrf_ok(env, form):
        return _redirect(location("csrf"))
    decision = _first(form, "decision")
    if (_first(form, "action") != "review_agent_note" or not account
            or decision not in ("approve", "reject")):
        return _redirect(location("note_invalid"))
    rows = await _rows(
        env, "SELECT entry_id, status, data FROM agent_note_queue "
        "WHERE entry_id=? LIMIT 1", entry_id) if entry_id else None
    row = rows[0] if rows else None
    if not row:
        await _audit_sensitive_action(
            env, account, "admin.console_review_agent_note", "agent_note",
            entry_id, "failed", {"reason": "note_missing"})
        return _redirect(location("note_missing"))
    if str(row.get("status") or "") != "pending":
        return _redirect(location("note_reviewed"))
    now = int(Date.now()) if now is None else int(now)
    note_id = ""
    if decision == "approve":
        record = await _decrypt(env, row.get("data"))
        owner_bi = await blind_index(env, str(account or "").strip().lower())
        note_id = _ap_uuid()
        sealed = await encrypt_row(env, {
            "title": agent_notes.approved_title(record),
            "markdown": agent_notes.approved_markdown(record)})
        claimed = await _rows(
            env, "UPDATE agent_note_queue SET status='approved', "
            "reviewed_at=?, note_id=? WHERE entry_id=? AND status='pending' "
            "RETURNING entry_id", now, note_id, entry_id)
        if not claimed:
            return _redirect(location("note_reviewed"))
        await d1_run(
            env, "INSERT INTO notes(note_id,owner_bi,visibility,version,"
            "created_at,updated_at,published_at,data) "
            "VALUES(?,?,'public',1,?,?,?,?)",
            note_id, owner_bi, now, now, now, sealed)
        await d1_run(
            env, "INSERT INTO note_versions(note_id,version,author_bi,"
            "created_at,data) VALUES(?,1,?,?,?)",
            note_id, owner_bi, now, sealed)
    else:
        claimed = await _rows(
            env, "UPDATE agent_note_queue SET status='rejected', "
            "reviewed_at=? WHERE entry_id=? AND status='pending' "
            "RETURNING entry_id", now, entry_id)
        if not claimed:
            return _redirect(location("note_reviewed"))
    await _audit_sensitive_action(
        env, account, "admin.console_review_agent_note", "agent_note",
        entry_id, "success", {"decision": decision, "note": note_id})
    return _redirect(location(
        "note_approved" if decision == "approve" else "note_rejected"))


def _agent_note_pill(status):
    tone = {"pending": "warn", "approved": "ok", "rejected": "bad"}.get(
        status, "")
    return _admin_shell.pill(status or "pending", tone)


async def render_agent_notes(env, params, csrf_field="", admin_query="",
                             now=None):
    shell = _admin_shell
    now = int(Date.now()) if now is None else int(now)
    status = _first(params, "status")
    status = status if status in agent_notes.STATUSES else "pending"
    page = _page(params)
    counts = await _rows(
        env, "SELECT status, COUNT(*) AS n FROM agent_note_queue "
        "GROUP BY status")
    if counts is None:
        return (shell.page_head("Agent notes", crumb="Support")
                + shell.card(shell.empty(
                    "The agent_note_queue table is not available on this "
                    "database yet. Apply migration 0225 and reload.")))
    by_status = {str(r.get("status")): int(r.get("n") or 0) for r in counts}
    matching = by_status.get(status, 0)
    pages = max(1, -(-matching // PAGE_SIZE))
    page = min(page, pages)
    listed = await _rows(
        env, "SELECT entry_id, status, created_at, reviewed_at, note_id, data "
        "FROM agent_note_queue WHERE status=? "
        "ORDER BY created_at DESC, entry_id DESC LIMIT ? OFFSET ?",
        status, PAGE_SIZE, (page - 1) * PAGE_SIZE) or []
    selected_id = _clean_report_id(_first(params, "entry"))
    selected = next(
        (row for row in listed if row.get("entry_id") == selected_id), None)
    if selected_id and selected is None:
        rows = await _rows(
            env, "SELECT entry_id, status, created_at, reviewed_at, note_id, "
            "data FROM agent_note_queue WHERE entry_id=? LIMIT 1",
            selected_id)
        selected = rows[0] if rows else None
    if selected is None and listed:
        selected = listed[0]

    tabs = shell.tabs([
        (key, key.capitalize(),
         _href(admin_query, view="agent-notes", status=key),
         shell.format_int(by_status.get(key, 0)))
        for key in agent_notes.STATUSES], status)
    items = []
    records = {}
    for row in listed:
        entry_id = str(row.get("entry_id") or "")
        record = await _decrypt(env, row.get("data"))
        records[entry_id] = record
        current = selected is not None and entry_id == selected.get("entry_id")
        items.append(
            '<a class="report%s" href="%s"%s><span class="stripe normal">'
            '</span><div><div class="msg">%s</div><div class="meta">'
            '<span>%s</span><span style="margin-left:auto">%s</span></div>'
            '</div></a>' % (
                " on" if current else "",
                _href(admin_query, view="agent-notes", status=status,
                      entry=entry_id),
                ' aria-current="true"' if current else "",
                _html_escape(record.get("title") or record.get("message")
                             or "(no message)"),
                _html_escape(record.get("agent") or "anonymous"),
                shell.time_html(row.get("created_at"), now)))
    if not items:
        items.append(shell.empty("No %s notes." % status))
    pager = ""
    if pages > 1:
        pager = (
            '<div class="pager"><span>Page %d of %d</span><div class="actions">'
            '%s%s</div></div>' % (
                page, pages,
                ('<a class="btn btn-sm" href="%s">Previous</a>' % _href(
                    admin_query, view="agent-notes", status=status,
                    page=page - 1)) if page > 1 else "",
                ('<a class="btn btn-sm" href="%s">Next</a>' % _href(
                    admin_query, view="agent-notes", status=status,
                    page=page + 1)) if page < pages else ""))
    detail = shell.empty("Select a note to read it.")
    if selected:
        entry_id = str(selected.get("entry_id") or "")
        record = records.get(entry_id)
        if record is None:
            record = await _decrypt(env, selected.get("data"))
        row_status = str(selected.get("status") or "pending")
        note_id = str(selected.get("note_id") or "")
        facts = (
            ("Title", _html_escape(record.get("title") or "—")),
            ("Agent", _html_escape(record.get("agent") or "—")),
            ("Sent for", _html_escape(record.get("user") or "—")),
            ("Received", "%s · %s" % (
                shell.time_html(selected.get("created_at"), now),
                _html_escape(shell.utc_label(selected.get("created_at"))))),
            ("Public note", ('<a href="/notes/%s">/notes/%s</a>' % (
                _html_escape(note_id), _html_escape(note_id)))
             if note_id else '<span class="muted">none</span>'),
        )
        form = ""
        if row_status == "pending":
            form = (
                '<form class="form-row" method="post" action="%s" '
                'style="justify-content:space-between;margin-top:18px">%s'
                '<input type="hidden" name="action" value="review_agent_note">'
                '<input type="hidden" name="entry" value="%s">'
                '<span class="hint">Approving publishes it as a public note '
                'owned by you</span><div class="seg" role="group" '
                'aria-label="Review"><button type="submit" name="decision" '
                'value="approve">Approve</button><button type="submit" '
                'name="decision" value="reject">Reject</button></div></form>'
            ) % (
                _href(admin_query, view="agent-notes", entry=entry_id,
                      status=status),
                csrf_field, _html_escape(entry_id))
        detail = (
            '<div class="form-row"><div class="form-row">%s'
            '<span class="mono muted">#%s</span></div></div>'
            '<div class="quote">%s</div><dl class="kv">%s</dl>%s'
        ) % (
            _agent_note_pill(row_status), _html_escape(entry_id),
            _html_escape(record.get("message") or "(no message)"),
            "".join("<dt>%s</dt><dd>%s</dd>" % (label, value)
                    for label, value in facts),
            form)
    return (
        shell.page_head(
            "Agent notes",
            "Anonymous notes left through GET /api/agent-notes, often by AI "
            "agents such as ChatGPT on a person's behalf. Nobody else sees "
            "them until you approve one.", crumb="Support")
        + '<section class="card">' + tabs
        + '<div class="inbox"><div class="inbox-list">' + "".join(items)
        + pager + '</div><div class="detail">' + detail
        + '</div></div></section>'
    )
