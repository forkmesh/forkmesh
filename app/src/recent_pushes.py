import re
from urllib.parse import quote

DEFAULT_WINDOW_MINUTES = 60
MAX_WINDOW_MINUTES = 24 * 60
RETENTION_MS = 24 * 60 * 60 * 1000
MAX_BRANCHES_PER_PUSH = 10
MAX_LISTED = 10
MAX_ID = 200
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/+-]{1,200}$")
SKIPPED_PREFIXES = ("forkmesh/",)


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def branch_name(ref):
    text = str(ref or "")
    if not text.startswith("refs/heads/"):
        return ""
    name = text[len("refs/heads/"):]
    if (not BRANCH_RE.fullmatch(name) or name[0] in "-/."
            or name.endswith(("/", ".lock")) or ".." in name or "//" in name
            or "@{" in name or name.startswith(SKIPPED_PREFIXES)):
        return ""
    return name


def _zero(oid):
    return bool(oid) and set(oid) == {"0"}


def _oid(value):
    text = str(value or "").strip().lower()
    return text if OID_RE.fullmatch(text) else ""


def window_ms(env):
    try:
        minutes = int(str(getattr(env, "RECENT_PUSH_WINDOW_MINUTES", "")
                          or DEFAULT_WINDOW_MINUTES).strip())
    except (TypeError, ValueError):
        minutes = DEFAULT_WINDOW_MINUTES
    return max(1, min(minutes, MAX_WINDOW_MINUTES)) * 60 * 1000


def _segment(value):
    return quote(str(value or ""), safe="")


def _ref_path(name):
    return quote(str(name or ""), safe="/")


async def _branch_key(env, branch):
    return await blind_index(env, "recent-push-branch:" + branch)


async def pull_key(env, target_repo_bi, head):
    return await blind_index(
        env, "recent-push-pull:" + str(target_repo_bi or "") + ":"
        + str(head or "").strip())


async def _public_path(env, owner, repo):
    try:
        path = await _repo_public_path(env, owner, repo)
    except Exception:
        path = ""
    return str(path or "/%s/%s" % (_segment(owner), _segment(repo)))


async def _target(env, owner, repo, public_owner, record):
    default = str((record or {}).get("branch") or "").strip() or "main"
    parent = str((record or {}).get("forkedFrom") or "").strip().lower()
    if parent.count("/") == 1 and parent != (owner + "/" + repo).lower():
        parent_owner, parent_repo = parent.split("/")
        parent_record = None
        read_only = False
        try:
            parent_record = await _repo_catalog_record(
                env, parent_owner, parent_repo)
            fields = await _read_only_mirror_fields(
                env, parent_owner, parent_repo, record=parent_record)
            read_only = bool((fields or {}).get("readOnly"))
        except Exception:
            parent_record = None
        if isinstance(parent_record, dict) and not read_only:
            base = str(parent_record.get("branch") or "").strip() or default
            return {
                "owner": parent_owner, "repo": parent_repo,
                "base": base, "fork": True,
                "head": public_owner + ":",
                "default": default,
            }
    return {"owner": owner, "repo": repo, "base": default, "fork": False,
            "head": "", "default": default}


async def remember(env, owner, repo, account, updates, alias=""):
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    account = str(account or "").strip().lower()
    if not owner or not repo or not account:
        return 0
    moved = []
    deleted = []
    for update in list(updates or [])[:MAX_BRANCHES_PER_PUSH * 4]:
        branch = branch_name((update or {}).get("ref"))
        if not branch:
            continue
        new = str((update or {}).get("new") or "").lower()
        if _zero(new):
            deleted.append(branch)
        elif _oid(new):
            moved.append((branch, new))
    if not moved and not deleted:
        return 0
    now = int(Date.now())
    repo_bi = await blind_index(env, owner + "/" + repo)
    for branch in deleted:
        await d1_run(
            env, "DELETE FROM recent_pushes WHERE repo_bi=? AND branch_bi=?",
            repo_bi, await _branch_key(env, branch))
    recorded = 0
    if moved:
        record = await _repo_catalog_record(env, owner, repo)
        public_owner = str(alias or owner).strip().lower()
        target = await _target(env, owner, repo, public_owner, record)
        if target["fork"]:
            target_bi = await blind_index(
                env, target["owner"] + "/" + target["repo"])
        else:
            target_bi = repo_bi
        path = await _public_path(env, public_owner, repo)
        target_path = path if not target["fork"] else await _public_path(
            env, target["owner"], target["repo"])
        account_bi = await blind_index(env, account)
        for branch, new in moved[:MAX_BRANCHES_PER_PUSH]:
            if branch == target["default"]:
                continue
            data = {
                "repo": public_owner + "/" + repo, "path": path,
                "branch": branch, "head": new, "base": target["base"],
                "fork": target["fork"],
                "target": target["owner"] + "/" + target["repo"],
                "targetPath": target_path,
            }
            await d1_run(
                env,
                "INSERT INTO recent_pushes (account_bi, repo_bi, branch_bi, "
                "pull_bi, pushed_at, pulled, data) VALUES (?,?,?,?,?,0,?) "
                "ON CONFLICT(account_bi, repo_bi, branch_bi) DO UPDATE SET "
                "pull_bi=excluded.pull_bi, pushed_at=excluded.pushed_at, "
                "data=excluded.data",
                account_bi, repo_bi, await _branch_key(env, branch),
                await pull_key(env, target_bi, target["head"] + branch),
                now, await encrypt_row(env, data))
            recorded += 1
    await d1_run(env, "DELETE FROM recent_pushes WHERE pushed_at<?",
                 now - RETENTION_MS)
    return recorded


