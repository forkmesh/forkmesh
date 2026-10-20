import hmac
from urllib.parse import parse_qs, quote, urlparse

import marketing_email as me


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


VIEW = "marketing-email"
SCAN = 100
MAX_FORM_BYTES = 256 * 1024
AUDIENCE_SCAN = 20000
NOTICES = {
    "saved": ("Draft saved.", "ok"),
    "deleted": ("Draft deleted.", "ok"),
    "test-sent": ("Test email sent to your account address.", "ok"),
    "test-failed": ("The test email was not sent. Check the mail log for "
                    "the reason.", "bad"),
    "test-no-email": ("Your account has no verified email address to send "
                      "the test to.", "bad"),
    "started": ("Sending started. The cron sends a few emails every ten "
                "minutes, within the daily marketing limit.", "ok"),
    "cancelled": ("Campaign cancelled. Emails already sent stay sent.", "ok"),
    "busy": ("Another campaign is still sending. Wait for it to finish or "
             "cancel it first.", "bad"),
    "postal": ("Set MARKETING_EMAIL_POSTAL before sending: "
               "commercial email must carry a postal address.", "bad"),
    "confirm": ("Tick the confirmation box to start sending.", "bad"),
    "missing": ("That campaign is gone or no longer in that state.", "bad"),
    "invalid": ("The campaign was not saved: %s.", "bad"),
    "csrf": ("Action blocked: invalid or missing CSRF token. Reload the page "
             "and try again.", "bad"),
}
ERRORS = {
    "subject_required": "it needs a subject",
    "body_required": "it needs a message",
    "invalid_kind": "pick Product update or Promotion",
    "link_must_be_https": "the link must start with https://",
}
STATUS_TONES = {"draft": "", "sending": "warn", "sent": "ok",
                "cancelled": "bad"}


def _now():
    return int(Date.now())


def _first(params, key):
    values = (params or {}).get(key) or [""]
    return str(values[0] or "").strip()


def _origin(env):
    return (str(getattr(env, "API_ORIGIN", "") or "")
            or "https://app.forkmesh.com").rstrip("/")


def postal_address(env):
    return " ".join(str(getattr(env, "MARKETING_EMAIL_POSTAL", "")
                        or "").split())[:300]


def _html(body, status=200):
    return Response(body, status=status, headers={
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    })


def _redirect(location):
    return Response("", status=303, headers={
        "location": location, "cache-control": "no-store"})


def _console_path(env):
    path = str(getattr(env, "ADMIN_PATH", "") or "").strip("/")
    return "/" + quote(path, safe="/-_.~") if path else ""


async def _signature(env, name):
    return (await blind_index(env, "marketing-unsub:" + name))[:32]


async def _campaign(env, campaign_id):
    row = await d1_first(
        env, "SELECT campaign_id,status,data,created_by,created_at,"
        "updated_at,started_at,finished_at,cursor_rowid,sent,failed,skipped "
        "FROM marketing_email_campaigns WHERE campaign_id=?",
        clean_string(campaign_id or "", 64))
    if not row:
        return None, {}
    row = dict(row)
    return row, (await decrypt_row(env, row.get("data") or "") or {})


async def compose(env, campaign, name):
    origin = _origin(env)
    unsubscribe = me.unsubscribe_url(origin, name, await _signature(env, name))
    settings = origin + me.SETTINGS_PATH
    postal = postal_address(env)
    text = me.text_body(campaign, settings, unsubscribe, postal)
    intro, body, footer = me.html_parts(campaign, settings, unsubscribe,
                                        postal)
    html = _forkmesh_email_card_html(
        _html_escape(campaign.get("subject") or "ForkMesh"), intro, body,
        action_label=campaign.get("linkLabel") or "Open ForkMesh",
        action_url=campaign.get("linkUrl") or FORKMESH_SITE_URL)
    closing = "</div></div></body></html>"
    html = html.replace(
        closing,
        "<div style=\"margin:22px 0 0;border-top:1px solid #d4d4d8;"
        "padding-top:14px\">" + footer + "</div>" + closing, 1)
    return campaign.get("subject") or "ForkMesh", text, html


