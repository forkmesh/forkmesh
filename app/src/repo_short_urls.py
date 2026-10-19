import json
import re
from urllib.parse import quote, unquote

from static_routes import (
    REPO_DOCUMENT_ROUTES,
    REPO_TAB_ROUTES,
    SHORT_NAME_RESERVED,
    repo_alias_redirect,
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
CLONE_KEYS = frozenset({"cloneUrl", "https", "remoteUrl"})
GIT_TAILS = (("info", "refs"), ("git-upload-pack",), ("git-receive-pack",))
DEFAULT = {"short": False, "owner": "", "canonical": False}
KEEP_CACHE_MS = 5 * 60 * 1000

_cache = {"at": -CACHE_MS, "value": None}
_keep_cache = {"at": -KEEP_CACHE_MS, "owner": "", "value": None}


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
    short = bool(value.get("short")) and bool(owner)
    return {"short": short, "owner": owner,
            "canonical": short and bool(value.get("canonical"))}


def clean(short, owner, canonical=False):
    owner = str(owner or "").strip().lower().lstrip("@")
    if owner and not OWNER_RE.fullmatch(owner):
        raise SettingError("The owner must be an account or organization name.")
    if short and not owner:
        raise SettingError("Choose the account or organization that owns the "
                           "short addresses.")
    return {"short": bool(short), "owner": owner,
            "canonical": bool(short) and bool(canonical)}


def dump(setting):
    return json.dumps({"short": bool(setting.get("short")),
                       "owner": str(setting.get("owner") or ""),
                       "canonical": bool(setting.get("short"))
                       and bool(setting.get("canonical"))},
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
    _keep_cache["value"] = None
    _keep_cache["at"] = -KEEP_CACHE_MS


def cached_keep(owner, now_ms):
    if (_keep_cache["value"] is not None and _keep_cache["owner"] == owner
            and now_ms - _keep_cache["at"] < KEEP_CACHE_MS):
        return _keep_cache["value"]
    return None


def remember_keep(owner, value, now_ms):
    _keep_cache.update(owner=owner, value=value, at=now_ms)
    return value


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
    if (lowered in SHORT_NAME_RESERVED or not NAME_RE.fullmatch(first)
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


def alias_redirect(path, query, owner):
    hit = rewrite(path, owner)
    if not hit or not hit[1]:
        return None
    target = repo_alias_redirect("/" + str(owner) + str(path), query)
    prefix = "/" + str(owner) + "/"
    if not target or not target.startswith(prefix):
        return None
    return target[len(prefix) - 1:]


def example(owner, host):
    owner = str(owner or "owner")
    return ("https://%s/<repository>/<folder>" % host,
            "https://%s/%s/<repository>/tree/<folder>" % (host, owner))


def canonical_owner(setting):
    if setting and setting.get("short") and setting.get("canonical"):
        return str(setting.get("owner") or "")
    return ""


def name_can_be_short(name, owner):
    name = str(name or "")
    hit = rewrite("/" + quote(name, safe=""), owner) if name else None
    return bool(hit and hit[1])


def owner_candidates(names, owner):
    return sorted({str(n).lower() for n in names
                   if name_can_be_short(n, owner)
                   and OWNER_RE.fullmatch(str(n).lower())})


def keep_names(names, owner, taken=()):
    taken = {str(t).lower() for t in taken}
    return sorted({str(n).lower() for n in names
                   if not name_can_be_short(n, owner)
                   or str(n).lower() in taken})


def canonical_redirect(path, query, setting):
    owner = canonical_owner(setting)
    parts = [p for p in str(path or "").split("/") if p]
    if not owner or len(parts) < 2 or unquote(parts[0]).lower() != owner:
        return None
    short = "/" + "/".join(parts[1:])
    hit = rewrite(short, owner)
    if not hit or not hit[1]:
        return None
    if hit[0] != "/%s/%s" % (owner, "/".join(parts[1:])):
        return None
    return short + ("?" + query if query else ""), hit[1]


def public_path(owner, name, setting, keep=()):
    owner = str(owner or "")
    name = str(name or "")
    if (canonical_owner(setting) == owner.lower() and owner
            and name_can_be_short(name, owner.lower())
            and name.lower() not in {str(k).lower() for k in keep}):
        return "/" + quote(name, safe="")
    return "/%s/%s" % (quote(owner, safe=""), quote(name, safe=""))


def links_doc(rows, owner, taken=()):
    owner = str(owner or "").lower()
    keep_all = keep_names(rows, owner, taken)
    public = [n for n, (_creator, private) in rows.items() if not private]
    links = {}
    aliases = {}
    for name, (creator, private) in rows.items():
        creator = str(creator or "").lower()
        if creator and creator != owner:
            links[str(name).lower()] = creator
            if not private:
                aliases[str(name).lower()] = creator
    return {"keep": [n for n in keep_all
                     if n in {str(p).lower() for p in public}],
            "allKeep": keep_all, "aliases": aliases, "links": links}


def client_doc(setting, links=None):
    owner = canonical_owner(setting)
    if not owner:
        return {"owner": str((setting or {}).get("owner") or ""),
                "short": bool((setting or {}).get("short")),
                "canonical": False}
    links = links or {}
    return {"owner": owner, "short": True, "canonical": True,
            "keep": sorted({str(k).lower() for k in links.get("keep") or ()}),
            "aliases": dict(sorted((links.get("aliases") or {}).items())),
            "reserved": sorted(SHORT_NAME_RESERVED)}


def _short_match(match, owner, links, clone):
    first, name, git, rest = match.group(1), match.group(2), \
        match.group(3) or "", match.group(4) or ""
    creator = (links.get("links") or {}).get(name.lower())
    if first.lower() != owner and creator != first.lower():
        return None
    if (not name_can_be_short(name, owner)
            or name.lower() in set(links.get("allKeep") or ())):
        return None
    if rest:
        segment = unquote(rest.split("/")[1]) if len(rest) > 1 else ""
        if git or segment not in ROUTED:
            return None
        return "/" + name + rest
    return "/" + name + (".git" if clone or git else "")


def short_urls(value, origin, setting, links, key=""):
    owner = canonical_owner(setting)
    if not owner or not origin or links is None:
        return value
    if isinstance(value, dict):
        return {k: short_urls(v, origin, setting, links, k)
                for k, v in value.items()}
    if isinstance(value, list):
        return [short_urls(v, origin, setting, links, key) for v in value]
    if not isinstance(value, str) or origin + "/" not in value:
        return value
    pattern = re.compile(
        re.escape(origin) + r"/([A-Za-z0-9][A-Za-z0-9_-]{0,63})/"
        r"([A-Za-z0-9_.-]+?)(\.git)?((?:/[^\s\"'?#]*)?)(?=$|[\s\"'?#])")
    clone = key in CLONE_KEYS or value.startswith("git ")

    def swap(match):
        short = _short_match(match, owner, links, clone)
        return origin + short if short else match.group(0)
    return pattern.sub(swap, value)
