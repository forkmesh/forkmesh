import hmac
import html as _html
import re
import secrets

import instance_privacy


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


EVENTS = {
    "open": ("opened", "opened_at", "open_count", "opened the email"),
    "click": ("clicked", "clicked_at", "click_count",
              "clicked a link in the email"),
    "view": ("viewed", "viewed_at", "view_count",
             "opened the page linked in the email"),
}
FOLLOWERS = {"task": "task_email"}
COLUMNS = ("send_id,kind,ref,data,opened_at,clicked_at,viewed_at,"
           "open_count,click_count,view_count")
VIEW_SCOPE = "mail-view:"
SEND_ID_RE = re.compile(r"^[0-9a-f]{32}$|^[0-9a-f-]{36}$")
TOKEN_RE = re.compile(r"^([0-9a-f]{32})\.([0-9a-f]{24})$")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
KIND_LIMIT = 40
REF_LIMIT = 200
SUBJECT_LIMIT = 300
VARIABLE_PREFIX = "forkmesh_"
VARIABLE_LIMIT = 8
FOOTER_SCOPE = "mail-footer:"
FOOTER_REPEAT_MS = 60 * 1000
FOOTER_LOGO = "https://app.forkmesh.com/assets/logo-96.png"
FOOTER_LINK = "https://forkmesh.com"


def _name(value):
    name = str(value or "").strip().lower()
    return name if NAME_RE.match(name) else ""


def _send_id(value):
    send_id = str(value or "").strip().lower()
    return send_id if SEND_ID_RE.match(send_id) else ""


def parse_token(value):
    match = TOKEN_RE.match(str(value or "").strip().lower())
    return (match.group(1), match.group(2)) if match else ("", "")


def detail(event, who, recipient, count, sender=""):
    text = ("@" + who if who else "The recipient") + " " + EVENTS[event][3]
    if who and recipient and who != recipient:
        text += " sent to @" + recipient
    if _name(sender):
        text += " from @" + _name(sender)
    if count > 1:
        text += " (%d times)" % count
    return text


async def recipient_index(env, email):
    return await blind_index(env, str(email or "").strip().lower()[:254])


async def view_token(env, send_id):
    return "%s.%s" % (
        send_id, (await blind_index(env, VIEW_SCOPE + send_id))[:24])


def clean_variables(value):
    if not isinstance(value, dict):
        return {}
    kept = {}
    for key, item in value.items():
        key = str(key or "")
        if (key.startswith(VARIABLE_PREFIX) and len(key) <= 64
                and isinstance(item, (str, int)) and len(kept) < VARIABLE_LIMIT):
            kept[key] = str(item)[:200]
    return kept


async def sent(env, send_id, kind, email, subject, now, ref="",
               recipient="", message_id="", sender="", variables=None,
               body="", body_html=""):
    send_id = _send_id(send_id)
    if not send_id or not email:
        return False
    data = {"email": str(email), "subject": str(subject or "")[:SUBJECT_LIMIT]}
    excerpt = mail_log.excerpt(body, body_html)
    if excerpt:
        data["body"] = excerpt
    if _name(recipient):
        data["recipient"] = _name(recipient)
    if _name(sender):
        data["sender"] = _name(sender)
    variables = clean_variables(variables)
    if variables:
        data["variables"] = variables
    if message_id:
        data["messageId"] = str(message_id).strip("<>")[:200]
    await d1_run(
        env,
        "INSERT OR IGNORE INTO email_sends "
        "(send_id,kind,recipient_bi,ref,data,sent_at) VALUES (?,?,?,?,?,?)",
        send_id, str(kind or "account")[:KIND_LIMIT],
        await recipient_index(env, email), str(ref or "")[:REF_LIMIT],
        await encrypt_row(env, data), int(now))
    return True


async def sent_since(env, kind, email, since):
    row = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM email_sends "
        "WHERE recipient_bi=? AND kind=? AND sent_at>?",
        await recipient_index(env, email), str(kind or ""), int(since))
    return int((row or {}).get("n") or 0)


async def send_details(env, send_id):
    send_id = _send_id(send_id)
    if not send_id:
        return {}
    row = await d1_first(
        env, "SELECT data FROM email_sends WHERE send_id=?", send_id)
    if not row:
        return {}
    data = await decrypt_row(env, row.get("data") or "")
    if not isinstance(data, dict):
        return {}
    return {"subject": str(data.get("subject") or ""),
            "body": str(data.get("body") or "")}


async def send_variables(env, send_id):
    send_id = _send_id(send_id)
    if not send_id:
        return {}
    row = await d1_first(
        env, "SELECT data FROM email_sends WHERE send_id=?", send_id)
    if not row:
        return {}
    data = await decrypt_row(env, row.get("data") or "")
    return clean_variables(data.get("variables")) if isinstance(
        data, dict) else {}


async def _follow(env, row, event, who, subject, first, now):
    follower = globals().get(FOLLOWERS.get(str(row.get("kind") or ""), ""))
    if follower is None or not row.get("ref"):
        return
    try:
        await follower.mail_event(
            env, str(row.get("ref") or ""), event, who, subject, first, now)
    except Exception as error:
        print("mail activity follower %s failed: %s" % (
            row.get("kind"), type(error).__name__))


