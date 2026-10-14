import re
import urllib.parse
from html import escape, unescape

UNSUBSCRIBE_KINDS = ("notifications", "general_chat", "channel_unread",
                     "direct_unread", "tip",
                     "badge", "reward_wallet_reminder", "package",
                     "marketing", "task")
DELIVERY_EVENTS = ("delivery", "soft bounce", "bounce", "reject")
SUPPRESS_EVENTS = {"bounce": "bounce", "unsubscribe": "unsubscribe",
                   "spam": "spam", "reject": "reject"}
DSN_RE = re.compile(r"^[245]\.\d{1,3}\.\d{1,3}$")
TOKEN_RE = re.compile(r"^([0-9a-f]{32}|[0-9a-f-]{36})\.([0-9a-f]{24})$")
HREF_RE = re.compile(r"""href=(["'])(https?://[^"'\s<>]{1,2000})\1""", re.I)
PIXEL = (b"GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff!"
         b"\xf9\x04\x01\x00\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00"
         b"\x00\x02\x02D\x01\x00;")


def dsn(value):
    value = str(value or "").strip()
    return value if DSN_RE.match(value) else ""


def token(send_id, signature):
    return "%s.%s" % (send_id, signature[:24])


def parse_token(value):
    match = TOKEN_RE.match(str(value or ""))
    return (match.group(1), match.group(2)) if match else (None, None)


def tracked_link(base, tok, target, signature):
    return "%s/api/mail/t/c/%s?%s" % (base, tok, urllib.parse.urlencode(
        {"u": target, "s": signature[:24]}))


def rewrite_links(html, make_url):
    def replace(match):
        target = match.group(2)
        if "/api/mail/t/" in target:
            return match.group(0)
        return "href=%s%s%s" % (match.group(1), escape(make_url(unescape(target))),
                                match.group(1))
    return HREF_RE.sub(replace, html)


def add_pixel(html, url):
    tag = ('<img src="%s" width="1" height="1" alt="" '
           'style="display:block;width:1px;height:1px;border:0">' % url)
    if "</body>" in html:
        return html.replace("</body>", tag + "</body>", 1)
    return html + tag


def href_target(value):
    return safe_target(unescape(str(value or "")))


def safe_target(value):
    parsed = urllib.parse.urlsplit(str(value or ""))
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        return ""
    return urllib.parse.urlunsplit(parsed)
