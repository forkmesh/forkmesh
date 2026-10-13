from __future__ import annotations

import base64
import hashlib
import hmac
import json
import re
from urllib.parse import urlencode, urlparse

import repository_webhooks as _webhooks


SCOPE_CATALOG = (
    ("repos:read", "Read the repositories this install covers",
     "Repository names, descriptions and branches of the account or "
     "organization that installs the app."),
    ("issues:read", "Read issues",
     "Issues and issue comments, and issue events."),
    ("issues:write", "Open issues and comment as the app",
     "Posts are signed by the app's bot, never by a person."),
    ("pulls:read", "Read pull requests",
     "Pull requests, their comments and merge events."),
    ("pulls:write", "Comment on pull requests as the app",
     "Posts are signed by the app's bot, never by a person."),
    ("chat:read", "See chat messages meant for the app",
     "Messages that mention the app and messages apps post in public "
     "channels. Other chat stays end-to-end encrypted and never reaches "
     "an app."),
    ("chat:write", "Post in public chat channels as the app",
     "Messages appear as \"<App> (app)\"."),
    ("pings:read", "Receive your Pings",
     "The title and link of each Ping you receive, as events."),
    ("pings:write", "Send you Pings",
     "Pings to the person (or the organization admins) who installed it."),
    ("agents:run", "Start coding agents on issues",
     "Asks the repository's host to start an agent; the host decides."),
    ("commands", "Add slash commands",
     "Commands you type in chat are sent to the app."),
)
SCOPES = tuple(key for key, _label, _hint in SCOPE_CATALOG)
SCOPE_LABELS = {key: label for key, label, _hint in SCOPE_CATALOG}

EVENT_SCOPES = {
    "issue.opened": ("issues:read",),
    "issue.comment": ("issues:read",),
    "pull.opened": ("pulls:read",),
    "pull.comment": ("pulls:read",),
    "pull.merged": ("pulls:read",),
    "repository.push": ("repos:read",),
    "chat.message": ("chat:read",),
    "app.mention": ("issues:read", "pulls:read", "chat:read"),
    "ping.created": ("pings:read",),
    "app.uninstalled": (),
}
EVENT_TYPES = tuple(EVENT_SCOPES)

TARGET_USER = "user"
TARGET_ORG = "org"

LISTING_STATES = ("private", "pending", "listed", "rejected", "suspended")
REVIEW_ACTIONS = {
    "approve": ("pending",),
    "reject": ("pending",),
    "suspend": ("listed", "pending", "private"),
    "reinstate": ("suspended",),
}

ORG_REQUEST_POLICIES = ("any", "allowlist", "none")

CLIENT_ID_RE = re.compile(r"^fmapp_[0-9a-f]{24}$")
APP_ID_RE = re.compile(r"^[0-9a-f]{32}$")
SECRET_RE = re.compile(r"^fm(cs|ss|at|rt|ac|ru)_[0-9a-f]{48,64}$")
ACCESS_PREFIX = "fmat_"
REFRESH_PREFIX = "fmrt_"
CODE_PREFIX = "fmac_"
CLIENT_SECRET_PREFIX = "fmcs_"
SIGNING_SECRET_PREFIX = "fmss_"
RESPONSE_URL_PREFIX = "fmru_"

ACCESS_TTL_MS = 60 * 60 * 1000
REFRESH_TTL_MS = 30 * 24 * 60 * 60 * 1000
CODE_TTL_MS = 10 * 60 * 1000
RESPONSE_URL_TTL_MS = 30 * 60 * 1000
RESPONSE_URL_MAX_USES = 5
INTERACTIVE_MESSAGE_TTL_MS = 7 * 24 * 60 * 60 * 1000

