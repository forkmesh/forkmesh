import hashlib
import re


TAG_MOVE_PREFIX = "forkmesh-tag-move-v1"
BRANCH_DELETE_PREFIX = "forkmesh-branch-delete-v1"
BRANCH_TRASH_PREFIX = "forkmesh-branch-trash-v1"
BRANCH_TRASH_CACHE_MS = 30 * 1000
BRANCH_TRASH_CACHE_MAX = 32
_BRANCH_TRASH_CACHE = {}
PROTECTED_BRANCHES = ("main", "master", "HEAD")
TAG_MOVE_TTL_MS = 15 * 60 * 1000
TAG_MOVE_RETENTION_MS = 30 * 24 * 60 * 60 * 1000
MAX_TAG_MOVES_PER_PUSH = 8
ACTIONS_PUSH_BRANCH_MAX = 5
ACTIONS_PUSH_CREATED_MAX = 3
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
TAG_REF_RE = re.compile(r"^refs/tags/[A-Za-z0-9._/-]{1,180}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/-]{1,200}$")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def normalize_tag_ref(value):
    text = str(value or "").strip()
    if not text.startswith("refs/tags/"):
        text = "refs/tags/" + text
    if (
        not TAG_REF_RE.fullmatch(text)
        or ".." in text
        or "//" in text
        or "@{" in text
        or text.endswith((".", "/", ".lock"))
        or "/." in text
    ):
        return ""
    return text


def normalize_deletable_branch(value):
    text = str(value or "").strip()
    if text.startswith("refs/heads/"):
        text = text[len("refs/heads/"):]
    if (
        not BRANCH_RE.fullmatch(text)
        or text in PROTECTED_BRANCHES
        or text.startswith(("forkmesh/", "/", "-"))
        or ".." in text
        or "//" in text
        or "@{" in text
        or text.endswith((".", "/", ".lock"))
        or "/." in text
    ):
        return ""
    return text


def branch_delete_message(owner, repo, branch, expect, ts):
    return "\n".join((
        BRANCH_DELETE_PREFIX, str(owner or "").strip().lower(),
        str(repo or "").strip(), str(branch or ""), str(expect or ""),
        str(ts),
    )).encode("utf-8")


def branch_trash_message(owner, repo, ts):
    return "\n".join((
        BRANCH_TRASH_PREFIX, str(owner or "").strip().lower(),
        str(repo or "").strip(), str(ts),
    )).encode("utf-8")


def tag_move_message(owner, repo, ref, target, ts):
    return "\n".join((
        TAG_MOVE_PREFIX, str(owner or "").strip().lower(),
        str(repo or "").strip(), str(ref or ""), str(target or ""), str(ts),
    )).encode("utf-8")


def updates_digest(updates):
    lines = sorted(
        "%s %s %s" % (item["ref"], item["old"], item["new"])
        for item in updates)
    return hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()


def _zero(oid):
    return bool(oid) and set(oid) == {"0"}


async def _repo_bi(env, owner, repo):
    return await blind_index(
        env, str(owner or "").strip().lower() + "/" + str(repo or "").strip())


async def pending_tag_moves(env, owner, repo, account):
    now = int(Date.now())
    try:
        rows = await d1_all(
            env,
            "SELECT ref, target FROM git_tag_moves WHERE repo_bi=? "
            "AND actor_bi=? AND used_at=0 AND expires_at>? "
            "ORDER BY created_at DESC, id DESC LIMIT ?",
            await _repo_bi(env, owner, repo),
            await blind_index(env, str(account or "").strip().lower()),
            now, MAX_TAG_MOVES_PER_PUSH * 4)
    except Exception:
        return {}
    moves = {}
    for row in rows or []:
        ref = normalize_tag_ref(row.get("ref"))
        target = str(row.get("target") or "").strip().lower()
        if not ref or not OID_RE.fullmatch(target) or ref in moves:
            continue
        moves[ref] = target
        if len(moves) >= MAX_TAG_MOVES_PER_PUSH:
            break
    return moves


async def _default_branch(env, owner, repo):
    try:
        row = await d1_first(
            env, "SELECT data FROM repositories WHERE key_bi=?",
            await _repo_bi(env, owner, repo))
        record = await decrypt_row(env, row.get("data")) if row else None
    except Exception:
        record = None
    branch = clean_string(
        (record or {}).get("branch", "") if isinstance(record, dict) else "",
        120).strip()
    return branch or "main"


