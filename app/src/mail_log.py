from __future__ import annotations

import html as _html
import json
import re
import time

import repository_webhooks as _webhooks


CHANNEL_NAME = "email-log"
CHANNEL_KIND = "notices"
CHANNEL_TOPIC = (
    "Server-visible notices: one line for every email ForkMesh sends. "
    "Not end-to-end encrypted. Only you can see this channel.")
SENDER_ID = "system:mail-log"
SENDER_NAME = "Mail log (ForkMesh)"
MAX_LINES = 40
MAX_TEXT = 7600
MAX_BLOCKS = 20
MAX_BACKLOG = 1000
MAX_ATTEMPTS = _webhooks.MAX_ATTEMPTS
SUBJECT_LIMIT = 120
ERROR_LIMIT = 160
BODY_LIMIT = 200
VIA_LIMIT = 60
BUDGET_LIMIT = 160
SOURCE_LIMIT = 80
DETAIL_LIMIT = 200
TRANSPORTS = {
    "postfix": "local mail (Postfix on the core)",
    "mailtrap": "Mailtrap API",
    "mailtrap_package": "Mailtrap API, the package's own token",
    "mailtrap_webhook": "Mailtrap",
    "ids_direct": "Mailtrap API, IDS direct",
}

RESULTS = {
    "accepted": "accepted",
    "rejected": "REJECTED",
    "failed": "FAILED",
    "suppressed": "suppressed",
    "delivered": "delivered",
    "bounced": "BOUNCED",
    "soft_bounced": "soft-bounced",
    "complained": "COMPLAINED",
    "blocked": "BLOCKED",
    "opened": "opened",
    "clicked": "clicked",
    "viewed": "viewed",
}
WEBHOOK_RESULTS = {
    "delivery": "delivered",
    "bounce": "bounced",
    "soft bounce": "soft_bounced",
    "spam": "complained",
    "reject": "blocked",
    "suspension": "blocked",
}
KIND_LABELS = {
    "account": "Account notification",
    "verification": "Verification",
    "password_reset": "Password reset",
    "password_change": "Password change",
    "tip": "Tip notification",
    "badge": "Badge awarded",
    "feedback": "Onboarding feedback",
    "notifications": "Notification digest",
    "general_chat": "General chat digest",
    "channel_unread": "Unread channel reminder",
    "direct_unread": "Unread direct message reminder",
    "cron_watchdog": "Cron alert",
    "status_alert": "Status alert",
    "security_report": "Security report",
    "security": "IDS alert",
    "package": "Package mail",
    "outside_app": "Not sent by the ForkMesh app",
    "ids_direct": "IDS direct mail",
    "task": "Task activity",
    "footer": "Own mail (tracked footer)",
}
KIND_EMOJI = {
    "account": "🔔",
    "verification": "🪪",
    "password_reset": "🔑",
    "password_change": "🔒",
    "tip": "💸",
    "badge": "🏅",
    "feedback": "📣",
    "notifications": "📬",
    "general_chat": "💬",
    "channel_unread": "📢",
    "direct_unread": "📨",
    "cron_watchdog": "⏰",
    "status_alert": "🚦",
    "security_report": "🛡️",
    "security": "🚨",
    "package": "📦",
    "outside_app": "📮",
    "ids_direct": "🛰️",
    "task": "🗂️",
    "footer": "🖼️",
}
DEFAULT_KIND_EMOJI = "✉️"
RESULT_EMOJI = {
    "accepted": "✅",
    "rejected": "⛔",
    "failed": "❌",
    "suppressed": "🚫",
    "delivered": "📥",
    "bounced": "↩️",
    "soft_bounced": "↪️",
    "complained": "😠",
    "blocked": "🛑",
    "opened": "👀",
    "clicked": "🖱️",
    "viewed": "🔎",
}
FIELD_EMOJI = {
    "To": "📨",
    "From": "📤",
    "Subject": "📝",
    "Body": "📄",
    "Reason": "⚠️",
    "Detail": "🔍",
    "Budget": "📊",
    "Route": "🛤️",
    "Source": "🧰",
    "Message ID": "🆔",
}
NODE_NOTE_TRANSPORTS = ("ids_direct", "postfix")
NODE_NOTE_BATCH = 200
NODE_NOTE_MAX_BYTES = 4096

