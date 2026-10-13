import html as _html
import re
from urllib.parse import urlencode, urlsplit

KINDS = ("update", "promotion")
KIND_LABELS = {"update": "Product update", "promotion": "Promotion"}
STATUSES = ("draft", "sending", "sent", "cancelled")
SOURCES = ("signup", "settings")
MAX_SUBJECT = 150
MAX_BODY = 20000
MAX_LINK_LABEL = 60
BATCH = 2
SETTINGS_PATH = "/dashboard/settings/notifications#marketing-email"
UNSUBSCRIBE_PATH = "/api/marketing-email/unsubscribe"
SIGNATURE_RE = re.compile(r"^[0-9a-f]{32}$")
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,62}$")


def _text(value, limit):
    value = str(value if value is not None else "")
    value = "".join(ch for ch in value if ch in "\n\t" or ord(ch) >= 32)
    return value.replace("\r\n", "\n").replace("\r", "\n").strip()[:limit]


def _int(value):
    try:
        return max(0, int(value or 0))
    except (TypeError, ValueError):
        return 0


def since(rec):
    raw = rec.get("marketing_email") if isinstance(rec, dict) else None
    return _int(raw.get("at")) if isinstance(raw, dict) else 0


def apply_consent(rec, wanted, source, now):
    if not isinstance(rec, dict):
        return False
    if not bool(wanted):
        return rec.pop("marketing_email", None) is not None
    if since(rec):
        return False
    rec["marketing_email"] = {
        "at": _int(now), "source": source if source in SOURCES else "settings"}
    return True


def subscribed(rec):
    if not isinstance(rec, dict) or not since(rec):
        return False
    if str(rec.get("status") or "active") != "active":
        return False
    if str(rec.get("kind") or "user") != "user":
        return False
    return bool(rec.get("email") and rec.get("email_verified"))


def https_url(value):
    value = _text(value, 2000)
    parts = urlsplit(value)
    if parts.scheme != "https" or not parts.netloc or " " in value:
        return ""
    return value


def campaign_data(data, existing=None):
    data = data if isinstance(data, dict) else {}
    base = dict(existing or {})
    for key, limit in (("subject", MAX_SUBJECT), ("body", MAX_BODY),
                       ("linkLabel", MAX_LINK_LABEL)):
        if key in data:
            base[key] = _text(data.get(key), limit)
    if "subject" in base:
        base["subject"] = " ".join(base["subject"].split())
    if "kind" in data:
        base["kind"] = _text(data.get("kind"), 20).lower()
    if "linkUrl" in data:
        base["linkUrl"] = https_url(data.get("linkUrl"))
        if _text(data.get("linkUrl"), 2000) and not base["linkUrl"]:
            return base, "link_must_be_https"
    base.setdefault("kind", "update")
    if base["kind"] not in KINDS:
        return base, "invalid_kind"
    if not base.get("subject"):
        return base, "subject_required"
    if not base.get("body"):
        return base, "body_required"
    if base.get("linkUrl") and not base.get("linkLabel"):
        base["linkLabel"] = "Read more"
    if not base.get("linkUrl"):
        base["linkUrl"] = ""
        base["linkLabel"] = ""
    return base, ""


def paragraphs(body):
    return [part.strip() for part in re.split(r"\n\s*\n", body or "")
            if part.strip()]


def unsubscribe_url(origin, name, signature):
    return "%s%s?%s" % (origin.rstrip("/"), UNSUBSCRIBE_PATH,
                        urlencode({"a": name, "s": signature}))


def valid_unsubscribe(name, signature):
    return bool(NAME_RE.match(str(name or ""))
                and SIGNATURE_RE.match(str(signature or "")))


def footer_lines(campaign, settings_url, unsubscribe, postal_address):
    lines = [
        "You get this %s because you turned on Product updates and offers "
        "for your ForkMesh account." % (
            "promotion" if campaign.get("kind") == "promotion"
            else "update"),
        "Unsubscribe: " + unsubscribe,
        "Email settings: " + settings_url,
    ]
    if postal_address:
        lines.append("ForkMesh, " + postal_address)
    return lines


def text_body(campaign, settings_url, unsubscribe, postal_address):
    parts = paragraphs(campaign.get("body"))
    if campaign.get("linkUrl"):
        parts.append("%s: %s" % (campaign.get("linkLabel") or "Read more",
                                 campaign["linkUrl"]))
    return "\n\n".join(parts) + "\n\n--\n" + "\n".join(footer_lines(
        campaign, settings_url, unsubscribe, postal_address))


def html_parts(campaign, settings_url, unsubscribe, postal_address):
    body = "".join(
        "<p style=\"margin:0 0 14px;color:#24292f;font-size:15px;"
        "line-height:1.55;white-space:pre-wrap\">%s</p>" % _html.escape(part)
        for part in paragraphs(campaign.get("body")))
    footer = (
        "<p style=\"margin:0;color:#57606a;font-size:12px;line-height:1.5\">"
        "You get this %s because you turned on Product updates and offers "
        "for your ForkMesh account. <a href=\"%s\" style=\"color:#0969da\">"
        "Unsubscribe</a> &middot; <a href=\"%s\" style=\"color:#0969da\">"
        "Email settings</a>%s</p>" % (
            "promotion" if campaign.get("kind") == "promotion" else "update",
            _html.escape(unsubscribe, quote=True),
            _html.escape(settings_url, quote=True),
            ("<br>ForkMesh, " + _html.escape(postal_address))
            if postal_address else ""))
    intro = _html.escape(KIND_LABELS.get(campaign.get("kind"), "Update")
                         + " from ForkMesh")
    return intro, body, footer


def public_campaign(row, data, stats=None):
    row = row or {}
    data = data if isinstance(data, dict) else {}
    out = {
        "id": str(row.get("campaign_id") or ""),
        "status": str(row.get("status") or "draft"),
        "kind": data.get("kind") or "update",
        "subject": data.get("subject") or "",
        "body": data.get("body") or "",
        "linkUrl": data.get("linkUrl") or "",
        "linkLabel": data.get("linkLabel") or "",
        "createdBy": str(row.get("created_by") or ""),
        "createdAt": _int(row.get("created_at")),
        "updatedAt": _int(row.get("updated_at")),
        "startedAt": _int(row.get("started_at")),
        "finishedAt": _int(row.get("finished_at")),
        "sent": _int(row.get("sent")),
        "failed": _int(row.get("failed")),
        "skipped": _int(row.get("skipped")),
    }
    if stats:
        out.update(stats)
    return out
