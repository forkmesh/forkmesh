import json
import re
import time
from urllib.parse import parse_qs, urlparse

MAX_FORKS = 20
MAX_URL_LENGTH = 300
MAX_LABEL_LENGTH = 60
NAME_RE = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$")
HOST_RE = re.compile(
    r"^(?=.{1,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+"
    r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def parse_url(text):
    raw = str(text or "").strip()
    if not raw or len(raw) > MAX_URL_LENGTH or any(c.isspace() for c in raw):
        return None
    if "://" not in raw:
        raw = "https://" + raw
    try:
        parsed = urlparse(raw)
        port = parsed.port
    except ValueError:
        return None
    if parsed.scheme.lower() != "https" or parsed.username or parsed.password:
        return None
    host = str(parsed.hostname or "").lower().rstrip(".")
    if not HOST_RE.fullmatch(host):
        return None
    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) < 2:
        return None
    owner, name = parts[0], parts[1]
    if name.lower().endswith(".git"):
        name = name[:-4]
    if not NAME_RE.fullmatch(owner) or not NAME_RE.fullmatch(name):
        return None
    netloc = host if port in (None, 443) else "%s:%d" % (host, port)
    origin = "https://" + netloc
    return {
        "url": "%s/%s/%s" % (origin, owner, name),
        "origin": origin,
        "host": netloc,
        "owner": owner,
        "name": name,
    }


def clean_label(value):
    return " ".join(str(value or "").split())[:MAX_LABEL_LENGTH]


def _origin_of(value):
    try:
        parsed = urlparse(str(value or ""))
    except ValueError:
        return ""
    if not parsed.scheme or not parsed.netloc:
        return ""
    return (parsed.scheme + "://" + parsed.netloc).lower()


def _same_instance(env, request, origin):
    own = {_origin_of(getattr(request, "url", "")),
           _origin_of(getattr(env, "APP_ORIGIN", "")),
           _origin_of(getattr(env, "PUBLIC_BASE_URL", ""))}
    own.discard("")
    return origin.lower() in own


def _entry(row):
    try:
        data = json.loads(row.get("data") or "{}")
    except (TypeError, ValueError):
        return None
    parsed = parse_url(data.get("url"))
    if not parsed:
        return None
    parsed["label"] = clean_label(data.get("label")) or parsed["host"]
    parsed["addedBy"] = str(data.get("addedBy") or "")[:64]
    parsed["createdAt"] = int(row.get("created_at") or 0)
    return parsed


async def _repo_key(env, owner, repo):
    return await blind_index(
        env, "remote-forks:%s/%s" % (owner.lower(), repo.lower()))


async def _url_key(env, url):
    return await blind_index(env, "remote-fork-url:" + url.lower())


async def list_forks(env, repo_bi):
    rows = await d1_all(
        env,
        "SELECT data, created_at FROM repo_remote_forks "
        "WHERE repo_bi=? ORDER BY created_at ASC, rowid ASC LIMIT ?",
        repo_bi, MAX_FORKS)
    return [entry for entry in (_entry(row) for row in rows or []) if entry]


async def _manager(env, request, owner, repo, data):
    actor = await _authed_account_name(env, request, data)
    if not actor:
        return ""
    if await _account_owns_node(env, actor, owner):
        return actor
    if await _org_owner_of_linked_repo(env, owner, repo, actor):
        return actor
    return ""


async def handler(env, request, owner, repo, public_owner=""):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST", "DELETE"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, POST, DELETE"})
    if await _repo_is_private(env, owner, repo):
        return json_response({"error": "not_found"}, status=404)
    repo_bi = await _repo_key(env, public_owner or owner, repo)
    if method == "GET":
        manager = await _manager(env, request, owner, repo, {})
        return json_response(
            {"ok": True, "forks": await list_forks(env, repo_bi),
             "canManage": bool(manager), "max": MAX_FORKS},
            cache_control="no-store")
    try:
        data = await bounded_json_request(request)
    except Exception:
        data = {}
    if not isinstance(data, dict):
        data = {}
    if method == "DELETE" and not data.get("url"):
        query = parse_qs(urlparse(str(request.url)).query)
        data["url"] = (query.get("url") or [""])[0]
    manager = await _manager(env, request, owner, repo, data)
    if not manager:
        return json_response({"error": "not_authorized"}, status=403)
    parsed = parse_url(data.get("url"))
    if not parsed:
        return json_response(
            {"error": "invalid_url",
             "message": "Enter the https address of a repository, such as "
                        "https://forkmesh.com/owner/repository."},
            status=400)
    url_bi = await _url_key(env, parsed["url"])
    if method == "DELETE":
        await d1_run(
            env,
            "DELETE FROM repo_remote_forks WHERE repo_bi=? AND url_bi=?",
            repo_bi, url_bi)
        return json_response(
            {"ok": True, "forks": await list_forks(env, repo_bi)},
            cache_control="no-store")
    if _same_instance(env, request, parsed["origin"]):
        return json_response(
            {"error": "same_instance",
             "message": "That repository is on this instance. Use Fork for "
                        "a copy here."},
            status=400)
    existing = await d1_first(
        env,
        "SELECT created_at FROM repo_remote_forks "
        "WHERE repo_bi=? AND url_bi=?",
        repo_bi, url_bi)
    if not existing:
        count = await d1_first(
            env,
            "SELECT COUNT(*) AS c FROM repo_remote_forks WHERE repo_bi=?",
            repo_bi)
        if count and int(count.get("c") or 0) >= MAX_FORKS:
            return json_response(
                {"error": "too_many_forks",
                 "message": "A repository can list up to %d forks on other "
                            "instances." % MAX_FORKS},
                status=409)
    created_at = int((existing or {}).get("created_at") or 0) or int(
        time.time() * 1000)
    record = json.dumps({
        "url": parsed["url"],
        "label": clean_label(data.get("label")),
        "addedBy": manager,
    }, separators=(",", ":"))
    await d1_run(
        env,
        "INSERT INTO repo_remote_forks (repo_bi, url_bi, created_at, data) "
        "VALUES (?,?,?,?) ON CONFLICT(repo_bi, url_bi) DO UPDATE SET "
        "data=excluded.data",
        repo_bi, url_bi, created_at, record)
    return json_response(
        {"ok": True, "fork": parsed, "forks": await list_forks(env, repo_bi)},
        status=201, cache_control="no-store")