_EMAIL_RE = re.compile(r"[A-Za-z0-9._%+'-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
_URL_RE = re.compile(r"(?i)\b(?:https?|ftp)://\S+|\bwww\.\S+")
_TOKEN_RE = re.compile(r"[A-Za-z0-9_\-+/=.]{24,}")
_CODE_RE = re.compile(r"\b\d{6,}\b")
_CONTROL_RE = re.compile(r"[\x00-\x1f\x7f]+")
_MESSAGE_ID_RE = re.compile(r"[^\x21-\x7e]+")
_HIDDEN_HTML_RE = re.compile(r"(?is)<(style|script|head|title)\b.*?</\1\s*>")
_TAG_RE = re.compile(r"(?s)<[^>]*>")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _scrub_words(text):
    text = _TOKEN_RE.sub("[redacted]", text)
    return _CODE_RE.sub("[code]", text)


def scrub(value, limit):
    text = _CONTROL_RE.sub(" ", str(value or ""))
    text = _URL_RE.sub("[link]", text)
    pieces = []
    last = 0
    for match in _EMAIL_RE.finditer(text):
        pieces.append(_scrub_words(text[last:match.start()]))
        pieces.append(match.group(0).lower())
        last = match.end()
    pieces.append(_scrub_words(text[last:]))
    text = " ".join("".join(pieces).split())
    return text if len(text) <= limit else text[:limit - 1] + "…"


def excerpt(text="", html=""):
    value = str(text or "")
    if not value.strip() and html:
        value = _html.unescape(_TAG_RE.sub(
            " ", _HIDDEN_HTML_RE.sub(" ", str(html))))
    return scrub(value, BODY_LIMIT)


def clean_message_id(value):
    return _MESSAGE_ID_RE.sub("", str(value or "")).strip("<>")[:200]


def kind_label(kind):
    key = str(kind or "account").strip().lower()
    return KIND_LABELS.get(key) or scrub(key, 40) or "Account notification"


def kind_title(kind):
    key = str(kind or "account").strip().lower()
    return "%s %s" % (KIND_EMOJI.get(key) or DEFAULT_KIND_EMOJI,
                      kind_label(key))


def result_title(result):
    key = str(result or "").strip().lower()
    return "%s %s" % (RESULT_EMOJI.get(key) or RESULT_EMOJI["failed"],
                      RESULTS.get(key, "FAILED"))


def field_label(label):
    emoji = FIELD_EMOJI.get(label)
    return ("%s %s" % (emoji, label)) if emoji else label


def note(kind, recipient, subject, result, status=0, error="", at=0,
         message_id="", node="", sender="", via="", body="", budget="",
         body_html="", source="", detail=""):
    result = result if result in RESULTS else "failed"
    entry = {
        "at": int(at or 0),
        "kind": str(kind or "account").strip().lower()[:40],
        "to": str(recipient or "").strip().lower()[:254],
        "subject": scrub(subject, SUBJECT_LIMIT),
        "result": result,
    }
    try:
        code = int(status or 0)
    except (TypeError, ValueError):
        code = 0
    if code:
        entry["status"] = code
    if error:
        entry["error"] = scrub(error, ERROR_LIMIT)
    message_id = clean_message_id(message_id)
    if message_id:
        entry["messageId"] = message_id
    node = str(node or "").strip().lower()
    if re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,62}", node):
        entry["node"] = node
    sender = str(sender or "").strip().lower()
    if sender:
        entry["from"] = sender[:254]
    for key, value in (("via", scrub(TRANSPORTS.get(via, via), VIA_LIMIT)),
                       ("body", excerpt(body, body_html)),
                       ("budget", scrub(budget, BUDGET_LIMIT)),
                       ("source", scrub(source, SOURCE_LIMIT)),
                       ("detail", scrub(detail, DETAIL_LIMIT))):
        if value:
            entry[key] = value
    return entry


