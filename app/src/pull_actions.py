import base64
import json
import re
from urllib.parse import quote, urlencode


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


INTERNAL_ORIGIN = "https://forkmesh.internal"
PULLS_REF = "forkmesh/pulls"
SYNC_PULLS = 20
SYNC_BRANCH_READS = 8
SYNC_MEMO_MS = 15 * 1000
SYNC_REPOS = 20
TRUSTED_LIMIT = 200
BODY_MAX_BYTES = 4 * 1024
ACCOUNT_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
FRONT_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]{0,63}):[ \t]?(.*)$")
_SYNC_MEMO = {}


def _runtime(env):
    import runner_leases
    runner_leases._bind_runtime(globals())
    return runner_leases.WorkerRuntime(env, None)


def _instance(env):
    return bool(str(getattr(env, "ACTIONS_INSTANCE_RUNNER", "") or "").strip())


def _account(value):
    name = str(value or "").strip().lstrip("@").lower()
    return name if ACCOUNT_RE.fullmatch(name) else ""


def parse_pull_record(text):
    lines = str(text or "").splitlines()
    if not lines or lines[0].strip() != "---":
        return {}
    values = {}
    for line in lines[1:]:
        if line.strip() == "---":
            return values
        match = FRONT_RE.match(line)
        if match:
            values[match.group(1)] = match.group(2).strip()
    return {}


def _blob_text(blob):
    if not isinstance(blob, dict) or not blob.get("ok"):
        return ""
    content = blob.get("content", "")
    if not isinstance(content, str):
        return ""
    if blob.get("encoding") == "base64":
        try:
            return base64.b64decode(content).decode("utf-8", "replace")
        except Exception:
            return ""
    return content


async def _read(env, owner, repo, operation, query):
    url = "%s/api/repo/%s/%s/%s?%s" % (
        INTERNAL_ORIGIN, quote(owner, safe=""), quote(repo, safe=""),
        operation, urlencode(query, doseq=True))
    try:
        response = await _routed_repository_read(
            env, url, operation, bypass_cache=True)
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        value = json.loads(str(await response.text()))
    except Exception:
        return None
    return value if isinstance(value, dict) else None


async def _ref_commit(env, owner, repo, ref):
    tree = await _read(env, owner, repo, "tree", {"ref": ref})
    commit = str((tree or {}).get("commit") or "").lower()
    return commit if OID_RE.fullmatch(commit) else ""


async def open_pulls(env, owner, repo):
    tree = await _read(env, owner, repo, "tree",
                       {"path": ".forkmesh/pulls", "ref": PULLS_REF})
    if not tree:
        return []
    commit = str(tree.get("commit") or "").lower()
    refs = tree.get("pullRefs") if isinstance(
        tree.get("pullRefs"), dict) else {}
    numbers = {int(entry["name"]) for entry in tree.get("entries") or []
               if isinstance(entry, dict) and entry.get("type") == "tree"
               and str(entry.get("name") or "").isdigit()}
    numbers.update(int(name) for name in refs if str(name).isdigit()
                   and OID_RE.fullmatch(str(refs[name] or "").lower()))
    numbers = sorted(n for n in numbers if 0 < n < 10 ** 9)[::-1][:SYNC_PULLS]
    path = ".forkmesh/pulls/%d/pull.md"
    blobs = {}
    indexed = [path % n for n in numbers if str(n) not in refs]
    if indexed and OID_RE.fullmatch(commit):
        found = await _read(env, owner, repo, "blobs",
                            {"path": indexed, "ref": commit})
        blobs.update((found or {}).get("blobs") or {})
    for number in [n for n in numbers if str(n) in refs][:SYNC_BRANCH_READS]:
        found = await _read(env, owner, repo, "blobs", {
            "path": [path % number], "ref": str(refs[str(number)]).lower()})
        blobs.update((found or {}).get("blobs") or {})
    pulls = []
    for number in numbers:
        record = parse_pull_record(_blob_text(blobs.get(path % number)))
        if record.get("status", "open") != "open" or not record:
            continue
        pulls.append({**record, "number": number})
    return pulls


async def _pull_head(env, owner, repo, pull):
    import runner_actions
    head = str(pull.get("headOid") or "").lower()
    if OID_RE.fullmatch(head):
        return head, "refs/pr/%d/head" % pull["number"]
    branch = str(pull.get("head") or "")
    if pull.get("derive") == "branch" and runner_actions.valid_branch(branch):
        head = await _ref_commit(env, owner, repo, branch)
        if head:
            return head, "refs/heads/" + branch
    return "", ""