async def forget_branches(env, repo_bi, branches):
    for branch in list(branches or [])[:MAX_BRANCHES_PER_PUSH * 10]:
        name = branch_name("refs/heads/" + str(branch or ""))
        if name:
            await d1_run(
                env,
                "DELETE FROM recent_pushes WHERE repo_bi=? AND branch_bi=?",
                repo_bi, await _branch_key(env, name))


async def mark_pulled(env, target_repo_bi, head):
    head = str(head or "").strip()
    if not target_repo_bi or not head:
        return
    await d1_run(env, "UPDATE recent_pushes SET pulled=1 WHERE pull_bi=?",
                 await pull_key(env, target_repo_bi, head))


def _item(row, data):
    path = str(data.get("path") or "")
    branch = str(data.get("branch") or "")
    base = str(data.get("base") or "main")
    if not path.startswith("/") or path.startswith("//") or not branch:
        return None
    return {
        "id": str(row.get("repo_bi") or "") + ":" + str(
            row.get("branch_bi") or ""),
        "repo": str(data.get("repo") or ""),
        "repoUrl": path,
        "branch": branch,
        "branchUrl": path + "/tree/" + _ref_path(branch),
        "head": _oid(data.get("head")),
        "pushedAt": int(row.get("pushed_at") or 0),
        "base": base,
        "fork": bool(data.get("fork")),
        "target": str(data.get("target") or ""),
        "targetUrl": str(data.get("targetPath") or path),
        "compareUrl": path + "/compare/" + _ref_path(base) + "..."
        + _ref_path(branch),
    }


async def list_for(env, account):
    account_bi = await blind_index(env, account)
    since = int(Date.now()) - window_ms(env)
    rows = await d1_all(
        env,
        "SELECT repo_bi, branch_bi, pushed_at, data FROM recent_pushes "
        "WHERE account_bi=? AND pushed_at>=? AND pulled=0 "
        "ORDER BY pushed_at DESC LIMIT ?",
        account_bi, since, MAX_LISTED)
    items = []
    for row in rows or []:
        try:
            data = await decrypt_row(env, row.get("data"))
        except Exception:
            data = None
        item = _item(row, data) if isinstance(data, dict) else None
        if item:
            items.append(item)
    return items


async def handler(env, request):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response({"error": "method_not_allowed"}, status=405)
    data = None
    if method == "POST":
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400)
    account = await _authed_account_name(env, request, data)
    if not account:
        if method == "GET":
            return json_response(
                {"ok": True, "signedIn": False, "pushes": []},
                cache_control="private, no-store")
        return json_response({"error": "unauthorized"}, status=401)
    if method == "POST":
        ident = str(data.get("id") or "")[:MAX_ID]
        repo_bi, _, branch_bi = ident.partition(":")
        if not re.fullmatch(r"[0-9a-f]{16,128}", repo_bi) or not re.fullmatch(
                r"[0-9a-f]{16,128}", branch_bi):
            return json_response({"error": "invalid_id"}, status=400)
        await d1_run(
            env, "UPDATE recent_pushes SET pulled=1 WHERE account_bi=? AND "
            "repo_bi=? AND branch_bi=?",
            await blind_index(env, account), repo_bi, branch_bi)
        return json_response({"ok": True}, cache_control="no-store")
    return json_response(
        {"ok": True, "signedIn": True,
         "windowMinutes": window_ms(env) // 60000,
         "pushes": await list_for(env, account)},
        cache_control="private, no-store")
