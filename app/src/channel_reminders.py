import re


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


CHANNEL_UNREAD_KIND = "channel_unread"
REMINDER_DELAY_MS = 30 * 60 * 1000
REMINDER_WINDOW_MS = 60 * 60 * 1000
REMINDER_LOOKBACK_MS = 24 * 60 * 60 * 1000
REMINDER_MAX_CHANNELS = 4
REMINDER_MAX_RECIPIENTS = 4
REMINDER_MAX_COUNTED = 200
REMINDER_MAX_LISTED = 10
MARK_READ_MAX_ACCOUNTS = 100
CHANNEL_ID_RE = re.compile(r"^[0-9a-f]{32}$")
ACCOUNT_BI_RE = re.compile(r"^[0-9a-f]{64}$")
ROOM_KEY_RE = re.compile(r"^chat-channel:([0-9a-f]{32}):v[0-9]+$")
NOTIFY_SQL = (
    "(body LIKE '%\"notify\":true%' OR body LIKE '%\"notify\": true%')")
MARK_READ_SQL = (
    "INSERT INTO chat_channel_reads (channel_id,account_bi,read_at) VALUES "
    "%s ON CONFLICT(channel_id,account_bi) DO UPDATE SET "
    "read_at=MAX(chat_channel_reads.read_at,excluded.read_at)")
MARK_REMINDED_SQL = (
    "INSERT INTO chat_channel_reads (channel_id,account_bi,reminded_at) "
    "VALUES %s ON CONFLICT(channel_id,account_bi) DO UPDATE SET "
    "reminded_at=MAX(chat_channel_reads.reminded_at,excluded.reminded_at)")


def _room_bounds(channel_id):
    prefix = "chat-channel:" + channel_id + ":v"
    return prefix, prefix[:-1] + "w"


def _plural(count, word):
    return "%d %s%s" % (count, word, "" if count == 1 else "s")


async def mark_read(env, channel_id, accounts, now):
    channel_id = str(channel_id or "")
    if not CHANNEL_ID_RE.fullmatch(channel_id):
        return 0
    unique = []
    for account in accounts or ():
        account = str(account or "")
        if ACCOUNT_BI_RE.fullmatch(account) and account not in unique:
            unique.append(account)
    unique = unique[:MARK_READ_MAX_ACCOUNTS]
    if not unique:
        return 0
    args = []
    for account in unique:
        args.extend((channel_id, account, int(now)))
    await d1_run(
        env, MARK_READ_SQL % ",".join(["(?,?,?)"] * len(unique)), *args)
    return len(unique)


async def _mark_reminded(env, account_bi, channel_ids, now):
    if not channel_ids:
        return
    args = []
    for channel_id in channel_ids:
        args.extend((channel_id, account_bi, int(now)))
    await d1_run(
        env, MARK_REMINDED_SQL % ",".join(["(?,?,?)"] * len(channel_ids)),
        *args)


async def _candidate_channels(env, now):
    rows = await d1_all(
        env,
        "SELECT room_key, MIN(ts) AS first_ts FROM chat_history "
        "WHERE ts>? AND ts<=? AND room_key>='chat-channel:' "
        "AND room_key<'chat-channel;' AND " + NOTIFY_SQL + " "
        "GROUP BY room_key ORDER BY first_ts LIMIT ?",
        now - REMINDER_DELAY_MS - REMINDER_WINDOW_MS,
        now - REMINDER_DELAY_MS,
        REMINDER_MAX_CHANNELS * 4,
    )
    channel_ids = []
    for row in rows or []:
        match = ROOM_KEY_RE.fullmatch(str(row.get("room_key") or ""))
        if match and match.group(1) not in channel_ids:
            channel_ids.append(match.group(1))
    return channel_ids[:REMINDER_MAX_CHANNELS]


async def _channel_audience(env, channel_id):
    channel = await d1_first(
        env,
        "SELECT data,created_by_bi,created_at FROM chat_channels "
        "WHERE channel_id=?",
        channel_id,
    )
    if not channel:
        return "", {}
    record = await decrypt_row(env, channel.get("data", ""))
    if (
        not isinstance(record, dict)
        or record.get("visibility") == "public"
        or record.get("kind") == "notices"
    ):
        return "", {}
    audience = {}
    creator = str(channel.get("created_by_bi") or "")
    if creator:
        audience[creator] = int(channel.get("created_at") or 0)
    members = await d1_all(
        env,
        "SELECT member_bi,joined_at FROM chat_channel_members "
        "WHERE channel_id=?",
        channel_id,
    )
    for member in members or []:
        member_bi = str(member.get("member_bi") or "")
        if member_bi:
            audience.setdefault(member_bi, int(member.get("joined_at") or 0))
    name = clean_string(record.get("name", ""), 80).strip() or "channel"
    return name, audience


def unread_counts(times, audience, reads, now):
    due = now - REMINDER_DELAY_MS
    counts = {}
    for account_bi, joined_at in audience.items():
        read = reads.get(account_bi) or {}
        read_at = int(read.get("read_at") or 0)
        if int(read.get("reminded_at") or 0) > read_at:
            continue
        since = max(read_at, int(joined_at or 0))
        unread = [ts for ts in times if ts > since]
        if unread and min(unread) <= due:
            counts[account_bi] = len(unread)
    return counts