MAX_APPS_PER_ACCOUNT = 20
MAX_REDIRECT_URIS = 10
MAX_COMMANDS = 10
MAX_NAME = 40
MAX_DESCRIPTION = 400
MAX_URL = 600
MAX_ICON_BYTES = 24 * 1024
MAX_BLOCKS = 20
MAX_BLOCK_TEXT = 3000
MAX_FIELDS = 10
MAX_ELEMENTS = 5
MAX_OPTIONS = 25
MAX_BLOCKS_JSON = 16 * 1024
MAX_MESSAGE_TEXT = 4000

RATE_WINDOW_MS = 60 * 1000
RATE_PER_INSTALL = 120
RATE_PER_APP = 600

SIGNATURE_SCHEME = _webhooks.WEBHOOK_SCHEME
TOLERANCE_SECONDS = _webhooks.TOLERANCE_SECONDS
MAX_EVENT_ATTEMPTS = _webhooks.MAX_ATTEMPTS
DISABLE_AFTER_FAILURES = 20
COMMAND_TIMEOUT_SECONDS = 5
DELIVERY_HISTORY_PER_APP = 100

SLUG_RE = re.compile(r"^[a-z][a-z0-9-]{1,30}[a-z0-9]$")
COMMAND_RE = re.compile(r"^[a-z][a-z0-9-]{1,31}$")
ACTION_ID_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,64}$")
PKCE_VERIFIER_RE = re.compile(r"^[A-Za-z0-9._~-]{43,128}$")
PKCE_CHALLENGE_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
STATE_MAX = 500
RESERVED_COMMANDS = frozenset({
    "issue", "topic", "me", "help", "invite", "leave", "join", "away",
    "status", "forkbot", "apps", "app"})
RESERVED_SLUGS = frozenset({
    "forkmesh", "forkbot", "admin", "system", "support", "security", "app",
    "apps", "bot", "official", "staff", "moderator"})

ICON_DATA_RE = re.compile(
    r"^data:image/(png|webp);base64,([A-Za-z0-9+/]+={0,2})$")
ICON_GLYPH_RE = re.compile(r"^.{1,2}$", re.DOTALL)
COLOR_RE = re.compile(r"^#[0-9a-fA-F]{6}$")


def clean_text(value, limit):
    text = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", "", str(value or ""))
    return re.sub(r"[ \t]+", " ", text).strip()[:int(limit)]


def clean_line(value, limit):
    return re.sub(r"\s+", " ", clean_text(value, limit * 2)).strip()[:limit]


def slug_from_name(name):
    slug = re.sub(r"[^a-z0-9]+", "-", str(name or "").lower()).strip("-")
    slug = re.sub(r"-{2,}", "-", slug)[:32].strip("-")
    if slug and not slug[0].isalpha():
        slug = ("app-" + slug)[:32].strip("-")
    return slug


def valid_slug(slug):
    return bool(SLUG_RE.fullmatch(str(slug or ""))) and \
        str(slug) not in RESERVED_SLUGS


def bot_display_name(name):
    return clean_line(name, MAX_NAME) + " (app)"


def bot_author(slug):
    return "app:" + str(slug)


def parse_scopes(value):
    if value is None:
        return ()
    if isinstance(value, str):
        items = re.split(r"[\s,]+", value.strip())
    elif isinstance(value, (list, tuple)):
        items = [str(item or "") for item in value]
    else:
        return None
    chosen = set()
    for item in items:
        item = item.strip().lower()
        if not item:
            continue
        if item not in SCOPE_LABELS:
            return None
        chosen.add(item)
    return tuple(scope for scope in SCOPES if scope in chosen)


def scope_string(scopes):
    return " ".join(scope for scope in SCOPES if scope in set(scopes or ()))


def has_scope(granted, needed):
    granted = set(granted or ())
    if isinstance(needed, str):
        return needed in granted
    return any(scope in granted for scope in needed)


def scope_view(scopes):
    hints = {key: hint for key, _label, hint in SCOPE_CATALOG}
    return [{"scope": scope, "label": SCOPE_LABELS[scope],
             "hint": hints[scope]} for scope in scopes]