async def _consume_tag_move(env, owner, repo, account, update):
    await d1_run(
        env,
        "UPDATE git_tag_moves SET used_at=?, used_from=? WHERE repo_bi=? "
        "AND actor_bi=? AND ref=? AND target=? AND used_at=0",
        int(Date.now()), update["old"], await _repo_bi(env, owner, repo),
        await blind_index(env, account), update["ref"], update["new"])
    await _audit_sensitive_action(
        env, account, "git.tag_move", "repository", owner + "/" + repo,
        "success",
        {"ref": update["ref"], "old": update["old"], "new": update["new"]})


async def _fire_push_hooks(env, owner, repo, account, updates, state):
    default = await _default_branch(env, owner, repo)
    default_ref = "refs/heads/" + default
    head = next((item for item in updates if item["ref"] == default_ref), None)
    head_moved = bool(head) and not _zero(head["new"])
    digest = updates_digest(updates)
    now = int(Date.now())
    refs = [
        {"ref": item["ref"], "before": item["old"], "after": item["new"]}
        for item in updates
    ]
    await queue_repo_webhook_event(
        env, owner, repo, "repository.push",
        {
            "defaultBranch": default,
            "before": head["old"] if head else "",
            "after": head["new"] if head else "",
            "refsDigest": state,
            "defaultBranchMoved": head_moved,
            "pusher": account,
            "refs": refs,
        },
        digest, occurred_at=now)
    if head_moved:
        await queue_repo_webhook_event(
            env, owner, repo, "branch.push",
            {
                "ref": default_ref,
                "branch": default,
                "before": "" if _zero(head["old"]) else head["old"],
                "after": head["new"],
                "refsDigest": state,
                "pusher": account,
            },
            head["new"], occurred_at=now)
    await notify_code_push(
        env, owner, repo,
        branch=default if head_moved else "",
        commit=head["new"] if head_moved else "",
        commits=head.get("commits", 0) if head_moved else 0,
        pusher=account,
        key=head["new"] if head_moved else digest,
        refs_digest=state,
        pusher_account=account)


async def _desktop_online(env, owner, repo, pool):
    names = [owner]
    node = await _org_repo_node(env, owner, repo)
    if node and node not in names:
        names.append(node)
    for name in names:
        if name not in pool and await _node_events_object_holds_node(env, name):
            return True
    return False


def _instance_actions(env):
    return bool(str(getattr(env, "ACTIONS_INSTANCE_RUNNER", "") or "").strip())


async def queue_runner_actions(env, owner, repo, commit, node="", pusher="",
                               branch="", before=""):
    try:
        owner = str(owner or "").strip().lower()
        repo = str(repo or "").strip()
        commit = str(commit or "").strip().lower()
        node = str(node or "").strip().lower()
        if not OID_RE.fullmatch(commit) or _zero(commit):
            return 0
        import runner_actions
        import runner_leases
        runner_leases._bind_runtime(globals())
        runtime = runner_leases.WorkerRuntime(env, None)
        pool = runtime.pool_targets()
        if node not in pool:
            return 0
        instance = _instance_actions(env)
        if not instance and not await runner_actions.owner_has_runner(
                runtime, owner):
            return 0
        if not instance and await _desktop_online(env, owner, repo, pool):
            return 0
        branch = branch or await _default_branch(env, owner, repo)
        return await runner_actions.enqueue(
            runtime, owner, repo, branch, commit, node, pusher,
            before=before)
    except Exception:
        return 0


def _created(item):
    old = str(item.get("old") or "")
    return not old or _zero(old)


def instance_action_updates(updates, default):
    default_ref = "refs/heads/" + default
    heads = [item for item in updates
             if str(item.get("ref") or "").startswith("refs/heads/")
             and not _zero(item.get("new"))]
    head = next((item for item in heads if item["ref"] == default_ref), None)
    others = [item for item in heads if item is not head]
    created = [item for item in others if _created(item)]
    if (head is not None and _created(head)) or (
            len(created) > ACTIONS_PUSH_CREATED_MAX):
        others = [item for item in others if not _created(item)]
    return ([head] if head is not None else []) + (
        others[:ACTIONS_PUSH_BRANCH_MAX])


async def _queue_pushed_actions(env, owner, repo, node, account, updates):
    if _instance_actions(env):
        default = await _default_branch(env, owner, repo)
        for item in instance_action_updates(updates, default):
            await queue_runner_actions(
                env, owner, repo, item["new"], node, account,
                item["ref"][len("refs/heads/"):], item.get("old") or "")
        return
    default = await _default_branch(env, owner, repo)
    head = next((item for item in updates
                 if item["ref"] == "refs/heads/" + default), None)
    if head and not _zero(head["new"]):
        await queue_runner_actions(env, owner, repo, head["new"], node,
                                   account, default)


