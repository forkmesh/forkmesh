import base64
import json
import re
from urllib.parse import quote, urlencode

PROVIDERS = ("mailtrap", "sendgrid", "mailgun")
MAILTRAP_URL = "https://send.api.mailtrap.io/api/send"
MAILTRAP_BULK_URL = "https://bulk.api.mailtrap.io/api/send"
SENDGRID_URL = "https://api.sendgrid.com/v3/mail/send"
MAILGUN_URLS = {"us": "https://api.mailgun.net", "eu": "https://api.eu.mailgun.net"}
KEY_NAMES = {
    "mailtrap": "MAILTRAP_API_TOKEN",
    "sendgrid": "SENDGRID_API_KEY",
    "mailgun": "MAILGUN_API_KEY",
}
DOMAIN_RE = re.compile(
    r"^(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+"
    r"[A-Za-z]{2,63}$")


def _env(env, name):
    return str(getattr(env, name, "") or "").strip()


def api_key(env, provider):
    name = KEY_NAMES.get(provider)
    return _env(env, name) if name else ""


def mailgun_domain(env):
    domain = _env(env, "MAILGUN_DOMAIN").lower()
    return domain if DOMAIN_RE.fullmatch(domain) else ""


def configured(env, provider):
    if not api_key(env, provider):
        return False
    if provider == "mailgun":
        return bool(mailgun_domain(env))
    return True


def provider(env, smtp=False):
    chosen = _env(env, "MAIL_PROVIDER").lower()
    if chosen in PROVIDERS:
        return chosen if configured(env, chosen) else ""
    if chosen == "none":
        return ""
    if smtp:
        return "smtp"
    return "mailtrap" if configured(env, "mailtrap") else ""


def fallback(env):
    if _env(env, "MAIL_PROVIDER").lower() == "none":
        return ""
    return "mailtrap" if configured(env, "mailtrap") else ""


def _mailtrap(env, message):
    payload = {
        "from": {"email": message["sender"], "name": message["senderName"]},
        "to": [{"email": message["to"]}],
        "subject": message["subject"],
        "text": message["text"],
        "category": message["category"],
    }
    if message.get("variables"):
        payload["custom_variables"] = dict(message["variables"])
    if message.get("html"):
        payload["html"] = message["html"]
    if message.get("bulk"):
        url = _env(env, "MAILTRAP_BULK_API_URL") or MAILTRAP_BULK_URL
    else:
        url = _env(env, "MAILTRAP_API_URL") or MAILTRAP_URL
    return url, {
        "content-type": "application/json",
        "authorization": "Bearer " + api_key(env, "mailtrap"),
    }, json.dumps(payload)


def _sendgrid(env, message):
    personalization = {"to": [{"email": message["to"]}]}
    if message.get("variables"):
        personalization["custom_args"] = {
            key: str(value) for key, value in message["variables"].items()}
    content = [{"type": "text/plain", "value": message["text"] or " "}]
    if message.get("html"):
        content.append({"type": "text/html", "value": message["html"]})
    payload = {
        "personalizations": [personalization],
        "from": {"email": message["sender"], "name": message["senderName"]},
        "subject": message["subject"],
        "content": content,
    }
    if message.get("category"):
        payload["categories"] = [str(message["category"])[:255]]
    url = _env(env, "SENDGRID_API_URL") or SENDGRID_URL
    return url, {
        "content-type": "application/json",
        "authorization": "Bearer " + api_key(env, "sendgrid"),
    }, json.dumps(payload)


def _mailgun(env, message):
    region = _env(env, "MAILGUN_REGION").lower()
    base = _env(env, "MAILGUN_API_URL") or MAILGUN_URLS.get(
        region, MAILGUN_URLS["us"])
    url = "%s/v3/%s/messages" % (base.rstrip("/"),
                                 quote(mailgun_domain(env), safe=""))
    name = message["senderName"].replace('"', "'")
    fields = [
        ("from", '"%s" <%s>' % (name, message["sender"])),
        ("to", message["to"]),
        ("subject", message["subject"]),
        ("text", message["text"]),
    ]
    if message.get("html"):
        fields.append(("html", message["html"]))
    if message.get("category"):
        fields.append(("o:tag", str(message["category"])[:128]))
    for key, value in (message.get("variables") or {}).items():
        fields.append(("v:" + key, str(value)))
    token = base64.b64encode(
        ("api:" + api_key(env, "mailgun")).encode("utf-8")).decode("ascii")
    return url, {
        "content-type": "application/x-www-form-urlencoded",
        "authorization": "Basic " + token,
    }, urlencode(fields)


BUILDERS = {"mailtrap": _mailtrap, "sendgrid": _sendgrid, "mailgun": _mailgun}


def build_request(env, provider_name, message):
    return BUILDERS[provider_name](env, message)


def message_id(provider_name, headers, body_text):
    if provider_name == "sendgrid":
        try:
            return str(headers.get("x-message-id") or "").strip()
        except Exception:
            return ""
    try:
        data = json.loads(body_text or "{}")
    except ValueError:
        return ""
    if not isinstance(data, dict):
        return ""
    if provider_name == "mailgun":
        return str(data.get("id") or "").strip().strip("<>")
    ids = data.get("message_ids")
    if isinstance(ids, list) and ids:
        return str(ids[0] or "").strip()
    return ""