def show_sender(address):
    value = str(address or "").strip().lower()
    if value == "<>":
        return "<> (a bounce notice)"
    return value[:254] if value else "(no sender)"


def show_recipient(address):
    value = str(address or "").strip().lower()
    return value[:254] if "@" in value else "(no recipient)"


def headline(entry):
    stamp = time.strftime(
        "%Y-%m-%d %H:%M:%S UTC",
        time.gmtime(int(entry.get("at") or 0) / 1000))
    return " · ".join((kind_title(entry.get("kind")),
                       result_title(entry.get("result")), stamp))


def fields(entry):
    rows = [("To", show_recipient(entry.get("to")))]
    if entry.get("from"):
        rows.append(("From", show_sender(entry["from"])))
    if entry.get("subject"):
        rows.append(("Subject", str(entry["subject"])))
    if entry.get("body"):
        rows.append(("Body", str(entry["body"])))
    reason = []
    if entry.get("status"):
        reason.append("HTTP %d" % int(entry["status"]))
    if entry.get("error"):
        reason.append(str(entry["error"]))
    if reason:
        rows.append(("Reason", ": ".join(reason)))
    if entry.get("detail"):
        rows.append(("Detail", str(entry["detail"])))
    if entry.get("budget"):
        rows.append(("Budget", str(entry["budget"])))
    route = str(entry.get("via") or "")
    if entry.get("node"):
        route = (route + " on " if route else "on ") + str(entry["node"])
    if route:
        rows.append(("Route", route))
    if entry.get("kind") == "outside_app":
        rows.append(("Source", "sent by " + str(entry["source"])
                     if entry.get("source") else
                     "no app send record: a tool, a cron job or a bounce"))
    if entry.get("messageId"):
        rows.append(("Message ID", str(entry["messageId"])))
    return rows


def section(entry):
    return {"type": "section", "text": headline(entry),
            "fields": ["%s: %s" % (field_label(label), value)
                       for label, value in fields(entry)]}


def block_text(block):
    return "\n".join([block["text"]] + list(block.get("fields") or ()))


def line(entry):
    return block_text(section(entry))


def compose(entries, dropped=0):
    items = [(section(entry), 1) for entry in entries]
    if dropped:
        items.append(({"type": "context", "text": (
            "%d older note%s dropped: the log backlog was over %d." % (
                dropped, "" if dropped == 1 else "s", MAX_BACKLOG))}, 0))
    messages = []
    text, count, blocks = "", 0, []
    for block, weight in items:
        value = block_text(block)[:MAX_TEXT]
        candidate = (text + "\n\n" + value) if text else value
        if blocks and (len(candidate) > MAX_TEXT or
                       len(blocks) >= MAX_BLOCKS):
            messages.append((text, count, blocks))
            text, count, blocks = value, weight, [block]
        else:
            text, count = candidate, count + weight
            blocks.append(block)
    if blocks:
        messages.append((text, count, blocks))
    return messages


def webhook_note(event, at, subject="", body=""):
    status = str(event.get("event") or event.get("type") or "").lower()
    result = WEBHOOK_RESULTS.get(status)
    if not result:
        return None
    variables = event.get("custom_variables")
    if not isinstance(variables, dict):
        variables = event.get("customVariables")
    variables = variables if isinstance(variables, dict) else {}
    kind = variables.get("forkmesh_kind") or ""
    if event.get("forkmesh_outside_app"):
        kind = "outside_app"
    category = str(event.get("category") or "").strip()
    if not kind and category:
        for key, label in KIND_LABELS.items():
            if label.lower() == category.lower():
                kind = key
                break
        kind = kind or category
    reason = (event.get("reason") or event.get("response")
              or event.get("bounce_category") or "")
    try:
        code = int(event.get("response_code") or 0)
    except (TypeError, ValueError):
        code = 0
    return note(kind or "account", event.get("email"), subject, result,
                status=code if result != "delivered" else 0,
                error=reason if result != "delivered" else "",
                at=at, message_id=event.get("message_id") or "",
                node=event.get("node") or "", sender=event.get("sender") or "",
                via="postfix" if event.get("node") else "mailtrap_webhook",
                body=body)