def event_allowed(event_type, scopes):
    needed = EVENT_SCOPES.get(event_type)
    if needed is None:
        return False
    return not needed or has_scope(scopes, needed)


def parse_events(value):
    if isinstance(value, str):
        value = value.split()
    chosen = {str(item or "").strip().lower() for item in (value or [])}
    unknown = chosen - set(EVENT_TYPES)
    if unknown:
        return None
    return tuple(event for event in EVENT_TYPES if event in chosen)


def dev_origins(value):
    out = []
    for item in re.split(r"[\s,]+", str(value or "").strip()):
        try:
            parsed = urlparse(item)
        except ValueError:
            continue
        if parsed.scheme in ("http", "https") and parsed.hostname and \
                not parsed.path.strip("/") and not parsed.query:
            out.append(parsed.scheme + "://" + parsed.netloc.lower())
    return tuple(out)


def _dev_allowed(raw, allowed_origins):
    try:
        parsed = urlparse(raw)
    except ValueError:
        return False
    origin = parsed.scheme + "://" + parsed.netloc.lower()
    return bool(allowed_origins) and origin in allowed_origins and \
        parsed.username is None and not parsed.fragment


def clean_redirect_uri(value, allowed_dev=()):
    raw = str(value or "").strip()
    if not raw or len(raw) > MAX_URL or any(ch in raw for ch in " \t\r\n\\"):
        return ""
    if _dev_allowed(raw, allowed_dev):
        return raw
    try:
        parsed = urlparse(raw)
        host = str(parsed.hostname or "")
        parsed.port  # noqa: B018
    except ValueError:
        return ""
    if parsed.scheme != "https" or not host or parsed.username is not None \
            or parsed.password is not None or parsed.fragment or \
            "#" in raw or "." not in host.strip("[]") and ":" not in host:
        return ""
    return raw


def redirect_matches(registered, supplied):
    supplied = str(supplied or "")
    return any(hmac.compare_digest(str(uri), supplied) for uri in registered)


def clean_endpoint_url(value, allowed_dev=()):
    raw = str(value or "").strip()
    if not raw:
        return ""
    if _dev_allowed(raw, allowed_dev):
        return raw
    return _webhooks.clean_url(raw)


def with_query(url, params):
    joiner = "&" if "?" in url else "?"
    return url + joiner + urlencode(
        [(key, value) for key, value in params if value not in (None, "")])


def pkce_s256(verifier):
    digest = hashlib.sha256(str(verifier).encode("ascii")).digest()
    return base64.urlsafe_b64encode(digest).decode("ascii").rstrip("=")


def verify_pkce(verifier, challenge, method="S256"):
    if method != "S256":
        return False
    verifier = str(verifier or "")
    challenge = str(challenge or "")
    if not PKCE_VERIFIER_RE.fullmatch(verifier) or \
            not PKCE_CHALLENGE_RE.fullmatch(challenge):
        return False
    return hmac.compare_digest(pkce_s256(verifier), challenge)


def token_kind(token):
    token = str(token or "")
    if not SECRET_RE.fullmatch(token):
        return ""
    return {"fmat": "access", "fmrt": "refresh", "fmac": "code",
            "fmcs": "client_secret", "fmss": "signing_secret",
            "fmru": "response_url"}[token[:4]]


def secret_digest_input(kind, token):
    return "forkmesh-app-%s:%s" % (kind, token)


def clean_icon(value):
    if not isinstance(value, dict):
        value = {"glyph": str(value or "")} if value else {}
    data = str(value.get("data") or "").strip()
    if data:
        match = ICON_DATA_RE.fullmatch(data)
        if not match:
            return None
        try:
            raw = base64.b64decode(match.group(2), validate=True)
        except ValueError:
            return None
        if len(raw) > MAX_ICON_BYTES:
            return None
        png = raw.startswith(b"\x89PNG\r\n\x1a\n")
        webp = raw[:4] == b"RIFF" and raw[8:12] == b"WEBP"
        if (match.group(1) == "png" and not png) or \
                (match.group(1) == "webp" and not webp):
            return None
        return {"data": data}
    glyph = clean_text(value.get("glyph"), 8)
    color = str(value.get("color") or "#6366f1")
    if not COLOR_RE.fullmatch(color):
        return None
    if glyph and not ICON_GLYPH_RE.fullmatch(glyph):
        glyph = glyph[:2]
    return {"glyph": glyph, "color": color.lower()}


