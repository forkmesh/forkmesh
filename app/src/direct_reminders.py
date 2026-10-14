def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


DIRECT_UNREAD_KIND = "direct_unread"
REMINDER_DELAY_MS = 24 * 60 * 60 * 1000
REMINDER_MAX_ROWS = 32
REMINDER_MAX_RECIPIENTS = 4
REMINDER_MAX_LISTED = 10
CANDIDATES_SQL = (
    "SELECT p.conversation_id,p.participant_bi,COUNT(*) AS stored,"
    "MIN(h.ts) AS first_ts "
    "FROM chat_history h JOIN chat_direct_participants p "
    "ON p.conversation_id=substr(h.room_key,13,32) "
    "WHERE h.room_key>='chat-direct:' AND h.room_key<'chat-direct;' "
    "AND h.ts>=? AND h.ts>p.unread_reminded_at AND h.msg_id LIKE '%:n' "
    "AND substr(h.msg_id,41,1)=':' "
    "AND substr(h.msg_id,42,16)<>substr(p.participant_bi,1,16) "
    "GROUP BY p.conversation_id,p.participant_bi "
    "HAVING MIN(h.ts)<=? ORDER BY first_ts LIMIT ?")
MARK_REMINDED_SQL = (
    "UPDATE chat_direct_participants "
    "SET unread_reminded_at=MAX(unread_reminded_at,?) "
    "WHERE conversation_id=? AND participant_bi=?")


def _plural(count, word):
    return "%d %s%s" % (count, word, "" if count == 1 else "s")


async def _candidates(env, now):
    rows = await d1_all(
        env,
        CANDIDATES_SQL,
        now - CHAT_HISTORY_RETAIN_MS,
        now - REMINDER_DELAY_MS,
        REMINDER_MAX_ROWS,
    )
    pending = {}
    for row in rows or []:
        conversation_id = str(row.get("conversation_id") or "")
        account_bi = str(row.get("participant_bi") or "")
        stored = int(row.get("stored") or 0)
        if conversation_id and account_bi and stored > 0:
            pending.setdefault(account_bi, []).append(
                (conversation_id, stored))
    return pending


async def _sender_name(env, conversation_id, recipient_name):
    row = await d1_first(
        env,
        "SELECT data FROM chat_direct_conversations WHERE conversation_id=?",
        conversation_id,
    )
    if not row:
        return ""
    try:
        record = await decrypt_row(env, row.get("data", ""))
    except Exception:
        return ""
    participants = (record or {}).get("participants")
    if not isinstance(participants, list) or len(participants) != 2:
        return ""
    names = [clean_string(str(value or ""), MAX_NODE_NAME).strip().lower()
             for value in participants]
    if recipient_name not in names:
        return ""
    return names[1] if names[0] == recipient_name else names[0]