async def withdraw(env, account_bi):
    account_bi = clean_string(account_bi or "", 128)
    if not account_bi:
        return False
    rec = await _account_identity_rec_by_bi(env, account_bi)
    if not rec or not me.apply_consent(rec, False, "", 0):
        return False
    await _save_account(env, account_bi, rec)
    return True


async def clear_unsubscribe(env, rec):
    email = clean_string((rec or {}).get("email") or "", 254).strip().lower()
    if not email:
        return
    await d1_run(
        env, "DELETE FROM mail_suppressions WHERE recipient_bi=? "
        "AND reason='marketing_unsubscribe'", await blind_index(env, email))


async def send_due(env):
    if not _is_main_relay(env):
        return {"skipped": "relay"}
    await ensure_schema(env)
    row = await d1_first(
        env, "SELECT campaign_id,data,cursor_rowid FROM "
        "marketing_email_campaigns WHERE status='sending' "
        "ORDER BY started_at LIMIT 1")
    if not row:
        return {"campaign": ""}
    row = dict(row)
    campaign_id = str(row.get("campaign_id") or "")
    if not postal_address(env):
        return {"campaign": campaign_id, "held": "postal_address"}
    campaign = await decrypt_row(env, row.get("data") or "") or {}
    cursor = int(row.get("cursor_rowid") or 0)
    now = _now()
    users = await d1_all(
        env, "SELECT rowid AS rid,user_bi,data FROM users WHERE rowid>? "
        "ORDER BY rowid LIMIT ?", cursor, SCAN)
    sent = failed = skipped = attempted = 0
    held = ""
    for user in users or []:
        user = dict(user)
        if attempted >= me.BATCH:
            break
        rid = int(user.get("rid") or 0)
        account_bi = str(user.get("user_bi") or "")
        rec = await decrypt_row(env, user.get("data") or "")
        if not me.subscribed(rec) or _account_kind(rec) != "user":
            cursor = rid
            continue
        email = clean_string(rec.get("email") or "", 254).strip()
        if not mail_budget.deliverable(email, env):
            skipped += 1
            cursor = rid
            continue
        if await _mail_provider_held(env):
            held = "provider"
            break
        claimed = await _mail_budget_execute(
            env, "INSERT OR IGNORE INTO marketing_email_sends"
            "(campaign_id,account_bi,sent_at) VALUES(?,?,?)",
            campaign_id, account_bi, now)
        if not claimed:
            cursor = rid
            continue
        if not await mail_budget.spend_marketing(
                lambda sql, *args: _mail_budget_execute(env, sql, *args),
                env, now):
            await d1_run(
                env, "DELETE FROM marketing_email_sends WHERE campaign_id=? "
                "AND account_bi=?", campaign_id, account_bi)
            held = "daily_limit"
            break
        cursor = rid
        attempted += 1
        subject, text, html = await compose(
            env, campaign, clean_string(rec.get("name") or "", 64))
        ok = await _send_email(env, email, subject, text, html,
                               email_kind="marketing")
        if ok:
            sent += 1
        else:
            failed += 1
        fresh = await _account_identity_rec_by_bi(env, account_bi)
        if fresh:
            await _save_account(env, account_bi, _stamp_account_email(
                fresh, "marketing", ok, now))
    else:
        if len(users or []) < SCAN:
            held = "done"
    await d1_run(
        env, "UPDATE marketing_email_campaigns SET cursor_rowid=?,"
        "sent=sent+?,failed=failed+?,skipped=skipped+?,updated_at=?,"
        "status=CASE WHEN ?=1 THEN 'sent' ELSE status END,"
        "finished_at=CASE WHEN ?=1 THEN ? ELSE finished_at END "
        "WHERE campaign_id=? AND status='sending'",
        cursor, sent, failed, skipped, now, 1 if held == "done" else 0,
        1 if held == "done" else 0, now, campaign_id)
    return {"campaign": campaign_id, "sent": sent, "failed": failed,
            "skipped": skipped, "held": held}