def clean_commands(value, allowed_dev=()):
    if value in (None, ""):
        return []
    if not isinstance(value, list) or len(value) > MAX_COMMANDS:
        return None
    seen = set()
    out = []
    for item in value:
        if not isinstance(item, dict):
            return None
        name = str(item.get("command") or "").strip().lower().lstrip("/")
        if not COMMAND_RE.fullmatch(name) or name in RESERVED_COMMANDS or \
                name in seen:
            return None
        url = clean_endpoint_url(item.get("url"), allowed_dev)
        if not url:
            return None
        seen.add(name)
        out.append({
            "command": name,
            "description": clean_line(item.get("description"), 120),
            "usageHint": clean_line(item.get("usageHint"), 80),
            "url": url,
        })
    return out


def parse_slash(text):
    match = re.match(r"^/([a-z][a-z0-9-]{1,31})(?:\s+([\s\S]*))?$",
                     str(text or "").strip(), re.IGNORECASE)
    if not match:
        return "", ""
    return match.group(1).lower(), clean_text(match.group(2) or "", 2000)


def _block_text(value, limit=MAX_BLOCK_TEXT):
    if isinstance(value, dict):
        value = value.get("text")
    text = clean_text(value, limit)
    return text


def clean_blocks(value):
    if value in (None, ""):
        return [], ()
    if not isinstance(value, list) or len(value) > MAX_BLOCKS:
        return None, "too_many_blocks"
    blocks = []
    actions = []
    for block in value:
        if not isinstance(block, dict):
            return None, "invalid_block"
        kind = str(block.get("type") or "")
        if kind == "divider":
            blocks.append({"type": "divider"})
            continue
        if kind in ("section", "context"):
            text = _block_text(block.get("text"))
            clean = {"type": kind, "text": text}
            fields = block.get("fields") or []
            if kind == "section" and fields:
                if not isinstance(fields, list) or len(fields) > MAX_FIELDS:
                    return None, "too_many_fields"
                clean["fields"] = [_block_text(item, 300) for item in fields]
            if not text and not clean.get("fields"):
                return None, "empty_block"
            blocks.append(clean)
            continue
        if kind == "actions":
            elements = block.get("elements")
            if not isinstance(elements, list) or not elements or \
                    len(elements) > MAX_ELEMENTS:
                return None, "invalid_actions"
            clean_elements = []
            for element in elements:
                if not isinstance(element, dict):
                    return None, "invalid_element"
                action_id = str(element.get("action_id") or "")
                if not ACTION_ID_RE.fullmatch(action_id) or \
                        action_id in actions:
                    return None, "invalid_action_id"
                etype = str(element.get("type") or "")
                if etype == "button":
                    label = _block_text(element.get("text"), 40)
                    if not label:
                        return None, "invalid_button"
                    style = str(element.get("style") or "")
                    clean_elements.append({
                        "type": "button", "action_id": action_id,
                        "text": label,
                        "value": clean_text(element.get("value"), 200),
                        "style": style if style in ("primary", "danger")
                        else "",
                    })
                elif etype == "static_select":
                    options = element.get("options")
                    if not isinstance(options, list) or not options or \
                            len(options) > MAX_OPTIONS:
                        return None, "invalid_select"
                    clean_options = []
                    for option in options:
                        if not isinstance(option, dict):
                            return None, "invalid_option"
                        label = _block_text(option.get("text"), 75)
                        option_value = clean_text(option.get("value"), 150)
                        if not label or not option_value:
                            return None, "invalid_option"
                        clean_options.append(
                            {"text": label, "value": option_value})
                    clean_elements.append({
                        "type": "static_select", "action_id": action_id,
                        "placeholder": _block_text(
                            element.get("placeholder"), 75) or "Choose",
                        "options": clean_options,
                    })
                else:
                    return None, "invalid_element"
                actions.append(action_id)
            blocks.append({"type": "actions", "elements": clean_elements})
            continue
        return None, "invalid_block"
    if len(json.dumps(blocks, separators=(",", ":")).encode()) > \
            MAX_BLOCKS_JSON:
        return None, "blocks_too_large"
    return blocks, tuple(actions)