async def record_accepted_push(env, owner, repo, endpoint, account, report,
                               core=False, state="", tag_moves=None,
                               credential="", contributor=False,
                               alias=""):
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    account = str(account or "").strip().lower()
    updates = list((report or {}).get("updates") or [])
    total = int((report or {}).get("total") or len(updates))
    node = str((endpoint or {}).get("node") or "")
    target = owner + "/" + repo
    for update in updates:
        details = {
            "ref": update["ref"], "old": update["old"], "new": update["new"],
            "node": node, "credential": credential,
        }
        if update["ref"].startswith("refs/heads/"):
            details["commits"] = int(update.get("commits") or 0)
        await _audit_sensitive_action(
            env, account, "git.push", "repository", target, "success",
            details)
    if total > len(updates):
        await _audit_sensitive_action(
            env, account, "git.push", "repository", target, "success",
            {"node": node, "refsNotListed": total - len(updates)})
    for update in updates:
        if (
            (tag_moves or {}).get(update["ref"]) == update["new"]
            and not _zero(update["old"])
        ):
            try:
                await _consume_tag_move(env, owner, repo, account, update)
            except Exception:
                pass
    if account and updates:
        try:
            await recent_push_remember(
                env, owner, repo, account, updates, alias=alias)
        except Exception:
            pass
    if core and updates:
        try:
            await _fire_push_hooks(env, owner, repo, account, updates, state)
        except Exception:
            pass
        try:
            if (report or {}).get("actions") is not True and not contributor:
                await _queue_pushed_actions(env, owner, repo, node, account,
                                            updates)
        except Exception:
            pass
    return len(updates)


def _header(headers, name, maximum):
    try:
        value = headers.get(name)
    except Exception:
        return ""
    if js_nullish(value) or value is None:
        return ""
    return clean_string(str(value), maximum).strip()


async def _signed_account(env, request, message_for):
    headers = getattr(request, "headers", None)
    if headers is None:
        return None
    pubkey = _header(headers, "x-forkmesh-pubkey", 120)
    ts = _header(headers, "x-forkmesh-ts", 20)
    sig = _header(headers, "x-forkmesh-sig", 200)
    if not (pubkey or ts or sig):
        return None
    if not valid_node_pubkey(pubkey) or not ts.isdigit():
        return ""
    if abs(int(Date.now()) - int(ts)) > LOGIN_MAX_SKEW_MS:
        return ""
    if not await ed25519_verify(pubkey, sig, message_for(ts)):
        return ""
    return await _git_push_account_for_pubkey(env, pubkey)


async def tag_moves_handler(env, request):
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    owner = clean_string(data.get("owner", ""), MAX_NODE_NAME).strip().lower()
    repo = safe_segment(clean_string(data.get("repo", ""), 100).strip())
    ref = normalize_tag_ref(data.get("ref", ""))
    target = clean_string(data.get("target", ""), 64).strip().lower()
    if (
        not valid_node_name(owner) or not repo or not ref
        or not OID_RE.fullmatch(target) or _zero(target)
    ):
        return json_response({"error": "invalid_request"}, status=400)
    account = await _signed_account(
        env, request,
        lambda ts: tag_move_message(owner, repo, ref, target, ts))
    if account is None:
        account = await _authed_account_name(env, request, data)
    if not account:
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control="no-store")
    granted, problem = await grant_tag_move(
        env, account, owner, repo, ref, target)
    if problem is not None:
        return problem
    return json_response(
        {"ok": True, "ref": ref, "target": target, "expiresAt": granted},
        cache_control="no-store")


async def grant_tag_move(env, account, owner, repo, ref, target):
    await ensure_schema(env)
    node = await _org_repo_node(env, owner, repo) or owner
    audit_target = node + "/" + repo
    if node not in _core_push_nodes(env):
        return 0, json_response(
            {"error": "tag_moves_unavailable",
             "message": "Tags move only on repositories the core nodes "
                        "hold."},
            status=409, cache_control="no-store")
    if not await _core_push_permitted(env, node, repo, account):
        await _audit_sensitive_action(
            env, account, "git.tag_move_authorize", "repository",
            audit_target, "denied", {"ref": ref, "target": target})
        return 0, json_response(
            {"error": "forbidden"}, status=403, cache_control="no-store")
    now = int(Date.now())
    expires_at = now + TAG_MOVE_TTL_MS
    await d1_run(
        env, "DELETE FROM git_tag_moves WHERE created_at<?",
        now - TAG_MOVE_RETENTION_MS)
    await d1_run(
        env,
        "INSERT INTO git_tag_moves (repo_bi, actor_bi, ref, target, "
        "created_at, expires_at) VALUES (?,?,?,?,?,?)",
        await _repo_bi(env, node, repo), await blind_index(env, account),
        ref, target, now, expires_at)
    await _audit_sensitive_action(
        env, account, "git.tag_move_authorize", "repository", audit_target,
        "success", {"ref": ref, "target": target, "expiresAt": expires_at})
    return expires_at, None