async def tracked(env, send_id, event, now, message_id="", viewer="",
                  ref=None):
    if instance_privacy.active(env):
        return False
    send_id = _send_id(send_id)
    if event not in EVENTS or not send_id:
        return False
    row = await d1_first(
        env, "SELECT " + COLUMNS + " FROM email_sends WHERE send_id=?",
        send_id)
    if not row or (ref is not None and str(row.get("ref") or "") != ref):
        return False
    result, column, counter, _verb = EVENTS[event]
    first = not int(row.get(column) or 0)
    count = int(row.get(counter) or 0) + 1
    await d1_run(
        env,
        "UPDATE email_sends SET %s=CASE WHEN %s=0 THEN ? ELSE %s END,"
        "%s=%s+1 WHERE send_id=?" % (column, column, column, counter,
                                     counter),
        int(now), send_id)
    data = await decrypt_row(env, row.get("data") or "")
    data = data if isinstance(data, dict) else {}
    recipient = _name(data.get("recipient"))
    who = _name(viewer) or recipient
    subject = str(data.get("subject") or "")
    await _follow(env, row, event, who, subject, first, now)
    await mail_log.enqueue(env, mail_log.note(
        str(row.get("kind") or "account"), data.get("email") or "", subject,
        result, at=now, message_id=message_id or data.get("messageId") or "",
        body=str(data.get("body") or ""),
        detail=detail(event, who, recipient, count,
                      sender=data.get("sender") or "")))
    return True


async def viewed(env, token, ref, viewer, now):
    send_id, signature = parse_token(token)
    if not send_id:
        return False
    expected = (await blind_index(env, VIEW_SCOPE + send_id))[:24]
    if not hmac.compare_digest(expected, signature):
        return False
    return await tracked(env, send_id, "view", now, viewer=viewer,
                         ref=str(ref or ""))


async def footer_token(env, footer_id):
    return "%s.%s" % (
        footer_id, (await blind_index(env, FOOTER_SCOPE + footer_id))[:24])


def footer_html(image_url):
    return (
        '<table role="presentation" cellpadding="0" cellspacing="0" '
        'border="0" style="margin-top:16px;border-collapse:collapse"><tr>'
        '<td style="background:#18181b;border-radius:6px;padding:4px 6px">'
        '<a href="%s"><img src="%s" width="25" height="30" alt="" '
        'style="display:block;border:0;width:25px;height:30px"></a></td>'
        '<td style="padding-left:8px;font:13px/1.4 -apple-system,'
        'Segoe UI,Helvetica,Arial,sans-serif;color:#71717a">'
        '<a href="%s" style="color:#71717a;text-decoration:none">Sent with '
        '<strong>ForkMesh</strong></a></td></tr></table>') % (
            FOOTER_LINK, _html.escape(image_url, quote=True), FOOTER_LINK)


def text_html(text):
    return "<div style=\"white-space:pre-wrap\">%s</div>" % _html.escape(
        str(text or ""))


def add_footer(html, image_url):
    tag = footer_html(image_url)
    if "</body>" in html:
        return html.replace("</body>", tag + "</body>", 1)
    return html + tag


async def footer(env, owner_bi, account, email, now):
    data = await encrypt_row(env, {"account": _name(account),
                                   "email": str(email or "")[:254]})
    row = await d1_first(
        env, "SELECT footer_id,opened_at,last_open_at,open_count "
        "FROM email_footers WHERE owner_bi=?", owner_bi)
    if row:
        await d1_run(env, "UPDATE email_footers SET data=? WHERE footer_id=?",
                     data, row.get("footer_id"))
        return row
    await d1_run(
        env, "INSERT OR IGNORE INTO email_footers "
        "(footer_id,owner_bi,data,created_at) VALUES (?,?,?,?)",
        secrets.token_hex(16), owner_bi, data, int(now))
    return await d1_first(
        env, "SELECT footer_id,opened_at,last_open_at,open_count "
        "FROM email_footers WHERE owner_bi=?", owner_bi)


def footer_detail(account, email, count):
    who = "@" + _name(account) if _name(account) else "A ForkMesh account"
    text = "%s's tracked footer was opened in an email sent from %s" % (
        who, email or "their own mail app")
    if count > 1:
        text += " (%d opens so far)" % count
    return text


async def footer_opened(env, token, now):
    footer_id, signature = parse_token(token)
    if not footer_id:
        return False
    expected = (await blind_index(env, FOOTER_SCOPE + footer_id))[:24]
    if not hmac.compare_digest(expected, signature):
        return False
    row = await d1_first(
        env, "SELECT data,last_open_at,open_count FROM email_footers "
        "WHERE footer_id=?", footer_id)
    if not row:
        return False
    if int(row.get("last_open_at") or 0) > int(now) - FOOTER_REPEAT_MS:
        return True
    count = int(row.get("open_count") or 0) + 1
    await d1_run(
        env, "UPDATE email_footers SET opened_at=CASE WHEN opened_at=0 "
        "THEN ? ELSE opened_at END,last_open_at=?,open_count=open_count+1 "
        "WHERE footer_id=?", int(now), int(now), footer_id)
    data = await decrypt_row(env, row.get("data") or "")
    data = data if isinstance(data, dict) else {}
    await mail_log.enqueue(env, mail_log.note(
        "footer", "", "", "opened", at=now, sender=data.get("email") or "",
        detail=footer_detail(data.get("account"), data.get("email"), count)))
    return True
