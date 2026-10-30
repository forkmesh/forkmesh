import json
import re
import time
from collections import OrderedDict
from urllib.parse import quote, urlencode, urlparse

import remote_forks

MAX_ROWS = 100
MAX_RECORDS = 1000
CACHE_ENTRIES = 2048
INTERNAL_ORIGIN = "https://forkmesh.internal"
PLATFORM_REPOSITORY = "forkmesh"
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/+-]{1,255}$")

_cherry_cache = OrderedDict()


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def branch_name(value):
    name = str(value or "").strip()
    if name.startswith("refs/heads/"):
        name = name[len("refs/heads/"):]
    if not BRANCH_RE.fullmatch(name):
        return ""
    if (name[0] in "-/." or name.endswith("/") or name.endswith(".lock")
            or ".." in name or "//" in name or "@{" in name):
        return ""
    return name


def oid(value):
    text = str(value or "").strip().lower()
    return text if OID_RE.fullmatch(text) else ""


def pull_number(value):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return 0
    return number if 0 < number < 1000000000 else 0


def _page(url):
    parsed = urlparse(str(url or ""))
    host = str(parsed.hostname or "").lower()
    if host.startswith("app."):
        host = host[4:]
    path = parsed.path.rstrip("/")
    if path.lower().endswith(".git"):
        path = path[:-4]
    return host + path.lower()


def _cache_get(key):
    if key not in _cherry_cache:
        return None
    _cherry_cache.move_to_end(key)
    return _cherry_cache[key]


def _cache_put(key, value):
    _cherry_cache[key] = value
    _cherry_cache.move_to_end(key)
    while len(_cherry_cache) > CACHE_ENTRIES:
        _cherry_cache.popitem(last=False)


async def _read(env, owner, repo, operation, query=None, fresh=True):
    url = "%s/api/repo/%s/%s/%s" % (
        INTERNAL_ORIGIN, quote(owner, safe=""), quote(repo, safe=""),
        operation)
    if query:
        url += "?" + urlencode(query)
    try:
        response = await _routed_repository_read(
            env, url, operation, bypass_cache=fresh)
        status = int(getattr(response, "status", 0) or 0)
        text = str(await response.text())
        value = json.loads(text) if text else None
    except Exception:
        return 0, None
    return status, value if isinstance(value, dict) else None


async def _tips(env, owner, repo):
    status, value = await _read(env, owner, repo, "branches")
    if status != 200 or not value:
        return None, ""
    tips = {}
    for item in value.get("branches") or []:
        if not isinstance(item, dict):
            continue
        name = branch_name(item.get("name"))
        commit = oid(item.get("commit"))
        if name and commit:
            tips[name] = commit
    return tips, branch_name(value.get("defaultBranch"))


async def upstream_mirror(env, target):
    flagship = str(_instance_solo_flagship_node(env) or "").strip().lower()
    candidates = [(target["owner"].lower(), target["name"])]
    if flagship:
        candidates.append((flagship, PLATFORM_REPOSITORY))
    wanted = _page(target["url"])
    for owner, name in dict.fromkeys(candidates):
        try:
            fields = await _read_only_mirror_fields(env, owner, name)
        except Exception:
            continue
        if not fields.get("mirrorOf") or _page(fields["mirrorOf"]) != wanted:
            continue
        tips, default = await _tips(env, owner, name)
        commit = (tips or {}).get(default or "main", "")
        if commit:
            return {"repo": "%s/%s" % (owner, name), "branch": default or "main",
                    "commit": commit}
    return None


async def upstream_state(env, owner, repo, upstream, head, limit):
    key = (upstream["repo"], upstream["commit"], head, limit)
    cached = _cache_get(key)
    if cached is not None:
        return cached
    query = {"base": upstream["commit"], "head": head,
             "cherry": upstream["repo"]}
    if limit:
        query["limit"] = limit
    status, value = await _read(env, owner, repo, "compare", query,
                                fresh=False)
    if status != 200 or not value or value.get("ok") is not True:
        return "unknown"
    commits = value.get("commits")
    if not isinstance(commits, list):
        return "unknown"
    if not commits:
        state = "empty"
    elif any(not item.get("upstream") for item in commits
             if isinstance(item, dict)):
        state = "pending"
    else:
        state = "merged"
    _cache_put(key, state)
    return state