async def audience(env):
    rows = await d1_all(env, "SELECT data FROM users LIMIT ?", AUDIENCE_SCAN)
    subscribed = 0
    for row in rows or []:
        rec = await decrypt_row(env, dict(row).get("data") or "")
        if me.subscribed(rec) and _account_kind(rec) == "user":
            subscribed += 1
    return subscribed


async def campaign_stats(env, row):
    started = int(row.get("started_at") or 0)
    if not started:
        return {}
    finished = int(row.get("finished_at") or 0) or _now()
    stats = await d1_first(
        env, "SELECT COUNT(*) AS delivered_total,"
        "COALESCE(SUM(CASE WHEN opened_at>0 THEN 1 ELSE 0 END),0) AS opened,"
        "COALESCE(SUM(CASE WHEN clicked_at>0 THEN 1 ELSE 0 END),0) AS clicked,"
        "COALESCE(SUM(CASE WHEN status='unsubscribe' THEN 1 ELSE 0 END),0) "
        "AS unsubscribed,"
        "COALESCE(SUM(CASE WHEN status='spam' THEN 1 ELSE 0 END),0) "
        "AS complaints,"
        "COALESCE(SUM(CASE WHEN status IN ('bounce','soft bounce','reject') "
        "THEN 1 ELSE 0 END),0) AS bounced "
        "FROM mailtrap_email_sends WHERE kind='marketing' "
        "AND sent_at>=? AND sent_at<=?", started, finished) or {}
    stats = dict(stats)
    return {key: int(stats.get(key) or 0) for key in (
        "opened", "clicked", "unsubscribed", "complaints", "bounced")}


def _page(text, status=200):
    return _html(
        "<!doctype html><meta charset=utf-8><meta name=viewport "
        "content='width=device-width'><title>ForkMesh</title>"
        "<body style='font:16px system-ui;margin:40px auto;max-width:520px;"
        "padding:0 16px'>" + text + "</body>", status)


async def unsubscribe_handler(env, request):
    await ensure_schema(env)
    params = parse_qs(urlparse(request.url).query)
    name = _first(params, "a").lower()
    signature = _first(params, "s").lower()
    if not me.valid_unsubscribe(name, signature) or not hmac.compare_digest(
            signature, await _signature(env, name)):
        return _page("<p>This unsubscribe link is not valid.</p>", 404)
    if method_name(request) == "POST":
        name_bi, rec = await _account_row(env, name)
        if name_bi and rec and me.apply_consent(rec, False, "", 0):
            await _save_account(env, name_bi, rec)
        return _page(
            "<p>You are unsubscribed from ForkMesh product updates and "
            "offers.</p><p>Emails about your pings, account and security are "
            "not affected. You can turn updates back on in your "
            "<a href='%s'>email settings</a>.</p>" % _html_escape(
                _origin(env) + me.SETTINGS_PATH))
    return _page(
        "<form method=post><p>Stop ForkMesh product updates and offers for "
        "<b>%s</b>?</p><button type=submit>Unsubscribe</button></form>"
        % _html_escape(name))


async def admin_handler(env, request, params, csrf_field="", admin_query="",
                        account=""):
    await ensure_schema(env)
    if method_name(request) == "POST":
        return await _handle_post(env, request, params, admin_query, account)
    shell = _admin_shell
    banner = ""
    notice = NOTICES.get(_first(params, "notice"))
    if notice:
        text = notice[0]
        if "%s" in text:
            text = text % ERRORS.get(_first(params, "error"), "check the form")
        banner = shell.banner(text, notice[1])
    content = await render(env, params, csrf_field, admin_query)
    url = urlparse(request.url)
    page = shell.render_page(
        "Marketing email", banner + content, active=VIEW,
        console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        subpages=shell.subpage_base(env),
        next_path=url.path + (("?" + url.query) if url.query else ""))
    return _html(page)