def owner_name(env):
    value = str(getattr(env, "MAIL_LOG_OWNER", "") or "").strip().lower()
    return value if re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,38}", value) else ""


def is_notices(record):
    return isinstance(record, dict) and record.get("kind") == CHANNEL_KIND


async def enqueue(env, entry):
    try:
        if not owner_name(env):
            return False
        now = int(entry.get("at") or 0) or int(time.time() * 1000)
        entry["at"] = now
        sealed = await encrypt_row(env, entry)
        await d1_run(
            env,
            "INSERT INTO mail_log_outbox (created_at,next_at,attempts,data) "
            "VALUES (?,?,0,?)",
            now, now, sealed)
        return True
    except Exception as error:
        try:
            print("mail log note not queued: %s" % type(error).__name__)
        except Exception:
            pass
        return False


async def _owner(env, name):
    owner_bi, record = await _account_row(env, name)
    if not owner_bi or not isinstance(record, dict):
        return "", {}
    if record.get("status") != "active" or _account_kind(record) != "user":
        return "", {}
    return owner_bi, record


async def channel_for(env, owner_bi, owner):
    name_bi = await blind_index(
        env, "chat-channel-notices:" + owner + ":" + CHANNEL_NAME)
    row = await d1_first(
        env,
        "SELECT channel_id,data,created_by_bi,key_version FROM chat_channels "
        "WHERE name_bi=?",
        name_bi)
    if row:
        record = await decrypt_row(env, row.get("data"))
        if not is_notices(record) or str(
                row.get("created_by_bi") or "") != owner_bi:
            raise RuntimeError("mail_log_channel_mismatch")
        return str(row.get("channel_id") or ""), int(
            row.get("key_version") or 1), record
    now = int(time.time() * 1000)
    channel_id = _ap_uuid()
    record = {
        "name": CHANNEL_NAME,
        "createdBy": owner,
        "visibility": "private",
        "kind": CHANNEL_KIND,
        "notices": CHANNEL_NAME,
        "topic": CHANNEL_TOPIC,
    }
    await d1_run(
        env,
        "INSERT INTO chat_channels "
        "(channel_id,name_bi,data,created_by_bi,created_at,updated_at,"
        "key_version) VALUES (?,?,?,?,?,?,1)",
        channel_id, name_bi, await encrypt_row(env, record), owner_bi,
        now, now)
    try:
        await notify_account_event(env, owner, "private-channels")
    except Exception:
        pass
    return channel_id, 1, record


async def _retry(env, rows, now):
    for row in rows:
        attempts = int(row.get("attempts") or 0) + 1
        if attempts >= MAX_ATTEMPTS:
            await d1_run(
                env, "DELETE FROM mail_log_outbox WHERE id=?", row.get("id"))
            continue
        await d1_run(
            env,
            "UPDATE mail_log_outbox SET attempts=?,next_at=? WHERE id=?",
            attempts, now + _webhooks.backoff_ms(attempts), row.get("id"))


async def _trim(env):
    row = await d1_first(env, "SELECT COUNT(*) AS n FROM mail_log_outbox")
    extra = int((row or {}).get("n") or 0) - MAX_BACKLOG
    if extra <= 0:
        return 0
    await d1_run(
        env,
        "DELETE FROM mail_log_outbox WHERE id IN (SELECT id FROM "
        "mail_log_outbox ORDER BY id LIMIT ?)",
        extra)
    return extra