def action_value_allowed(blocks, action_id, value):
    for block in blocks or []:
        for element in block.get("elements") or []:
            if element.get("action_id") != action_id:
                continue
            if element.get("type") == "button":
                return str(value or "") == str(element.get("value") or "")
            if element.get("type") == "static_select":
                return any(str(option.get("value")) == str(value or "")
                           for option in element.get("options") or [])
    return False


def blocks_plain_text(blocks):
    lines = []
    for block in blocks or []:
        if block.get("text"):
            lines.append(block["text"])
        for field in block.get("fields") or []:
            lines.append(field)
        for element in block.get("elements") or []:
            if element.get("type") == "button":
                lines.append("[" + element.get("text", "") + "]")
    return "\n".join(lines)[:MAX_MESSAGE_TEXT]


def canonical_body(payload):
    return _webhooks.canonical_body(payload)


def sign(secret, request_id, timestamp, body):
    return _webhooks.sign_body(secret, request_id, timestamp, body)


def request_headers(signature, request_id, timestamp, request_type,
                    attempt=1):
    headers = _webhooks.delivery_headers(
        signature, {"id": request_id, "type": request_type}, attempt,
        timestamp)
    headers["user-agent"] = "forkmesh-apps/1.0 (+https://forkmesh.com" \
        "/docs/apps)"
    return headers


def verify_signature(secret, request_id, timestamp, body, signature, now_s,
                     tolerance=TOLERANCE_SECONDS):
    try:
        timestamp = int(timestamp)
    except (TypeError, ValueError):
        return False
    if abs(int(now_s) - timestamp) > tolerance:
        return False
    expected = sign(secret, request_id, timestamp, body)
    return hmac.compare_digest(expected, str(signature or ""))


def classify(status):
    return _webhooks.classify(status)


def backoff_ms(attempt):
    return _webhooks.backoff_ms(attempt)


LISTING_FIELDS = ("name", "description", "icon", "scopes", "events",
                  "commands", "redirectUris", "eventUrl", "interactivityUrl",
                  "bot", "homepageUrl")


def content_hash(record):
    subset = {key: record.get(key) for key in LISTING_FIELDS}
    return hashlib.sha256(json.dumps(
        subset, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def public_app_view(row, record, rating=None):
    view = {
        "id": str(row.get("app_id") or ""),
        "clientId": str(row.get("client_id") or ""),
        "slug": str(row.get("slug") or ""),
        "name": str(record.get("name") or ""),
        "botName": bot_display_name(record.get("name")),
        "description": str(record.get("description") or ""),
        "icon": record.get("icon") or {},
        "scopes": list(record.get("scopes") or ()),
        "events": list(record.get("events") or ()),
        "commands": [
            {"command": item.get("command"),
             "description": item.get("description", ""),
             "usageHint": item.get("usageHint", "")}
            for item in record.get("commands") or []],
        "bot": bool((record.get("bot") or {}).get("enabled")),
        "developer": str(record.get("ownerName") or ""),
        "organization": str(record.get("orgName") or ""),
        "listing": str(row.get("listing") or "private"),
        "createdAt": int(row.get("created_at") or 0),
    }
    if rating is not None:
        view["rating"] = rating
    return view