async def _handle_post(env, request, params, admin_query, account):
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = parse_qs(str(raw or "")[:MAX_FORM_BYTES], keep_blank_values=True)

    def location(notice, **extra):
        return _console_path(env) + _admin_href(
            admin_query, view=VIEW, notice=notice, **extra).replace(
                "&amp;", "&")

    if not _admin_csrf_ok(env, form):
        return _redirect(location("csrf"))
    action = _first(form, "action")
    campaign_id = _first(form, "campaign")
    now = _now()
    if action == "save":
        row, existing = (await _campaign(env, campaign_id)
                         if campaign_id else (None, {}))
        if campaign_id and (not row or row.get("status") != "draft"):
            return _redirect(location("missing"))
        data, error = me.campaign_data({
            "kind": _first(form, "kind"), "subject": _first(form, "subject"),
            "body": ((form.get("body") or [""])[0] or ""),
            "linkUrl": _first(form, "link_url"),
            "linkLabel": _first(form, "link_label")}, existing)
        if error:
            return _redirect(location("invalid", error=error,
                                      edit=campaign_id or None))
        sealed = await encrypt_row(env, data)
        if row:
            await d1_run(
                env, "UPDATE marketing_email_campaigns SET data=?,updated_at=? "
                "WHERE campaign_id=? AND status='draft'",
                sealed, now, campaign_id)
        else:
            campaign_id = _ap_uuid()
            await d1_run(
                env, "INSERT INTO marketing_email_campaigns(campaign_id,status,"
                "data,created_by,created_at,updated_at) "
                "VALUES(?,'draft',?,?,?,?)",
                campaign_id, sealed, clean_string(account or "", 64), now, now)
        await _audit_sensitive_action(
            env, account, "admin.marketing_email_save",
            "marketing_email_campaigns", campaign_id, "success",
            {"kind": data.get("kind")})
        return _redirect(location("saved"))
    row, data = await _campaign(env, campaign_id)
    if not row:
        return _redirect(location("missing"))
    if action == "delete" and row.get("status") == "draft":
        await d1_run(
            env, "DELETE FROM marketing_email_campaigns WHERE campaign_id=? "
            "AND status='draft'", campaign_id)
        await _audit_sensitive_action(
            env, account, "admin.marketing_email_delete",
            "marketing_email_campaigns", campaign_id, "success", {})
        return _redirect(location("deleted"))
    if action == "test":
        _, rec = await _account_row(env, account)
        email = clean_string((rec or {}).get("email") or "", 254).strip()
        if not email or not (rec or {}).get("email_verified"):
            return _redirect(location("test-no-email"))
        subject, text, html = await compose(env, data, account)
        ok = await _send_email(env, email, "[Test] " + subject, text, html,
                               email_kind="marketing")
        await _audit_sensitive_action(
            env, account, "admin.marketing_email_test",
            "marketing_email_campaigns", campaign_id,
            "success" if ok else "failure", {})
        return _redirect(location("test-sent" if ok else "test-failed"))
    if action == "start" and row.get("status") == "draft":
        if not postal_address(env):
            return _redirect(location("postal"))
        if _first(form, "confirm") != "1":
            return _redirect(location("confirm"))
        busy = await d1_first(
            env, "SELECT campaign_id FROM marketing_email_campaigns "
            "WHERE status='sending' LIMIT 1")
        if busy:
            return _redirect(location("busy"))
        await d1_run(
            env, "UPDATE marketing_email_campaigns SET status='sending',"
            "started_at=?,updated_at=?,cursor_rowid=0 "
            "WHERE campaign_id=? AND status='draft'", now, now, campaign_id)
        await _audit_sensitive_action(
            env, account, "admin.marketing_email_start",
            "marketing_email_campaigns", campaign_id, "success",
            {"kind": data.get("kind")})
        return _redirect(location("started"))
    if action == "cancel" and row.get("status") == "sending":
        await d1_run(
            env, "UPDATE marketing_email_campaigns SET status='cancelled',"
            "finished_at=?,updated_at=? WHERE campaign_id=? "
            "AND status='sending'", now, now, campaign_id)
        await _audit_sensitive_action(
            env, account, "admin.marketing_email_cancel",
            "marketing_email_campaigns", campaign_id, "success", {})
        return _redirect(location("cancelled"))
    return _redirect(location("missing"))


