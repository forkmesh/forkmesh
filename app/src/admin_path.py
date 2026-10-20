import re
import secrets
from urllib.parse import quote

import instance_features
from static_routes import RESERVED_ROUTE_PREFIXES

LEGACY_BASE = "/admin"
SUBPAGES = ("theme", "acquisition", "hackathon")
SEGMENT_RE = re.compile(r"^[A-Za-z0-9_-]{8,64}$")
EXTRA_RESERVED = frozenset({
    "admin", "developers", "product-analysis", "transparency",
    "pitch-deck", "simulate-sentry-error", "instance-theme", "nodeinfo",
    "well-known", "forkmesh-mirror", "forkmesh", "health", "readyz",
    "favicon", "static", "robots", "sitemap", "security",
})
RESERVED = frozenset(name.lower() for name in RESERVED_ROUTE_PREFIXES) \
    | EXTRA_RESERVED
ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789"
GENERATED_PREFIX = "/admin-"
GENERATED_LENGTH = 20
GENERATED_RE = re.compile(r"^/admin-[a-z0-9]{20}$")


def generate(random_bytes=secrets.token_bytes):
    chars = []
    limit = 256 - 256 % len(ALPHABET)
    while len(chars) < GENERATED_LENGTH:
        for byte in random_bytes(32):
            if byte < limit and len(chars) < GENERATED_LENGTH:
                chars.append(ALPHABET[byte % len(ALPHABET)])
    return GENERATED_PREFIX + "".join(chars)


def problem(value):
    text = str(value or "")
    if not text.startswith("/"):
        return "must start with /"
    segment = text[1:]
    if "/" in segment:
        return "must be a single path segment"
    if not SEGMENT_RE.fullmatch(segment):
        return "must be 8 to 64 letters, digits, - or _"
    lowered = segment.lower()
    if lowered in RESERVED:
        return "collides with a built-in route"
    return ""


def configured(env):
    return str(getattr(env, "ADMIN_PATH", "") or "").strip().strip("/")


def private_segment(env):
    if not instance_features.branded(env):
        return ""
    segment = configured(env)
    if not segment or problem("/" + segment):
        return ""
    return segment


def private(env):
    return bool(private_segment(env))


def console_base(env):
    segment = configured(env)
    return ("/" + quote(segment, safe="/-_.~")) if segment else ""


def sub_base(env):
    segment = private_segment(env)
    return ("/" + segment) if segment else LEGACY_BASE


def sub_path(env, name):
    return sub_base(env) + "/" + name


def notification_href(env, query=""):
    segment = private_segment(env)
    return ("/" + segment if segment else LEGACY_BASE) + query


def subpage(env, path):
    segment = private_segment(env)
    if not segment:
        return ""
    prefix = "/" + segment + "/"
    if not str(path or "").startswith(prefix):
        return ""
    rest = path[len(prefix):].rstrip("/")
    return rest if rest in SUBPAGES else ""


def is_console(env, path):
    segment = private_segment(env)
    if not segment:
        return False
    path = str(path or "")
    return path.strip("/") == segment or bool(subpage(env, path))


def legacy_hidden(env, path):
    if not private(env):
        return False
    path = str(path or "")
    return path == LEGACY_BASE or path.startswith(LEGACY_BASE + "/")