def reminder_email(name, senders):
    senders = list(senders)[:REMINDER_MAX_LISTED]
    total = sum(count for _sender, count in senders)
    greeting = name or "there"
    chat_url = FORKMESH_SITE_URL + "chat"
    if len(senders) == 1:
        subject = "ForkMesh: %s from %s waiting on the server" % (
            _plural(total, "unread direct message"), senders[0][0])
    else:
        subject = "ForkMesh: %s from %d people waiting on the server" % (
            _plural(total, "unread direct message"), len(senders))
    lines = ["%s: %s" % (sender, _plural(count, "message"))
             for sender, count in senders]
    text = (
        "Hi " + greeting + ",\n\n"
        "You have direct messages that have been waiting for more than a "
        "day:\n\n"
        + "\n".join("- " + line for line in lines) + "\n\n"
        "ForkMesh keeps an encrypted copy of a direct message on the server "
        "only until one of your devices receives it. Open ForkMesh chat to "
        "read them so they are deleted from the server: " + chat_url
        + "\n\n"
        "Messages are end-to-end encrypted, so their contents are not sent "
        "over email. Unread messages are deleted from the server after 7 "
        "days.\n"
        "To stop these emails, turn off \"Unread direct messages\" in your "
        "notification settings."
    )
    rows = "".join(
        "<tr><td style=\"padding:10px 14px;" + (
            "border-top:1px solid #d0d7de;" if index else "")
        + "font-size:15px;font-weight:700\"><a href=\""
        + _html_escape(chat_url) + "\" style=\"color:#24292f;"
        "text-decoration:none\">" + _html_escape(sender) + "</a></td>"
        "<td align=\"right\" style=\"padding:10px 14px;" + (
            "border-top:1px solid #d0d7de;" if index else "")
        + "white-space:nowrap\"><span style=\"display:inline-block;"
        "border-radius:999px;background:#ddf4ff;color:#0969da;"
        "padding:2px 9px;font-size:12px;font-weight:700\">"
        + _html_escape(_plural(count, "message")) + "</span></td></tr>"
        for index, (sender, count) in enumerate(senders)
    )
    items = (
        "<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" "
        "cellspacing=\"0\" style=\"border:1px solid #d0d7de;"
        "border-radius:8px;border-collapse:separate;background:#ffffff;"
        "margin:0 0 14px\">" + rows + "</table>"
    )
    html = _forkmesh_email_card_html(
        _html_escape(_plural(total, "unread direct message")),
        "Hi <strong class=\"fm-strong\" style=\"color:#f5f5f5\">"
        + _html_escape(greeting)
        + "</strong>, you have direct messages that have been waiting for "
        "more than a day.",
        items
        + "<p class=\"fm-text\" style=\"margin:8px 0 0;color:#d4d4d8;"
        "font-size:14px\">ForkMesh keeps an encrypted copy of a direct "
        "message on the server only until one of your devices receives it. "
        "Open ForkMesh chat to read them so they are deleted from the "
        "server.</p>",
        "<p class=\"fm-muted\" style=\"margin:22px 0 0;color:#8a8a93;"
        "font-size:12px\">Messages are end-to-end encrypted, so their "
        "contents are not sent over email. Unread messages are deleted from "
        "the server after 7 days. To stop these emails, turn off \"Unread "
        "direct messages\" in your notification settings.</p>",
        action_label="Open chat",
        action_url=chat_url,
    )
    return subject, text, html


async def _remind(env, account_bi, conversations, now):
    rec, email_rec, email = await _notification_email_recipient(
        env, account_bi)
    if not email or not isinstance(rec, dict):
        return False
    if _account_kind(rec) != "user":
        return False
    if DIRECT_UNREAD_KIND in ping_muted_kinds(rec):
        return False
    if not notification_email_enabled(email_rec, DIRECT_UNREAD_KIND):
        return False
    name = clean_string(rec.get("name", ""), MAX_NODE_NAME).lower()
    senders = []
    for conversation_id, stored in conversations:
        sender = await _sender_name(env, conversation_id, name)
        if sender:
            senders.append((sender, stored))
    if not senders:
        return False
    subject, text, html = reminder_email(name, senders)
    ok = await _send_email(
        env, email, subject, text, html, email_kind=DIRECT_UNREAD_KIND)
    _stamp_account_email(email_rec, DIRECT_UNREAD_KIND, ok, now)
    if email_rec is rec:
        await _save_account(env, account_bi, rec)
    return bool(ok)


async def _mark_reminded(env, account_bi, conversations, now):
    for conversation_id, _stored in conversations:
        await d1_run(
            env, MARK_REMINDED_SQL, int(now), conversation_id, account_bi)


async def send_reminders(env):
    if not _is_main_relay(env):
        return 0
    await ensure_schema(env)
    now = int(Date.now())
    pending = await _candidates(env, now)
    sent = 0
    for account_bi in list(pending)[:REMINDER_MAX_RECIPIENTS]:
        conversations = pending[account_bi]
        try:
            if await _remind(env, account_bi, conversations, now):
                sent += 1
        except Exception:
            pass
        try:
            await _mark_reminded(env, account_bi, conversations, now)
        except Exception:
            continue
    return sent
