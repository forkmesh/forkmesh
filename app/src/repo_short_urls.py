import json
import re
from urllib.parse import unquote

from static_routes import (
    REPO_DOCUMENT_ROUTES,
    REPO_TAB_ROUTES,
    RESERVED_ROUTE_PREFIXES,
)

SETTING_KEY = "repo_paths"
CACHE_MS = 30 * 1000
MAX_DOC_BYTES = 1024
NAME_RE = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$")
OWNER_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")
FILE_RE = re.compile(
    r"\.(?:php\d?|txt|xml|json|ico|png|jpe?g|gif|svg|js|mjs|css|map|env|"
    r"html?|aspx?|jsp|cgi|zip|gz|tgz|tar|sql|bak|ya?ml|ini|log|"
    r"webmanifest|woff2?|ttf|pdf)$", re.IGNORECASE)
ROUTED = frozenset(REPO_TAB_ROUTES | REPO_DOCUMENT_ROUTES)
GIT_TAILS = (("info", "refs"), ("git-upload-pack",), ("git-receive-pack",))
DEFAULT = {"short": False, "owner": ""}

_cache = {"at": -CACHE_MS, "value": None}


class SettingError(ValueError):
    pass


def parse(doc):
    try:
        value = json.loads(str(doc or "")[:MAX_DOC_BYTES] or "{}")
    except ValueError:
        return dict(DEFAULT)
    if not isinstance(value, dict):
        return dict(DEFAULT)
    owner = str(value.get("owner") or "").strip().lower()
    if not OWNER_RE.fullmatch(owner):
        owner = ""
    return {"short": bool(value.get("short")) and bool(owner), "owner": owner}


def clean(short, owner):
    owner = str(owner or "").strip().lower().lstrip("@")
    if owner and not OWNER_RE.fullmatch(owner):
        raise SettingError("The owner must be an account or organization name.")
    if short and not owner:
        raise SettingError("Choose the account or organization that owns the "
                           "short addresses.")
    return {"short": bool(short), "owner": owner}


def dump(setting):
    return json.dumps({"short": bool(setting.get("short")),
                       "owner": str(setting.get("owner") or "")},
                      sort_keys=True, separators=(",", ":"))


def cached(now_ms):
    if _cache["value"] is not None and now_ms - _cache["at"] < CACHE_MS:
        return _cache["value"]
    return None


def remember(value, now_ms):
    _cache["value"] = value
    _cache["at"] = now_ms
    return value


def forget():
    _cache["value"] = None
    _cache["at"] = -CACHE_MS


def first_name(path):
    parts = [p for p in str(path or "").split("/") if p]
    if not parts:
        return ""
    name = unquote(parts[0]).lower()
    return name[:-4] if name.endswith(".git") else name


def rewrite(path, owner):
    owner = str(owner or "").lower()
    if not OWNER_RE.fullmatch(owner):
        return None
    parts = [p for p in str(path or "").split("/") if p]
    if not parts:
        return None
    first = unquote(parts[0])
    lowered = first.lower()
    if (lowered in RESERVED_ROUTE_PREFIXES or not NAME_RE.fullmatch(first)
            or FILE_RE.search(first)):
        return None
    git_suffix = lowered.endswith(".git")
    name = first[:-4] if git_suffix else first
    if not name or name.startswith(".") or name.lower() == owner:
        return None
    rest = parts[1:]
    tail = tuple(unquote(p) for p in rest)
    if tail in GIT_TAILS:
        return "/%s/%s/%s" % (owner, parts[0], "/".join(rest)), ""
    if git_suffix:
        return None
    if not rest:
        return "/%s/%s" % (owner, parts[0]), name
    if tail[0] in ROUTED:
        return "/%s/%s/%s" % (owner, parts[0], "/".join(rest)), name
    return "/%s/%s/tree/%s" % (owner, parts[0], "/".join(rest)), name


def example(owner, host):
    owner = str(owner or "owner")
    return ("https://%s/<repository>/<folder>" % host,
            "https://%s/%s/<repository>/tree/<folder>" % (host, owner))