async def _author_account(env, pull):
    resolve = globals().get("_git_push_account_for_pubkey")
    key = str(pull.get("author") or "")
    if not callable(resolve) or not key:
        return ""
    try:
        return _account(await resolve(env, key))
    except Exception:
        return ""


async def is_writer(env, storage, repo, account):
    account = _account(account)
    if not account:
        return False
    try:
        return bool(
            account == storage
            or await _account_owns_node(env, account, storage)
            or await _org_write_allowed(env, storage, repo, account))
    except Exception:
        return False


async def author_trust(env, storage, repo, account):
    account = _account(account)
    if not account:
        return ""
    if await is_writer(env, storage, repo, account):
        return "writer"
    row = await d1_first(
        env, "SELECT account FROM repo_action_trusted_authors WHERE owner=? "
        "AND repo=? AND account=?", storage, str(repo).lower(), account)
    return "allowlist" if row else ""


async def trusted_authors(env, storage, repo):
    rows = await d1_all(
        env, "SELECT account,added_by,added_at FROM "
        "repo_action_trusted_authors WHERE owner=? AND repo=? "
        "ORDER BY account LIMIT %d" % TRUSTED_LIMIT,
        storage, str(repo).lower())
    return [{"account": str(row.get("account") or ""),
             "addedBy": str(row.get("added_by") or ""),
             "addedAt": int(row.get("added_at") or 0)} for row in rows or []]


async def sync_repo(env, owner, repo, force=False):
    import runner_actions
    runtime = _runtime(env)
    storage, public = await runner_actions.identity(runtime, owner, repo)
    key = storage + "/" + str(repo).lower()
    now = int(runtime.now())
    if not force and now - _SYNC_MEMO.get(key, 0) < SYNC_MEMO_MS:
        return 0
    if len(_SYNC_MEMO) > 512:
        _SYNC_MEMO.clear()
    _SYNC_MEMO[key] = now
    if not await runner_actions.pull_workflows(runtime, storage, repo):
        return 0
    queued = 0
    bases = {}
    for pull in await open_pulls(env, public, repo):
        head, head_ref = await _pull_head(env, public, repo, pull)
        base = str(pull.get("base") or "")
        if not head or not runner_actions.valid_branch(base):
            continue
        if base not in bases:
            bases[base] = await _ref_commit(env, public, repo, base)
        account = await _author_account(env, pull)
        trust = await author_trust(env, storage, repo, account)
        queued += len(await runner_actions.enqueue_pull(
            runtime, storage, repo, pull["number"], head, head_ref, base,
            bases[base], account, "" if trust else "author",
            pull.get("title") or ""))
    return queued


async def sync_all(env):
    rows = await d1_all(
        env, "SELECT DISTINCT owner,repo FROM repo_action_workflows "
        "WHERE events LIKE '%\"pull_request\"%' ORDER BY owner,repo "
        "LIMIT ?", SYNC_REPOS)
    queued = 0
    for row in rows or []:
        try:
            queued += await sync_repo(
                env, str(row.get("owner") or ""), str(row.get("repo") or ""),
                force=True)
        except Exception:
            continue
    return queued


async def _viewer(env, request, storage, repo, data=None):
    _, record = await _account_session_record(env, request, data or {})
    actor = _account((record or {}).get("name", ""))
    return actor, await is_writer(env, storage, repo, actor)


def _error(error, status, message=""):
    body = {"error": error}
    if message:
        body["message"] = message
    return json_response(body, status=status, cache_control="no-store")


def _current(jobs):
    head = jobs[0]["commit"] if jobs else ""
    return head, [job for job in jobs if job["commit"] == head]


async def pull_checks(env, runtime, storage, repo, number, actor, writer):
    import runner_actions
    jobs = await runner_actions.pull_runs(runtime, storage, repo, number)
    head, current = _current(jobs)
    waiting = [job for job in current if job["status"] == "waiting"]
    reason = "workflows" if any(
        job["approval"] == "workflows" for job in waiting) else (
            "author" if waiting else "")
    author = next((runner_actions._line(run.get("actor"), 64)
                   for job in current for run in job["runs"]
                   if run.get("actor")), "")
    runs = []
    for job in current:
        for run in job["runs"]:
            runs.append({key: run[key] for key in (
                "id", "workflow", "workflowPath", "status", "commit",
                "createdAt", "startedAt", "finishedAt") if key in run})
    approved_by = next((job["approvedBy"] for job in current
                        if job["approvedBy"]), "")
    configured = bool(await runner_actions.pull_workflows(
        runtime, storage, repo))
    return {
        "ok": True,
        "number": int(number),
        "configured": configured,
        "head": head,
        "runs": runs,
        "approval": {
            "required": bool(waiting),
            "reason": reason,
            "author": author,
            "authorTrust": await author_trust(env, storage, repo, author),
            "approvedBy": approved_by,
        },
        "canApprove": bool(writer),
        "viewer": actor,
        "previousHeads": len({job["commit"] for job in jobs}) - (
            1 if head else 0),
    }