async def branch_deletes_handler(env, request):
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    owner = clean_string(data.get("owner", ""), MAX_NODE_NAME).strip().lower()
    repo = safe_segment(clean_string(data.get("repo", ""), 100).strip())
    branch = normalize_deletable_branch(
        clean_string(data.get("branch", ""), 220))
    expect = clean_string(data.get("expect", ""), 64).strip().lower()
    if (
        not valid_node_name(owner) or not repo or not branch
        or not OID_RE.fullmatch(expect) or _zero(expect)
    ):
        return json_response({"error": "invalid_request"}, status=400)
    account = await _signed_account(
        env, request,
        lambda ts: branch_delete_message(owner, repo, branch, expect, ts))
    if account is None:
        account = await _authed_account_name(env, request, data)
    if not account:
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control="no-store")
    await ensure_schema(env)
    node = await _org_repo_node(env, owner, repo) or owner
    audit_target = node + "/" + repo
    if node not in _core_push_nodes(env):
        return json_response(
            {"error": "branch_deletes_unavailable",
             "message": "Branches are deleted this way only on repositories "
                        "the core nodes hold."},
            status=409, cache_control="no-store")
    if not await _core_push_permitted(env, node, repo, account):
        await _audit_sensitive_action(
            env, account, "repository.branch_delete", "repository",
            audit_target, "denied", {"branch": branch, "expect": expect})
        return json_response(
            {"error": "forbidden"}, status=403, cache_control="no-store")
    record = {
        "op": "delete", "branch": branch, "expectedOid": expect,
        "requestedBy": account, "queuedAt": int(Date.now()),
        "source": "source",
    }
    resume = {name: data[name] for name in ("requestId", "node", "pinBase")
              if isinstance(data.get(name), str)}
    response = await _route_handlers._branch_op_on_cores(
        env, request, node, repo, await _repo_bi(env, node, repo),
        audit_target, record, resume, "", account)
    if response is None:
        return json_response(
            {"error": "cores_unavailable", "retryable": True,
             "message": "No core answered; the branch was not deleted."},
            status=503, cache_control="no-store",
            extra_headers=EXPECTED_DEGRADED_HEADERS)
    return response


async def branch_trash_handler(env, request):
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    owner = clean_string(data.get("owner", ""), MAX_NODE_NAME).strip().lower()
    repo = safe_segment(clean_string(data.get("repo", ""), 100).strip())
    if not valid_node_name(owner) or not repo:
        return json_response({"error": "invalid_request"}, status=400)
    account = await _signed_account(
        env, request, lambda ts: branch_trash_message(owner, repo, ts))
    if account is None:
        account = await _authed_account_name(env, request, data)
    if not account:
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control="no-store")
    await ensure_schema(env)
    node = await _org_repo_node(env, owner, repo) or owner
    audit_target = node + "/" + repo
    if node not in _core_push_nodes(env):
        return json_response(
            {"error": "branch_trash_unavailable",
             "message": "Deleted branches are listed only for repositories "
                        "the core nodes hold."},
            status=409, cache_control="no-store")
    if not await _core_push_permitted(env, node, repo, account):
        await _audit_sensitive_action(
            env, account, "repository.branch_trash_read", "repository",
            audit_target, "denied", {})
        return json_response(
            {"error": "forbidden"}, status=403, cache_control="no-store")
    now = int(Date.now())
    key = (node, repo.lower())
    cached = _BRANCH_TRASH_CACHE.get(key)
    if cached is None or now - cached[0] > BRANCH_TRASH_CACHE_MS:
        listing = await _branch_trash_on_cores(env, request, node, repo)
        if listing is None:
            return json_response(
                {"error": "cores_unavailable", "retryable": True,
                 "message": "No core answered; nothing is known deleted."},
                status=503, cache_control="no-store",
                extra_headers=EXPECTED_DEGRADED_HEADERS)
        if len(_BRANCH_TRASH_CACHE) >= BRANCH_TRASH_CACHE_MAX:
            _BRANCH_TRASH_CACHE.clear()
        cached = (now, listing)
        _BRANCH_TRASH_CACHE[key] = cached
    listing = cached[1]
    return json_response(
        {"ok": True, "owner": owner, "repo": repo,
         "entries": listing["entries"], "truncated": listing["truncated"],
         "cores": listing["cores"], "unanswered": listing["unanswered"],
         "listedAt": cached[0]},
        cache_control="no-store")