def _field(name, label, value, maxlength, hint="", wide=False):
    return (
        '<div class="field%s"><label for="me-%s">%s</label>'
        '<input id="me-%s" type="text" name="%s" value="%s" maxlength="%d" '
        'autocomplete="off">%s</div>' % (
            " wide" if wide else "", name, label, name, name,
            _html_escape(value or ""), maxlength,
            ('<span class="hint">%s</span>' % hint) if hint else ""))


def _editor(csrf_field, admin_query, campaign_id, data):
    kind = data.get("kind") or "update"
    options = "".join(
        '<option value="%s"%s>%s</option>' % (
            key, " selected" if key == kind else "", label)
        for key, label in me.KIND_LABELS.items())
    return (
        '<form method="post" action="%s">%s'
        '<input type="hidden" name="action" value="save">'
        '<input type="hidden" name="campaign" value="%s">'
        '<div class="form-grid">'
        '<div class="field"><label for="me-kind">Type</label>'
        '<select id="me-kind" name="kind">%s</select></div>'
        '%s'
        '<div class="field wide"><label for="me-body">Message</label>'
        '<textarea id="me-body" name="body" rows="10" maxlength="%d">%s'
        '</textarea><span class="hint">Plain text. A blank line starts a new '
        'paragraph. The footer with the unsubscribe link, the settings link '
        'and the postal address is added to every email.</span></div>'
        '%s%s'
        '<div class="wide form-row" style="justify-content:flex-end">%s'
        '<button class="btn btn-primary" type="submit">Save draft</button>'
        '</div></div></form>' % (
            _admin_href(admin_query, view=VIEW), csrf_field,
            _html_escape(campaign_id or ""), options,
            _field("subject", "Subject", data.get("subject"), me.MAX_SUBJECT),
            me.MAX_BODY, _html_escape(data.get("body") or ""),
            _field("link_url", "Button link", data.get("linkUrl"), 2000,
                   "Optional https:// link, for example a blog post with "
                   "utm_source=email&amp;utm_medium=email&amp;utm_campaign=..."),
            _field("link_label", "Button label", data.get("linkLabel"),
                   me.MAX_LINK_LABEL),
            ('<a class="btn" href="%s">Cancel</a>' % _admin_href(
                admin_query, view=VIEW)) if campaign_id else ""))


def _action_form(csrf_field, admin_query, campaign_id, action, label,
                 extra="", confirm="", primary=False):
    return (
        '<form method="post" class="inline" action="%s"%s>%s'
        '<input type="hidden" name="action" value="%s">'
        '<input type="hidden" name="campaign" value="%s">%s'
        '<button type="submit" class="btn btn-sm%s">%s</button></form>' % (
            _admin_href(admin_query, view=VIEW),
            (' onsubmit="return confirm(\'%s\')"' % confirm) if confirm else "",
            csrf_field, action, _html_escape(campaign_id), extra,
            " btn-primary" if primary else "", label))