async def _sent_records(env, repo_bi, target_bi):
    rows = await d1_all(
        env,
        "SELECT branch, head, number, created_at FROM pull_batch_sent "
        "WHERE repo_bi=? AND target_bi=? ORDER BY created_at DESC LIMIT ?",
        repo_bi, target_bi, MAX_RECORDS)
    records = {}
    for row in rows or []:
        branch = branch_name(row.get("branch"))
        number = pull_number(row.get("number"))
        if branch and number:
            records.setdefault(branch, []).append({
                "head": oid(row.get("head")), "number": number,
                "at": int(row.get("created_at") or 0)})
    return records


def _limit(compare_base, tips):
    commit = oid(compare_base)
    if commit:
        return commit
    name = branch_name(compare_base)
    return (tips or {}).get(name, "") if name else ""


async def _status(env, owner, repo, target, repo_bi, target_bi, data):
    rows = data.get("rows")
    if not isinstance(rows, list):
        return json_response({"error": "invalid_rows"}, status=400)
    tips, _default = await _tips(env, owner, repo)
    if tips is None:
        return json_response(
            {"error": "mirror_unavailable",
             "message": "The branch list could not be read. Try again "
                        "shortly."},
            status=503, cache_control="no-store")
    records = await _sent_records(env, repo_bi, target_bi)
    upstream = await upstream_mirror(env, target)
    out = {}
    for item in rows[:MAX_ROWS]:
        if not isinstance(item, dict):
            continue
        branch = branch_name(item.get("branch"))
        if not branch or branch in out:
            continue
        head = tips.get(branch, "")
        if not head:
            out[branch] = {"state": "missing", "head": ""}
            continue
        sent = records.get(branch) or []
        current = next((record for record in sent if record["head"] == head),
                       None)
        upstream_check = "unchecked"
        if upstream:
            upstream_check = await upstream_state(
                env, owner, repo, upstream, head,
                _limit(item.get("compareBase"), tips))
        if upstream_check in ("merged", "empty"):
            state = "merged"
        elif current:
            state = "sent"
        else:
            state = "pending"
        out[branch] = {
            "state": state,
            "head": head,
            "upstream": upstream_check,
            "sent": current,
            "sentBefore": None if current else (sent[0] if sent else None),
        }
    return json_response(
        {"ok": True, "upstream": upstream, "rows": out},
        cache_control="no-store")


async def _record(env, repo_bi, target_bi, data):
    entries = data.get("sent")
    if not isinstance(entries, list) or not entries:
        return json_response({"error": "invalid_sent"}, status=400)
    now = int(time.time() * 1000)
    saved = 0
    for item in entries[:MAX_ROWS]:
        if not isinstance(item, dict):
            continue
        branch = branch_name(item.get("branch"))
        head = oid(item.get("head"))
        number = pull_number(item.get("number"))
        if not branch or not head or not number:
            continue
        await d1_run(
            env,
            "INSERT INTO pull_batch_sent "
            "(repo_bi, target_bi, branch, head, number, created_at) "
            "VALUES (?,?,?,?,?,?) ON CONFLICT(repo_bi, target_bi, branch, "
            "head) DO UPDATE SET number=excluded.number",
            repo_bi, target_bi, branch, head, number, now)
        saved += 1
    if not saved:
        return json_response({"error": "invalid_sent"}, status=400)
    return json_response(
        {"ok": True, "saved": saved,
         "sent": await _sent_records(env, repo_bi, target_bi)},
        cache_control="no-store")


async def handler(env, request, owner, repo, public_owner=""):
    await ensure_schema(env)
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    if await _repo_is_private(env, owner, repo):
        return json_response({"error": "not_found"}, status=404)
    try:
        data = await bounded_json_request(request)
    except Exception:
        data = {}
    if not isinstance(data, dict):
        data = {}
    if not await remote_forks._manager(env, request, owner, repo, data):
        return json_response({"error": "not_authorized"}, status=403)
    target = remote_forks.parse_url(data.get("target"))
    forks_bi = await remote_forks._repo_key(env, public_owner or owner, repo)
    saved = {fork["url"] for fork in await remote_forks.list_forks(
        env, forks_bi)}
    if not target or target["url"] not in saved:
        return json_response(
            {"error": "unknown_target",
             "message": "Save that fork on another instance first."},
            status=400)
    repo_bi = await blind_index(
        env, "%s/%s" % ((public_owner or owner).lower(), repo.lower()))
    target_bi = await blind_index(
        env, "pull-batch-target:" + target["url"].lower())
    action = str(data.get("action") or "")
    if action == "status":
        return await _status(
            env, owner, repo, target, repo_bi, target_bi, data)
    if action == "record":
        return await _record(env, repo_bi, target_bi, data)
    return json_response({"error": "invalid_action"}, status=400)
