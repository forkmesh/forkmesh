import json
import re
from collections import OrderedDict
from urllib.parse import quote, urlencode

MAX_FORKS = 24
CACHE_ENTRIES = 1024
INTERNAL_ORIGIN = "https://forkmesh.internal"
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
SEGMENT_RE = re.compile(r"^[A-Za-z0-9_.-]{1,100}$")

_counts_cache = OrderedDict()


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def oid(value):
    text = str(value or "").strip().lower()
    return text if OID_RE.fullmatch(text) else ""


def repo_key(value):
    parts = str(value or "").strip().lower().split("/")
    if len(parts) != 2 or not all(
            SEGMENT_RE.fullmatch(part) and part not in (".", "..")
            for part in parts):
        return ""
    return parts[0] + "/" + parts[1]


def fork_keys(values):
    keys = []
    for value in values if isinstance(values, list) else []:
        key = repo_key(value)
        if key and key not in keys:
            keys.append(key)
        if len(keys) >= MAX_FORKS:
            break
    return keys


def _cache_get(key):
    if key not in _counts_cache:
        return None
    _counts_cache.move_to_end(key)
    return _counts_cache[key]


def _cache_put(key, value):
    _counts_cache[key] = value
    _counts_cache.move_to_end(key)
    while len(_counts_cache) > CACHE_ENTRIES:
        _counts_cache.popitem(last=False)


async def _read(env, owner, repo, operation, query=None):
    url = "%s/api/repo/%s/%s/%s" % (
        INTERNAL_ORIGIN, quote(owner, safe=""), quote(repo, safe=""),
        operation)
    if query:
        url += "?" + urlencode(query)
    try:
        response = await _routed_repository_read(
            env, url, operation, bypass_cache=False)
        status = int(getattr(response, "status", 0) or 0)
        text = str(await response.text())
        value = json.loads(text) if text else None
    except Exception:
        return 0, None
    return status, value if isinstance(value, dict) else None


async def _default_tip(env, owner, repo, tips):
    key = owner + "/" + repo
    if key in tips:
        return tips[key]
    tips[key] = ("", "")
    status, value = await _read(env, owner, repo, "branches")
    if status != 200 or not value:
        return tips[key]
    default = str(value.get("defaultBranch") or "").strip() or "main"
    for item in value.get("branches") or []:
        if isinstance(item, dict) and str(item.get("name") or "") == default:
            commit = oid(item.get("commit"))
            if commit:
                tips[key] = (default, commit)
            break
    return tips[key]


async def _public_record(env, owner, repo):
    node = ""
    try:
        node = str(await _org_repo_node(env, owner, repo) or "").lower()
    except Exception:
        node = ""
    node = node or owner
    try:
        if await _repo_is_private(env, node, repo):
            return node, None
        record = await _repo_catalog_record(env, node, repo)
    except Exception:
        return node, None
    if (not isinstance(record, dict) or record.get("isPrivate")
            or record.get("visibility") not in (None, "", "public")):
        return node, None
    return node, record


async def fork_count(env, key, tips):
    owner, repo = key.split("/")
    _node, record = await _public_record(env, owner, repo)
    parent = repo_key((record or {}).get("forkedFrom"))
    if not parent or parent == key:
        return None
    parent_owner, parent_repo = parent.split("/")
    parent_node, parent_record = await _public_record(
        env, parent_owner, parent_repo)
    if parent_record is None:
        return None
    branch, base = await _default_tip(env, parent_owner, parent_repo, tips)
    _fork_branch, head = await _default_tip(env, owner, repo, tips)
    if not base or not head:
        return None
    cache_key = (key, parent, base, head)
    cached = _cache_get(cache_key)
    if cached is not None:
        return cached
    status, value = await _read(env, owner, repo, "compare", {
        "base": base, "head": head,
        "cherry": parent_node + "/" + parent_repo, "counts": "1"})
    if status != 200 or not value or value.get("ok") is not True:
        return None
    try:
        ahead = int(value.get("ahead"))
        behind = int(value.get("behind"))
    except (TypeError, ValueError):
        return None
    if ahead < 0 or behind < 0:
        return None
    counts = {"ahead": ahead, "behind": behind, "parent": parent,
              "branch": branch}
    _cache_put(cache_key, counts)
    return counts


async def handler(env, request):
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    keys = fork_keys(data.get("forks") if isinstance(data, dict) else None)
    tips = {}
    counts = {}
    for key in keys:
        found = await fork_count(env, key, tips)
        if found is not None:
            counts[key] = found
    return json_response(
        {"ok": True, "counts": counts},
        cache_control="no-store, max-age=0")