async def _body(request):
    try:
        raw = str(await request.text())
    except Exception:
        return None
    if len(raw.encode("utf-8")) > BODY_MAX_BYTES:
        return None
    try:
        data = json.loads(raw)
    except ValueError:
        return None
    return data if isinstance(data, dict) else None


async def repo_pull_checks_handler(env, request, owner, repo, number):
    method = method_name(request)
    if method not in ("GET", "POST"):
        return _error("method_not_allowed", 405)
    if not _instance(env):
        return _error("not_available", 404)
    import runner_actions
    await ensure_schema(env)
    runtime = _runtime(env)
    storage, _public = await runner_actions.identity(runtime, owner, repo)
    data = await _body(request) if method == "POST" else {}
    if data is None:
        return _error("invalid_request", 400)
    actor, writer = await _viewer(env, request, storage, repo, data)
    if await _repo_is_private(env, storage, repo) and not writer:
        return _error("not_found", 404)
    if method == "GET":
        try:
            await sync_repo(env, owner, repo)
        except Exception:
            pass
        return json_response(
            await pull_checks(env, runtime, storage, repo, number, actor,
                              writer),
            cache_control="private, no-store, max-age=0")
    target = "%s/%s#%d" % (storage, repo, int(number))
    action = str(data.get("action") or "")
    if action not in ("approve", "trust", "untrust"):
        return _error("invalid_request", 400)
    if not actor:
        return _error("invalid_session", 401)
    if not writer:
        await _audit_sensitive_action(
            env, actor, "actions.pull_" + action, "repository", target,
            "denied", {})
        return _error("forbidden", 403)
    jobs = await runner_actions.pull_runs(runtime, storage, repo, number)
    head, current = _current(jobs)
    author = next((runner_actions._line(run.get("actor"), 64)
                   for job in current for run in job["runs"]
                   if run.get("actor")), "")
    if action == "approve":
        wanted = str(data.get("head") or "").lower()
        if not OID_RE.fullmatch(wanted) or wanted != head:
            return _error("stale_head", 409,
                          "The pull request has a newer commit. Review it "
                          "before approving.")
        changed = await runner_actions.approve_pull(
            runtime, storage, repo, number, head, actor)
        await _audit_sensitive_action(
            env, actor, "actions.pull_approve", "repository", target,
            "success" if changed else "failure",
            {"head": head, "jobs": changed})
        if not changed:
            return _error("not_awaiting_approval", 409)
        return json_response({"ok": True, "approved": changed},
                             cache_control="no-store")
    account = _account(data.get("account"))
    if not account or account != _account(author):
        return _error("author_unknown", 409,
                      "Only the pull request's verified author can be "
                      "trusted from here.")
    if action == "trust":
        await d1_run(
            env, "INSERT INTO repo_action_trusted_authors (owner,repo,"
            "account,added_by,added_at) VALUES (?,?,?,?,?) "
            "ON CONFLICT(owner,repo,account) DO NOTHING",
            storage, str(repo).lower(), account, actor, int(runtime.now()))
        changed = await runner_actions.approve_pull(
            runtime, storage, repo, number, head, actor) if head else 0
        await _audit_sensitive_action(
            env, actor, "actions.trust_author", "repository", target,
            "success", {"account": account, "head": head, "jobs": changed})
        return json_response({"ok": True, "trusted": account,
                              "approved": changed}, cache_control="no-store")
    await d1_run(
        env, "DELETE FROM repo_action_trusted_authors WHERE owner=? "
        "AND repo=? AND account=?", storage, str(repo).lower(), account)
    await _audit_sensitive_action(
        env, actor, "actions.untrust_author", "repository", target,
        "success", {"account": account})
    return json_response({"ok": True, "untrusted": account},
                         cache_control="no-store")