async def _channel_unread(env, channel_id, audience, now):
    low, high = _room_bounds(channel_id)
    rows = await d1_all(
        env,
        "SELECT ts FROM chat_history WHERE room_key>=? AND room_key<? "
        "AND ts>? AND " + NOTIFY_SQL + " ORDER BY ts DESC LIMIT ?",
        low, high, now - REMINDER_LOOKBACK_MS, REMINDER_MAX_COUNTED,
    )
    times = [int(row.get("ts") or 0) for row in rows or []]
    if not times:
        return {}
    read_rows = await d1_all(
        env,
        "SELECT account_bi,read_at,reminded_at FROM chat_channel_reads "
        "WHERE channel_id=?",
        channel_id,
    )
    reads = {str(row.get("account_bi") or ""): row for row in read_rows or []}
    return unread_counts(times, audience, reads, now)


def reminder_email(name, channels):
    channels = list(channels)[:REMINDER_MAX_LISTED]
    total = sum(count for _channel, count in channels)
    greeting = name or "there"
    chat_url = FORKMESH_SITE_URL + "chat"
    if len(channels) == 1:
        subject = "ForkMesh: %s in #%s" % (
            _plural(total, "unread message"), channels[0][0])
    else:
        subject = "ForkMesh: %s in %d channels" % (
            _plural(total, "unread message"), len(channels))
    lines = ["#%s: %s" % (channel, _plural(count, "unread message"))
             for channel, count in channels]
    text = (
        "Hi " + greeting + ",\n\n"
        "People posted in channels you're in while you were away:\n\n"
        + "\n".join("- " + line for line in lines) + "\n\n"
        "Messages are end-to-end encrypted, so their contents are not sent "
        "over email. Open ForkMesh chat to read and reply: " + chat_url
        + "\n\n"
        "This is a one-time reminder: you won't get another one for these "
        "channels until you've opened them.\n"
        "To stop these emails, turn off \"Unread channel messages\" in your "
        "notification settings."
    )
    rows = "".join(
        "<tr><td style=\"padding:10px 14px;" + (
            "border-top:1px solid #d0d7de;" if index else "")
        + "font-size:15px;font-weight:700\"><a href=\""
        + _html_escape(chat_url) + "\" style=\"color:#24292f;"
        "text-decoration:none\">#" + _html_escape(channel) + "</a></td>"
        "<td align=\"right\" style=\"padding:10px 14px;" + (
            "border-top:1px solid #d0d7de;" if index else "")
        + "white-space:nowrap\"><span style=\"display:inline-block;"
        "border-radius:999px;background:#ddf4ff;color:#0969da;"
        "padding:2px 9px;font-size:12px;font-weight:700\">"
        + _html_escape(_plural(count, "unread message")) + "</span></td></tr>"
        for index, (channel, count) in enumerate(channels)
    )
    items = (
        "<table role=\"presentation\" width=\"100%\" cellpadding=\"0\" "
        "cellspacing=\"0\" style=\"border:1px solid #d0d7de;"
        "border-radius:8px;border-collapse:separate;background:#ffffff;"
        "margin:0 0 14px\">" + rows + "</table>"
    )
    html = _forkmesh_email_card_html(
        _html_escape(_plural(total, "unread message")),
        "Hi <strong class=\"fm-strong\" style=\"color:#f5f5f5\">"
        + _html_escape(greeting)
        + "</strong>, people posted in channels you're in while you were "
        "away.",
        items
        + "<p class=\"fm-text\" style=\"margin:8px 0 0;color:#d4d4d8;"
        "font-size:14px\">Messages are end-to-end encrypted, so their "
        "contents are not sent over email. Open ForkMesh chat to read and "
        "reply.</p>",
        "<p class=\"fm-muted\" style=\"margin:22px 0 0;color:#8a8a93;"
        "font-size:12px\">This is a one-time reminder: you won't get another "
        "one for these channels until you've opened them. To stop these "
        "emails, turn off \"Unread channel messages\" in your notification "
        "settings.</p>",
        action_label="Open chat",
        action_url=chat_url,
    )
    return subject, text, html


async def _remind(env, account_bi, channels, now):
    rec, email_rec, email = await _notification_email_recipient(
        env, account_bi)
    if not email or not isinstance(rec, dict):
        return False
    if _account_kind(rec) != "user":
        return False
    if CHANNEL_UNREAD_KIND in ping_muted_kinds(rec):
        return False
    if not notification_email_enabled(email_rec, CHANNEL_UNREAD_KIND):
        return False
    name = clean_string(rec.get("name", ""), MAX_NODE_NAME).lower()
    subject, text, html = reminder_email(name, channels)
    ok = await _send_email(
        env, email, subject, text, html, email_kind=CHANNEL_UNREAD_KIND)
    _stamp_account_email(email_rec, CHANNEL_UNREAD_KIND, ok, now)
    if email_rec is rec:
        await _save_account(env, account_bi, rec)
    return bool(ok)


async def send_reminders(env):
    if not _is_main_relay(env):
        return 0
    await ensure_schema(env)
    now = int(Date.now())
    names = {}
    pending = {}
    for channel_id in await _candidate_channels(env, now):
        name, audience = await _channel_audience(env, channel_id)
        if not audience:
            continue
        names[channel_id] = name
        counts = await _channel_unread(env, channel_id, audience, now)
        for account_bi, count in counts.items():
            pending.setdefault(account_bi, []).append((channel_id, count))
    sent = 0
    for account_bi in sorted(pending)[:REMINDER_MAX_RECIPIENTS]:
        channels = pending[account_bi]
        try:
            if await _remind(
                    env, account_bi,
                    [(names[channel_id], count)
                     for channel_id, count in channels],
                    now):
                sent += 1
            await _mark_reminded(
                env, account_bi,
                [channel_id for channel_id, _count in channels], now)
        except Exception:
            continue
    return sent