def node_note(data, node, at):
    if not isinstance(data, dict):
        return None
    result = str(data.get("result") or "").strip().lower()
    if result not in RESULTS:
        return None
    kind = str(data.get("kind") or "ids_direct").strip().lower()
    via = str(data.get("via") or "ids_direct").strip().lower()
    return note(kind if kind in KIND_LABELS else "ids_direct",
                data.get("to"), data.get("subject"), result,
                status=data.get("status") or 0, error=data.get("error") or "",
                at=at, node=node, sender=data.get("from") or "",
                via=via if via in NODE_NOTE_TRANSPORTS else "ids_direct",
                message_id=data.get("messageId") or "",
                body=data.get("body") or "", source=data.get("source") or "")


async def ingest_node_notes(env):
    rows = await d1_all(
        env,
        "SELECT id,created_at,node,data FROM mail_log_node_notes "
        "ORDER BY id LIMIT ?",
        NODE_NOTE_BATCH)
    moved = 0
    for row in rows or []:
        raw = str(row.get("data") or "")
        entry = None
        if len(raw) <= NODE_NOTE_MAX_BYTES:
            try:
                entry = node_note(json.loads(raw), row.get("node") or "",
                                  int(row.get("created_at") or 0))
            except ValueError:
                entry = None
        if entry is None:
            entry = note("ids_direct", "", "", "failed",
                         error="unreadable node mail note",
                         at=int(row.get("created_at") or 0),
                         node=row.get("node") or "")
        if not await enqueue(env, entry):
            break
        await d1_run(
            env, "DELETE FROM mail_log_node_notes WHERE id=?", row.get("id"))
        moved += 1
    return moved


async def drain(env, deliver, limit=MAX_LINES):
    owner = owner_name(env)
    if not owner:
        return {"posted": 0, "reason": "no_owner"}
    try:
        await ingest_node_notes(env)
    except Exception as error:
        print("mail log node notes not ingested: %s" % type(error).__name__)
    now = int(time.time() * 1000)
    rows = await d1_all(
        env,
        "SELECT id,attempts,data FROM mail_log_outbox WHERE next_at<=? "
        "ORDER BY id LIMIT ?",
        now, int(limit))
    if not rows:
        return {"posted": 0}
    dropped = await _trim(env)
    owner_bi, record = await _owner(env, owner)
    if not owner_bi:
        await _retry(env, rows, now)
        return {"posted": 0, "reason": "owner_unavailable"}
    kept = []
    unreadable = []
    for row in rows:
        try:
            entry = await decrypt_row(env, row.get("data"))
        except Exception:
            entry = None
        if isinstance(entry, dict):
            kept.append((row, entry))
        else:
            unreadable.append(row.get("id"))
    await _delete(env, unreadable)
    posted = 0
    if kept:
        try:
            channel_id, key_version, channel = await channel_for(
                env, owner_bi, owner)
        except Exception as error:
            print("mail log post failed: %s" % type(error).__name__)
            await _retry(env, [row for row, _ in kept], now)
            return {"posted": 0, "retry": len(kept)}
        for index, (text, count, blocks) in enumerate(compose(
                [entry for _, entry in kept], dropped)):
            batch = [row for row, _ in kept[posted:posted + count]]
            try:
                await deliver(channel_id, key_version, {
                    "type": "chat",
                    "id": _ap_uuid(),
                    "senderId": SENDER_ID,
                    "sender": SENDER_NAME,
                    "accountKind": "user",
                    "ts": now + index,
                    "channel": "#" + CHANNEL_NAME,
                    "text": text,
                    "blocks": blocks,
                    "webhook": True,
                    "notice": True,
                })
            except Exception as error:
                print("mail log post failed: %s" % type(error).__name__)
                rest = [row for row, _ in kept[posted:]]
                await _retry(env, rest, now)
                return {"posted": posted, "retry": len(rest)}
            await _delete(env, [row.get("id") for row in batch])
            posted += count
    return {"posted": posted, "dropped": dropped}


async def _delete(env, ids):
    if not ids:
        return
    placeholders = ",".join("?" for _ in ids)
    await d1_run(
        env, "DELETE FROM mail_log_outbox WHERE id IN (" + placeholders + ")",
        *ids)