async def render(env, params, csrf_field="", admin_query="", now=None):
    shell = _admin_shell
    now = int(now or _now())
    subscribers = await audience(env)
    rows = await d1_all(
        env, "SELECT campaign_id,status,data,created_by,created_at,updated_at,"
        "started_at,finished_at,sent,failed,skipped FROM "
        "marketing_email_campaigns ORDER BY created_at DESC LIMIT 50")
    editing = _first(params, "edit")
    edit_data = {}
    body = []
    for row in rows or []:
        row = dict(row)
        data = await decrypt_row(env, row.get("data") or "") or {}
        campaign = me.public_campaign(row, data, await campaign_stats(env, row))
        cid = campaign["id"]
        if cid == editing and campaign["status"] == "draft":
            edit_data = data
        actions = []
        if campaign["status"] == "draft":
            actions.append('<a class="btn btn-sm" href="%s">Edit</a>' % (
                _admin_href(admin_query, view=VIEW, edit=cid)))
            actions.append(_action_form(
                csrf_field, admin_query, cid, "test", "Send test to me"))
            actions.append(_action_form(
                csrf_field, admin_query, cid, "start", "Start sending",
                '<label class="hint" style="margin:0 6px"><input type="checkbox" '
                'name="confirm" value="1"> to %s subscribers</label>'
                % shell.format_int(subscribers),
                "Email this campaign to every subscriber?", primary=True))
            actions.append(_action_form(
                csrf_field, admin_query, cid, "delete", "Delete",
                confirm="Delete this draft?"))
        elif campaign["status"] == "sending":
            actions.append(_action_form(
                csrf_field, admin_query, cid, "test", "Send test to me"))
            actions.append(_action_form(
                csrf_field, admin_query, cid, "cancel", "Cancel",
                confirm="Stop sending this campaign?"))
        results = ""
        if campaign["startedAt"]:
            results = (
                "%s sent · %s not sent · %s skipped<br>"
                '<span class="muted">%s opened · %s clicked · %s unsubscribed '
                "· %s complaints · %s bounced</span>" % tuple(
                    shell.format_int(campaign.get(key) or 0) for key in (
                        "sent", "failed", "skipped", "opened", "clicked",
                        "unsubscribed", "complaints", "bounced")))
        body.append(
            "<tr><td><b>%s</b><br><span class=\"muted\">%s · by %s</span></td>"
            "<td>%s</td><td>%s</td><td>%s</td>"
            '<td class="r">%s</td></tr>' % (
                _html_escape(campaign["subject"]),
                me.KIND_LABELS.get(campaign["kind"], "Update"),
                _html_escape(campaign["createdBy"] or "—"),
                shell.pill(campaign["status"].capitalize(),
                           STATUS_TONES.get(campaign["status"], "")),
                shell.time_html(campaign["startedAt"] or campaign["createdAt"],
                                now),
                results or '<span class="muted">—</span>',
                " ".join(actions)))
    postal = postal_address(env)
    kpis = (
        '<div class="grid cols-4">'
        + shell.kpi("Subscribers", shell.format_int(subscribers),
                    "opted in, verified, active")
        + shell.kpi("Daily limit", shell.format_int(
            mail_budget.marketing_limit(env)), "MAIL_MARKETING_DAILY_LIMIT")
        + shell.kpi("Pace", "%d per 10 min" % me.BATCH, "cron marketing-email")
        + shell.kpi("Postal address", "Set" if postal else "Missing",
                    postal or "MARKETING_EMAIL_POSTAL",
                    tone="" if postal else "bad")
        + "</div>")
    table = shell.table(
        "<th>Campaign</th><th>Status</th><th>Started</th><th>Results</th>"
        '<th class="r">Actions</th>', body, "No campaigns yet.")
    return (
        shell.page_head(
            "Marketing email",
            "Product updates and promotions emailed to accounts that turned "
            "on Product updates and offers, at signup or in their email "
            "settings. Opens, clicks, unsubscribes and complaints count for "
            "90 days after a send.",
            crumb="Growth")
        + kpis
        + shell.card(_editor(csrf_field, admin_query, editing if edit_data
                             else "", edit_data),
                     title="Edit draft" if edit_data else "New campaign")
        + shell.card(title="Campaigns", table=table))
