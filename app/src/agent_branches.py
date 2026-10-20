import hashlib
import json
import re
from urllib.parse import parse_qs, quote, urlencode, urlparse


AGENT_PREFIX = "agent/"
INTERNAL_ORIGIN = "https://forkmesh.internal"
DIFF_PAGE_DEFAULT = 25
DIFF_PAGE_MAX = 100
MAX_DIFF_FILES = 3000
ACTIVE_STATUSES = ("queued", "running", "security_pending")
MERGE_OPERATION = "merge-agent-branch"
DELETE_OPERATION = "delete-agent-branch"
BRANCH_SLUG_MAX = 40
BRANCH_SCAN_LIMIT = 400
PENDING_MERGE_MS = 10 * 60 * 1000
MAX_PULL_NUMBER = 10_000_000
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{12,80}$")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
NUMBERED_BRANCH_RE = re.compile(r"^agent/([0-9]{1,7})-")
DIFF_HEADER_RE = re.compile(r'^diff --git "?a/(.*?)"? "?b/(.*?)"?$')


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def valid_branch(value):
    branch = str(value or "").strip()
    if (
        not BRANCH_RE.fullmatch(branch)
        or branch.startswith((".", "-", "/"))
        or branch.endswith((".", "/", ".lock"))
        or ".." in branch
        or "//" in branch
        or "@{" in branch
        or "\\" in branch
    ):
        return ""
    return branch


def valid_agent_branch(value):
    branch = valid_branch(value)
    if not branch.startswith(AGENT_PREFIX) or branch == AGENT_PREFIX:
        return ""
    return branch


def valid_base(value):
    base = valid_branch(value)
    if base.startswith(AGENT_PREFIX) or base.startswith("forkmesh/pull"):
        return ""
    return base


def branch_slug(value):
    text = re.sub(r"[^a-z0-9]+", "-", str(value or "").lower()).strip("-")
    words = [word for word in text.split("-") if word][:6]
    slug = "-".join(words)[:BRANCH_SLUG_MAX].strip("-")
    if slug.startswith("agent-"):
        slug = slug[len("agent-"):]
    return slug


def _info(record):
    info = record.get("agentInfo") if isinstance(record, dict) else None
    return info if isinstance(info, dict) else {}


def session_branch(record):
    return (
        valid_branch(_info(record).get("branchName"))
        or valid_branch((record or {}).get("branch"))
    )


def session_base(record):
    return (
        valid_base(_info(record).get("baseBranch"))
        or valid_base((record or {}).get("baseBranch"))
    )


def session_runner(row, record):
    record = record if isinstance(record, dict) else {}
    runner = str(record.get("runnerNode") or "").strip().lower()
    target = str((row or {}).get("target_node") or "").strip().lower()
    if not runner:
        return ""
    if record.get("route") == "runner_pool" or record.get("runnerPin") or (
            runner != target):
        return runner
    return ""


def runner_job_owner(record):
    record = record if isinstance(record, dict) else {}
    if record.get("route") != "runner_pool" and not record.get("runnerOwner"):
        return {}
    owner = str(record.get("runnerOwner") or record.get("createdBy")
                or "").strip().lower()
    return {"organization": owner} if owner else {}


def session_home(row, record):
    record = record if isinstance(record, dict) else {}
    if session_runner(row, record):
        return ""
    return str(record.get("runnerNode") or (row or {}).get("target_node")
               or "").strip().lower()


def _int(value, maximum=2 ** 31):
    try:
        number = int(value or 0)
    except (TypeError, ValueError, OverflowError):
        return 0
    return max(0, min(number, maximum))


def diff_stats_projection(value):
    value = value if isinstance(value, dict) else {}
    return {
        "files": _int(value.get("files")),
        "added": _int(value.get("added")),
        "removed": _int(value.get("removed")),
        "ahead": _int(value.get("ahead")),
        "behind": _int(value.get("behind")),
        "conflicted": value.get("conflicted") is True,
        "known": value.get("known") is True,
    }


def merge_projection(record):
    merge = (record or {}).get("merge")
    merge = merge if isinstance(merge, dict) else {}
    state = str(merge.get("state") or "")
    if state not in ("", "merging", "merged", "conflict", "failed"):
        state = ""
    if not state and _info(record).get("merged") is True:
        state = "merged"
    pending = merge.get("cleanupPending")
    conflicts = merge.get("conflicts")
    return {
        "state": state,
        "base": str(merge.get("base") or ""),
        "mergeOid": str(merge.get("mergeOid") or ""),
        "mergedAt": _int(merge.get("mergedAt") or _info(record).get("mergedAt"),
                         2 ** 53 - 1),
        "branchDeleted": merge.get("branchDeleted") is True,
        "cleanupPending": [
            str(item) for item in (pending if isinstance(pending, list) else [])
        ][:8],
        "conflicts": [
            str(item)[:300]
            for item in (conflicts if isinstance(conflicts, list) else [])
        ][:50],
        "message": str(merge.get("message") or "")[:300],
    }


def projection_extras(row, record):
    record = record if isinstance(record, dict) else {}
    branch = session_branch(record)
    merge = merge_projection(record)
    status = str((row or {}).get("status") or "")
    pr_number = session_pull_number(record)
    return {
        "branch": branch,
        "baseBranch": session_base(record),
        "runner": session_runner(row, record),
        "diffStats": diff_stats_projection(record.get("diffStats")),
        "merge": merge,
        "canMerge": bool(
            branch.startswith(AGENT_PREFIX)
            and status not in ACTIVE_STATUSES
            and merge["state"] != "merged"
        ),
        "prNumber": pr_number,
        "canOpenPull": bool(
            branch.startswith(AGENT_PREFIX)
            and not pr_number
            and status not in ACTIVE_STATUSES
            and merge["state"] not in ("merging", "merged")
            and not published_nothing(row, record)
        ),
    }


def session_pull_number(record):
    record = record if isinstance(record, dict) else {}
    pull = record.get("pull")
    linked = _int((pull if isinstance(pull, dict) else {}).get("number"),
                  MAX_PULL_NUMBER)
    return linked or _int(_info(record).get("prNumber"), MAX_PULL_NUMBER)


def parse_patch_files(patch):
    files = []
    current = None
    in_hunk = False
    for line in str(patch or "").split("\n"):
        header = DIFF_HEADER_RE.match(line)
        if header:
            current = {
                "path": header.group(2), "oldPath": header.group(1),
                "status": "modified", "added": 0, "removed": 0,
                "binary": False, "lines": [line],
            }
            files.append(current)
            in_hunk = False
            continue
        if current is None:
            continue
        current["lines"].append(line)
        if line.startswith("@@"):
            in_hunk = True
        elif not in_hunk:
            if line.startswith("new file mode"):
                current["status"] = "added"
            elif line.startswith("deleted file mode"):
                current["status"] = "deleted"
                current["path"] = current["oldPath"]
            elif line.startswith("rename from "):
                current["status"] = "renamed"
            elif line.startswith("Binary files") or line == "GIT binary patch":
                current["binary"] = True
        elif line.startswith("+"):
            current["added"] += 1
        elif line.startswith("-"):
            current["removed"] += 1
        elif line == "GIT binary patch":
            current["binary"] = True
    output = []
    for item in files[:MAX_DIFF_FILES]:
        lines = item.pop("lines")
        while lines and lines[-1] == "":
            lines.pop()
        item["patch"] = "\n".join(lines) + "\n"
        output.append(item)
    return output


def _page_args(request):
    try:
        query = urlparse(str(request.url)).query
        params = dict(
            (key, values[0]) for key, values in parse_qs(query).items())
    except Exception:
        params = {}
    page = _int(params.get("page"), 100_000)
    size = _int(params.get("pageSize")) or DIFF_PAGE_DEFAULT
    return page, max(1, min(size, DIFF_PAGE_MAX))


def _derived_request_id(base, node):
    digest = hashlib.sha256(str(node).encode("utf-8")).hexdigest()[:12]
    return (str(base)[:60] + "-" + digest)[:80]


def _new_request_id():
    return "ab-" + hashlib.sha256(_random_bytes(24)).hexdigest()[:40]


async def _served_owner(env, owner, repo):
    owner = str(owner or "").strip().lower()
    try:
        rows = await d1_all(
            env,
            "SELECT o.name AS name FROM org_repos r JOIN orgs o "
            "ON o.org_bi=r.org_bi WHERE r.node_owner=? AND r.repo=? LIMIT 2",
            owner, str(repo or "").strip().lower())
    except Exception:
        return owner
    names = {str(row.get("name") or "").strip().lower() for row in rows or []}
    if len(names) == 1:
        name = names.pop()
        if valid_node_name(name):
            return name
    return owner


async def _read_repo(env, owner, repo, operation, query=None):
    url = "%s/api/repo/%s/%s/%s" % (
        INTERNAL_ORIGIN, quote(owner, safe=""), quote(repo, safe=""),
        operation)
    if query:
        url += "?" + urlencode(query)
    try:
        response = await _routed_repository_read(
            env, url, operation, bypass_cache=True)
        status = int(getattr(response, "status", 0) or 0)
        text = str(await response.text())
        value = json.loads(text) if text else None
    except Exception:
        return 0, None
    return status, value if isinstance(value, dict) else None


async def _branch_tips(env, owner, repo):
    status, value = await _read_repo(env, owner, repo, "branches")
    if status != 200 or not value:
        return None, ""
    tips = {}
    for item in value.get("branches") or []:
        if not isinstance(item, dict):
            continue
        name = git_branch_name(item.get("name"))
        oid = str(item.get("commit") or "").strip().lower()
        if name and OID_RE.fullmatch(oid):
            tips[name] = oid
    return tips, valid_base(value.get("defaultBranch"))


async def _mirror_context(env, owner, repo):
    served = owner
    try:
        node = await _org_repo_node(env, owner, repo)
    except Exception:
        node = None
    if node:
        served = str(node).strip().lower()
    try:
        context = await _https_mirror_public_context(env, served, repo)
    except _HttpsMirrorContextUnavailable:
        return None, "mirror_unavailable"
    alias = str(owner or "").strip().lower()
    if context and node and alias != served:
        context = dict(context)
        context["routeOwner"] = alias
    return context, ""


async def _save(env, row, record, touch=False):
    now = int(Date.now())
    if touch:
        await d1_run(
            env,
            "UPDATE org_agent_sessions SET data=?,updated_at=? "
            "WHERE session_id=? AND org_bi=?",
            await encrypt_row(env, record), now, row["session_id"],
            row["org_bi"],
        )
    else:
        await d1_run(
            env,
            "UPDATE org_agent_sessions SET data=? "
            "WHERE session_id=? AND org_bi=?",
            await encrypt_row(env, record), row["session_id"], row["org_bi"],
        )


def _error(code, message, status, **kwargs):
    return _org_agent_error(code, message, status, **kwargs)


async def _load(env, request, owner, repo, session_id, owner_scope,
                data=None, own_session=True):
    signed_account = None
    if owner_scope and _route_handlers._owner_agent_signed_triple(request):
        signed_account = await _route_handlers._owner_agent_signed_session(
            env, request)
    context, error = await _org_agent_member_context(
        env, request, owner, repo, data or {}, owner_scope=owner_scope,
        signed_account=signed_account, own_session=own_session)
    if error:
        return None, None, None, error
    row, record = await _org_agent_session_row(
        env, context, clean_string(session_id, 64))
    if not row:
        return None, None, None, _error(
            "not_found",
            "This agent session was not found in the selected repository.",
            404)
    row = dict(row)
    row.setdefault("org_bi", context["orgBi"])
    return context, row, record, None


def published_nothing(row, record):
    record = record if isinstance(record, dict) else {}
    if str((row or {}).get("status") or "") in ACTIVE_STATUSES:
        return False
    info = _info(record)
    if "publish branch" in str(info.get("lastError") or ""):
        return False
    if not info:
        return True
    stats = diff_stats_projection(record.get("diffStats"))
    return stats["known"] and stats["files"] == 0 and stats["ahead"] == 0


def _missing_branch_message(row, record, branch):
    if str((row or {}).get("status") or "") in ACTIVE_STATUSES:
        return (branch + " is not on ForkMesh yet. Its changes show here "
                "once the runner pushes them.")
    error = str(_info(record).get("lastError") or "")
    if "publish branch" in error:
        return (branch + " never reached ForkMesh: the runner could not "
                "push it (" + error[:200] + ").")
    return (branch + " is not on ForkMesh yet. It appears once the runner "
            "has pushed it.")


def _stats_from_files(files, commits):
    return {
        "files": len(files),
        "added": sum(item["added"] for item in files),
        "removed": sum(item["removed"] for item in files),
        "commits": commits,
    }


async def diff_handler(env, request, owner, repo, session_id,
                       owner_scope=False):
    if method_name(request) != "GET":
        return _error("method_not_allowed", "Use GET to read the diff.", 405)
    context, row, record, error = await _load(
        env, request, owner, repo, session_id, owner_scope)
    if error:
        return error
    branch = session_branch(record)
    if not branch:
        return _error(
            "no_branch",
            "This run has not pushed a branch yet, so there is nothing to "
            "compare.", 409, required_action="wait_for_agent")
    owner = await _served_owner(env, owner, repo)
    base = session_base(record)
    if not base:
        tips, default = await _branch_tips(env, owner, repo)
        base = default or "main"
    page, page_size = _page_args(request)
    status, compared = await _read_repo(
        env, owner, repo, "compare", {"base": base, "head": branch})
    merge = merge_projection(record)
    if status != 200 or not compared or compared.get("ok") is not True:
        text = json.dumps(compared or {})
        if "too large" in text or status == 413:
            return _error(
                "diff_too_large",
                "This branch changes more than one comparison can carry. "
                "Open it on a desktop instead.", 413)
        if merge["state"] == "merged" and merge["branchDeleted"]:
            return json_response(_diff_payload(
                row, branch, base, {}, [], page, page_size, {}, True),
                cache_control="no-store")
        if status in (0, 502, 503, 504):
            return _error(
                "mirror_unavailable",
                "No core answered for this repository. Try again shortly.",
                503, retryable=True, degraded=True)
        if published_nothing(row, record):
            return json_response(_diff_payload(
                row, branch, base, {}, [], page, page_size, {},
                merge["state"] == "merged"), cache_control="no-store")
        return _error(
            "branch_not_found", _missing_branch_message(row, record, branch),
            404, retryable=True)
    files = parse_patch_files(compared.get("patch"))
    _, preview = await _read_repo(
        env, owner, repo, "merge-preview", {"base": base, "head": branch})
    preview = preview if isinstance(preview, dict) and preview.get(
        "ok") is True else {}
    commits = _int(compared.get("commitCount"))
    ahead = _int(preview.get("ahead")) if preview else commits
    merged = bool(preview) and ahead == 0
    stats = _stats_from_files(files, commits)
    diff_stats = {
        "files": stats["files"], "added": stats["added"],
        "removed": stats["removed"], "ahead": ahead,
        "behind": _int(preview.get("behind")),
        "conflicted": preview.get("clean") is False,
        "known": True,
    }
    if record.get("diffStats") != diff_stats:
        record["diffStats"] = diff_stats
        try:
            await _save(env, row, record)
        except Exception:
            pass
    return json_response(_diff_payload(
        row, branch, base, compared, files, page, page_size, preview,
        merged or merge["state"] == "merged"), cache_control="no-store")


def _diff_payload(row, branch, base, compared, files, page, page_size,
                  preview, merged):
    page_count = max(1, (len(files) + page_size - 1) // page_size)
    page = min(page, page_count - 1)
    chunk = files[page * page_size:(page + 1) * page_size]
    conflicts = preview.get("conflicts") if isinstance(preview, dict) else []
    return {
        "ok": True,
        "id": str(row.get("session_id") or ""),
        "branch": branch,
        "base": base,
        "baseOid": str(compared.get("baseOid") or ""),
        "headOid": str(compared.get("headOid") or ""),
        "mergeBaseOid": str(compared.get("mergeBaseOid") or ""),
        "commits": _int(compared.get("commitCount")),
        "ahead": _int((preview or {}).get("ahead")),
        "behind": _int((preview or {}).get("behind")),
        "clean": (preview or {}).get("clean") is not False,
        "conflicts": [str(item) for item in (conflicts or [])][:50],
        "merged": bool(merged),
        "files": [
            {key: item[key] for key in (
                "path", "oldPath", "status", "added", "removed", "binary")}
            for item in files
        ],
        "added": sum(item["added"] for item in files),
        "removed": sum(item["removed"] for item in files),
        "page": page,
        "pageSize": page_size,
        "pageCount": page_count,
        "committed": "".join(item["patch"] for item in chunk),
        "uncommitted": "",
        "untracked": [],
        "hasWorktree": False,
        "truncated": page < page_count - 1,
    }


async def session_action(env, request, context, row, record, action, data,
                         owner_scope=False):
    row = dict(row)
    row.setdefault("org_bi", context["orgBi"])
    context = dict(context)
    context["servedOwner"] = await _served_owner(
        env, context["org"], context["repo"])
    if action == "merge":
        return await _merge(env, request, context, row, record, data,
                            owner_scope)
    if action == "link-pull":
        return await _link_pull(env, context, row, record, data)
    if action == "update":
        return await _update(env, request, context, row, record, data)
    return await _cleanup(env, request, context, row, record, data,
                          owner_scope)


def _merge_message(branch, base, deleted, pending):
    text = "Merged " + branch + " into " + base
    if deleted and not pending:
        return text + " and cleaned up."
    if deleted:
        return (text + " and removed the branch; " + ", ".join(pending)
                + " still has it and is retried on the next clean up.")
    return text + "."


async def _audit(env, context, action, row, outcome, detail):
    try:
        await _audit_sensitive_action(
            env, context["actor"], "owner." + action, "owner_agent",
            context["org"] + "/" + context["repo"] + "/" +
            str(row.get("session_id") or ""), outcome, detail)
    except Exception:
        pass


async def _deploy_approval(env, context, row, data, request_id):
    deploy_approvals = globals().get("_deploy_approvals")
    if (deploy_approvals is None or not request_id
            or data.get("deployApproval") is None):
        return None
    try:
        return await deploy_approvals.verify(
            env, context["actor"], data["deployApproval"], "branch",
            context["org"], context["repo"],
            str(row.get("session_id") or ""), "", "", request_id,
            int(Date.now()))
    except Exception:
        return None


async def _merge(env, request, context, row, record, data, owner_scope):
    if str(row.get("status") or "") in ACTIVE_STATUSES:
        return _error(
            "session_running",
            "The agent is still running. Stop it or wait for it to finish "
            "before merging.", 409, required_action="stop_session")
    branch = session_branch(record)
    if not valid_agent_branch(branch):
        return _error(
            "no_branch",
            "This run has no agent branch to merge yet.", 409,
            required_action="wait_for_agent")
    merge = record.get("merge") if isinstance(record.get("merge"), dict) \
        else {}
    cleanup = data.get("cleanup") is not False
    request_id = clean_string(data.get("requestId"), 80).strip()
    if request_id and not REQUEST_ID_RE.fullmatch(request_id):
        return _error("invalid_request", "The request id is invalid.", 400)
    approved = await _deploy_approval(env, context, row, data, request_id)
    mirror, reason = await _mirror_context(
        env, context["servedOwner"], context["repo"])
    if mirror is None:
        return _error(
            reason or "not_found",
            "This repository is not served by a core, so it cannot be "
            "merged from here.", 503 if reason else 404,
            retryable=bool(reason), degraded=bool(reason))
    pending = merge.get("request") if merge.get("state") == "merging" else None
    node = str(merge.get("node") or "")
    candidates = None
    if isinstance(pending, dict):
        fresh = int(Date.now()) - _int(merge.get("startedAt"), 2 ** 53 - 1) \
            < PENDING_MERGE_MS
        if not (fresh or not request_id
                or request_id == pending.get("requestId")):
            candidates = await _https_mirror_candidates(
                env, mirror, world_request_country(request), sticky=node,
                operation=MERGE_OPERATION)
            if not any(item.get("node") == node for item in candidates):
                pending = None
                candidates = None
    if isinstance(pending, dict):
        node_request = pending
    else:
        if merge.get("state") == "merged" or _info(record).get("merged"):
            if cleanup and not merge.get("branchDeleted"):
                return await _cleanup(env, request, context, row, record,
                                      data, owner_scope)
            return _error(
                "already_merged",
                branch + " is already merged. Clean it up instead.", 409,
                required_action="cleanup")
        tips, default = await _branch_tips(env, context["servedOwner"],
                                           context["repo"])
        if tips is None:
            return _error(
                "mirror_unavailable",
                "No core answered for this repository. Try again shortly.",
                503, retryable=True, degraded=True)
        base = session_base(record) or default or "main"
        protection = await _branch_protection.branch_refusal(
            env, context["servedOwner"], context["repo"], [base])
        if protection is not None:
            return protection
        head_oid, base_oid = tips.get(branch, ""), tips.get(base, "")
        if not head_oid and published_nothing(row, record):
            return await _merge_nothing(env, context, row, record, branch,
                                        base, request_id)
        if not head_oid:
            return _error(
                "branch_not_found",
                _missing_branch_message(row, record, branch), 404,
                retryable=True)
        if not base_oid or len(base_oid) != len(head_oid):
            return _error(
                "no_base", base + " was not found on ForkMesh.", 409)
        request_id = request_id or _new_request_id()
        node_request = {
            "schemaVersion": 1,
            "type": "forkmesh.branch-merge-v1",
            "requestId": request_id,
            "branch": branch,
            "base": base,
            "expectedBaseOid": base_oid,
            "expectedHeadOid": head_oid,
        }
        if cleanup:
            node_request["deleteBranch"] = True
        candidates = await _https_mirror_candidates(
            env, mirror, world_request_country(request),
            sticky=session_home(row, record), operation=MERGE_OPERATION)
        selected, unavailable = await _route_handlers._pull_mutation_node(
            env, mirror, candidates, MERGE_OPERATION)
        if selected is None:
            return _error(
                "merge_node_unavailable",
                "No core can merge this branch right now. Try again in a "
                "minute.", 503, retryable=True,
                degraded=unavailable != "no_merge_executor")
        node = selected["node"]
        stats = {"base": base, "commits": 0, "files": 0, "insertions": 0,
                 "deletions": 0}
        compare_status, compared = await _read_repo(
            env, context["servedOwner"], context["repo"], "compare",
            {"base": base_oid, "head": head_oid})
        if compare_status == 200 and compared and compared.get("ok") is True:
            files = parse_patch_files(compared.get("patch"))
            stats.update({
                "commits": _int(compared.get("commitCount")),
                "files": len(files),
                "insertions": sum(item["added"] for item in files),
                "deletions": sum(item["removed"] for item in files),
            })
        merge = {
            "state": "merging", "requestId": request_id,
            "request": node_request, "node": node, "base": base,
            "head": head_oid, "stats": stats,
            "startedAt": int(Date.now()),
        }
        record["merge"] = merge
        await _save(env, row, record)
        await _audit(env, context, "agent_branch_merge", row, "requested",
                     {"branch": branch, "base": base, "node": node})
    if candidates is None:
        candidates = await _https_mirror_candidates(
            env, mirror, world_request_country(request), sticky=node,
            operation=MERGE_OPERATION)
    endpoint = next(
        (item for item in candidates if item.get("node") == node), None)
    if endpoint is None:
        return _error(
            "merge_node_unavailable",
            "The core running this merge is not answering yet.", 503,
            retryable=True, degraded=True)
    upstream, result = await _https_mirror_merge_proxy(
        env, endpoint, mirror, node_request, "branch-merge")
    if result is None:
        return _error(
            "merge_status_pending",
            "The merge result is not known yet. Try again.", 503,
            retryable=True, degraded=True)
    if upstream == 202 or result.get("status") == "processing":
        return json_response({
            "ok": True, "status": "processing",
            "requestId": node_request["requestId"],
            "message": "Merging " + branch + "…",
        }, status=202, cache_control="no-store")
    base = node_request["base"]
    if result.get("status") != "merged":
        error = str(result.get("error") or "failed")
        conflicts = []
        if error == "merge_conflict":
            _, preview = await _read_repo(
                env, context["servedOwner"], context["repo"],
                "merge-preview",
                {"base": base, "head": branch})
            conflicts = [
                str(item) for item in ((preview or {}).get("conflicts") or [])
            ][:50]
            message = (branch + " conflicts with " + base + ". Continue the "
                       "agent to resolve it, then merge again.")
            record["merge"] = {"state": "conflict", "base": base,
                               "conflicts": conflicts, "message": message}
        elif error in ("stale_base", "stale_head"):
            message = ("A branch moved while merging. Nothing changed; "
                       "try again.")
            record["merge"] = {}
        elif error == "already_merged":
            message = branch + " is already merged. Clean it up instead."
            record["merge"] = {"state": "merged", "base": base,
                               "branchDeleted": False,
                               "mergedAt": int(Date.now())}
        else:
            message = "The core refused the merge (" + error + ")."
            record["merge"] = {"state": "failed", "base": base,
                               "message": message}
        await _save(env, row, record, touch=True)
        await _audit(env, context, "agent_branch_merge", row, "failed",
                     {"branch": branch, "reason": error})
        await _ring_cloud_agent_readers(env, record)
        payload = {"ok": False, "status": "failed", "error": error,
                   "message": message, "requestId": node_request["requestId"]}
        if conflicts:
            payload["conflicts"] = conflicts
        return json_response(payload, status=409, cache_control="no-store")
    admitted = await _publish_core_merge(env, mirror, endpoint, result,
                                         context["actor"])
    if approved is not None:
        try:
            await globals()["_deploy_approvals"].record(
                env, approved, result, int(Date.now()))
        except Exception:
            pass
    deleted = result.get("branchDeleted") is True
    pending_nodes = []
    if deleted:
        pending_nodes, _done, _refused, _processing = await _fan_out_cleanup(
            env, request, mirror, branch, base, node_request[
                "expectedHeadOid"], node_request["requestId"], {node})
    stats = dict(merge.get("stats") or {})
    stats["base"] = base
    stats["commit"] = str(result.get("baseAfter") or "")
    now = int(Date.now())
    record["merge"] = {
        "state": "merged", "base": base, "head": node_request[
            "expectedHeadOid"],
        "mergeOid": stats["commit"], "mergedAt": now,
        "branchDeleted": deleted, "cleanupPending": pending_nodes,
        "requestId": node_request["requestId"], "stats": stats,
        "message": _merge_message(branch, base, deleted, pending_nodes),
    }
    info = dict(_info(record))
    info["merged"] = True
    info["mergedAt"] = now
    record["agentInfo"] = info
    await _save(env, row, record, touch=True)
    await _audit(env, context, "agent_branch_merge", row, "success",
                 {"branch": branch, "base": base,
                  "statePinned": bool(admitted), "cleanup": deleted})
    await _ring_cloud_agent_readers(env, record)
    return json_response({
        "ok": True, "status": "merged",
        "requestId": node_request["requestId"],
        "branch": branch, "base": base, "mergeOid": stats["commit"],
        "branchDeleted": deleted, "cleanupPending": pending_nodes,
        "stats": {key: stats.get(key) for key in (
            "base", "commit", "commits", "files", "insertions", "deletions")},
        "message": record["merge"]["message"],
    }, cache_control="no-store")


async def _publish_core_merge(env, mirror, endpoint, result, pusher=""):
    admitted = ""
    try:
        admitted = await _admit_owner_merge_state(
            env, mirror, endpoint, result)
    except Exception:
        admitted = ""
    generation = int(Date.now()) if admitted else 0
    try:
        scheduled = await schedule_repo_mirror_wake(
            env, mirror["owner"], mirror["repo"], "commits", admitted,
            generation)
        if not scheduled:
            await notify_repo_mirrors(
                env, mirror["owner"], mirror["repo"], "commits",
                admitted, generation)
    except Exception:
        pass
    try:
        await record_hosted_push_state(
            env, mirror["owner"], mirror["repo"], endpoint, pusher=pusher)
    except Exception:
        pass
    queue_actions = globals().get("queue_runner_actions")
    if queue_actions is not None:
        await queue_actions(
            env, mirror["owner"], mirror["repo"],
            str(result.get("baseAfter") or ""),
            (endpoint or {}).get("node", ""), pusher)
    try:
        await notify_repo_host(
            env, mirror["owner"], mirror["repo"], "mirror_merge")
    except Exception:
        pass
    return admitted


async def _merge_nothing(env, context, row, record, branch, base,
                         request_id):
    now = int(Date.now())
    request_id = request_id or _new_request_id()
    stats = {"base": base, "commit": "", "commits": 0, "files": 0,
             "insertions": 0, "deletions": 0}
    message = (branch + " had no changes, so nothing was merged into " + base
               + ". Cleaned up.")
    record["merge"] = {
        "state": "merged", "base": base, "head": "", "mergeOid": "",
        "mergedAt": now, "branchDeleted": True, "cleanupPending": [],
        "requestId": request_id, "stats": stats, "message": message,
    }
    info = dict(_info(record))
    info["merged"] = True
    info["mergedAt"] = now
    record["agentInfo"] = info
    await _save(env, row, record, touch=True)
    await _audit(env, context, "agent_branch_merge", row, "success",
                 {"branch": branch, "base": base, "empty": True})
    await _ring_cloud_agent_readers(env, record)
    return json_response({
        "ok": True, "status": "merged", "requestId": request_id,
        "branch": branch, "base": base, "mergeOid": "",
        "branchDeleted": True, "cleanupPending": [], "stats": stats,
        "message": message,
    }, cache_control="no-store")


async def _fan_out_cleanup(env, request, mirror, branch, base, tip,
                           request_id, skip, discard=False):
    candidates = await _https_mirror_candidates(
        env, mirror, world_request_country(request),
        operation=DELETE_OPERATION)
    pending, done, refused, processing = [], [], [], []
    for endpoint in candidates:
        node = str(endpoint.get("node") or "")
        if not node or node in skip:
            continue
        node_request = {
            "schemaVersion": 1,
            "type": "forkmesh.branch-delete-v1",
            "requestId": _derived_request_id(request_id, node),
            "branch": branch,
            "base": base,
            "expectedHeadOid": tip,
        }
        if discard:
            node_request["discard"] = True
        result = None
        for _attempt in range(2):
            status, result = await _https_mirror_merge_proxy(
                env, endpoint, mirror, node_request, "branch-delete")
            if result is not None and status != 202:
                break
        if result is None or result.get("status") == "processing":
            pending.append(node)
            if result is not None:
                processing.append(node)
        elif result.get("status") == "deleted":
            done.append(node)
        elif result.get("error") == "branch_not_found":
            done.append(node)
        elif result.get("error") in ("not_merged", "stale_base"):
            pending.append(node)
        else:
            refused.append(node)
    return pending, done, refused, processing


async def _cleanup(env, request, context, row, record, data, owner_scope):
    if str(row.get("status") or "") in ACTIVE_STATUSES:
        return _error(
            "session_running",
            "The agent is still running. Stop it before cleaning up.", 409,
            required_action="stop_session")
    branch = session_branch(record)
    if not valid_agent_branch(branch):
        return _error("no_branch", "This run has no agent branch.", 409)
    mirror, reason = await _mirror_context(
        env, context["servedOwner"], context["repo"])
    if mirror is None:
        return _error(
            reason or "not_found",
            "This repository is not served by a core.",
            503 if reason else 404, retryable=bool(reason),
            degraded=bool(reason))
    merge = dict(record.get("merge") if isinstance(record.get("merge"), dict)
                 else {})
    tips, default = await _branch_tips(
        env, context["servedOwner"], context["repo"])
    base = merge.get("base") or session_base(record) or default or "main"
    discard = data.get("discard") is True
    tip = str(merge.get("head") or (tips or {}).get(branch) or "")
    if discard:
        tip = str((tips or {}).get(branch) or merge.get("head") or "")
    if not OID_RE.fullmatch(tip):
        merge.update({"state": merge.get("state") or (
                          "" if discard else "merged"),
                      "branchDeleted": True, "cleanupPending": []})
        record["merge"] = merge
        await _save(env, row, record, touch=True)
        return json_response({
            "ok": True, "status": "cleaned", "branch": branch,
            "branchDeleted": True, "cleanupPending": [],
            "message": branch + " is already gone.",
        }, cache_control="no-store")
    request_id = clean_string(data.get("requestId"), 80).strip()
    if not REQUEST_ID_RE.fullmatch(request_id):
        request_id = str(merge.get("requestId") or "") or _new_request_id()
    pending, done, refused, processing = await _fan_out_cleanup(
        env, request, mirror, branch, base, tip,
        request_id + ("-d" if discard else "-c"), set(), discard)
    if not done and not pending and refused:
        return _error(
            "stale_head",
            branch + " has new commits on ForkMesh, so it was kept.", 409)
    if discard and not done and processing:
        return json_response({
            "ok": True, "status": "processing", "requestId": request_id,
            "branch": branch, "message": "Deleting " + branch + "…",
        }, status=202, cache_control="no-store")
    if discard and not done:
        await _audit(env, context, "agent_branch_discard", row, "failed",
                     {"branch": branch, "pending": pending,
                      "refused": refused})
        return _error(
            "branch_delete_pending",
            branch + " could not be deleted on ForkMesh yet, so the run "
            "was kept. Try again.", 503, retryable=True)
    if discard:
        pending = pending + refused
        merge.update({"branchDeleted": True, "cleanupPending": pending})
        record["merge"] = merge
        await _save(env, row, record, touch=True)
        await _audit(env, context, "agent_branch_discard", row, "success",
                     {"branch": branch, "nodes": done, "pending": pending})
        await _ring_cloud_agent_readers(env, record)
        return json_response({
            "ok": True, "status": "cleaned", "branch": branch,
            "branchDeleted": True, "cleanupPending": pending,
            "message": "Deleted " + branch + "." if not pending else
            "Deleted " + branch + "; " + ", ".join(pending) +
            " still has it.",
        }, cache_control="no-store")
    if not done and pending and not merge.get("branchDeleted"):
        await _audit(env, context, "agent_branch_cleanup", row, "failed",
                     {"branch": branch, "pending": pending})
        return _error(
            "not_merged",
            branch + " is not merged into " + base + " yet, so it was kept. "
            "Merge it first.", 409, required_action="merge")
    merge.update({
        "state": "merged", "base": base, "head": tip,
        "branchDeleted": True, "cleanupPending": pending,
        "mergedAt": merge.get("mergedAt") or int(Date.now()),
        "message": (
            "Cleaned up " + branch + "." if not pending else
            "Removed " + branch + "; " + ", ".join(pending) +
            " still has it and is retried on the next clean up."),
    })
    record["merge"] = merge
    await _save(env, row, record, touch=True)
    await _audit(env, context, "agent_branch_cleanup", row, "success",
                 {"branch": branch, "nodes": done, "pending": pending})
    await _ring_cloud_agent_readers(env, record)
    return json_response({
        "ok": True, "status": "cleaned", "branch": branch,
        "branchDeleted": True, "cleanupPending": pending,
        "message": merge["message"],
    }, cache_control="no-store")


async def _link_pull(env, context, row, record, data):
    branch = session_branch(record)
    if not valid_agent_branch(branch):
        return _error(
            "no_branch",
            "This run has no agent branch to open a pull request from.", 409,
            required_action="wait_for_agent")
    number = data.get("number")
    if (isinstance(number, bool) or not isinstance(number, int)
            or number < 1 or number > MAX_PULL_NUMBER):
        return _error(
            "invalid_pull", "The pull request number is invalid.", 400)
    current = session_pull_number(record)
    if current and current != number:
        return _error(
            "pull_exists",
            branch + " already has pull request #" + str(current) + ".",
            409, required_action="open_pull")
    if current != number or not isinstance(record.get("pull"), dict):
        record["pull"] = {"number": number, "branch": branch,
                          "linkedAt": int(Date.now()),
                          "linkedBy": context["actor"]}
        await _save(env, row, record, touch=True)
        await _audit(env, context, "agent_branch_pull", row, "success",
                     {"branch": branch, "number": number})
        await _ring_cloud_agent_readers(env, record)
    return json_response({
        "ok": True, "status": "linked", "branch": branch,
        "prNumber": number,
        "message": "Opened pull request #" + str(number) + " for " + branch
        + ".",
    }, cache_control="no-store")


async def _core_endpoint(env, request, mirror, operation, node=""):
    candidates = await _https_mirror_candidates(
        env, mirror, world_request_country(request), sticky=node,
        operation=operation)
    if node:
        return next(
            (item for item in candidates if item.get("node") == node),
            None), "merge_node_unavailable"
    selected, unavailable = await _route_handlers._pull_mutation_node(
        env, mirror, candidates, operation)
    return selected, unavailable or "merge_node_unavailable"


async def _merge_conflicts(env, owner, repo, base, head):
    _, preview = await _read_repo(
        env, owner, repo, "merge-preview", {"base": base, "head": head})
    return [
        str(item) for item in ((preview or {}).get("conflicts") or [])
    ][:50]


async def _update(env, request, context, row, record, data):
    if str(row.get("status") or "") in ACTIVE_STATUSES:
        return _error(
            "session_running",
            "The agent is still running. Stop it or wait for it to finish "
            "before updating its branch.", 409,
            required_action="stop_session")
    branch = session_branch(record)
    if not valid_agent_branch(branch):
        return _error("no_branch", "This run has no agent branch.", 409)
    mirror, reason = await _mirror_context(
        env, context["servedOwner"], context["repo"])
    if mirror is None:
        return _error(
            reason or "not_found",
            "This repository is not served by a core, so its branch cannot "
            "be updated from here.", 503 if reason else 404,
            retryable=bool(reason), degraded=bool(reason))
    pending = record.get("update") if isinstance(
        record.get("update"), dict) else {}
    fresh = (pending.get("state") == "updating"
             and int(Date.now()) - _int(pending.get("startedAt"),
                                        2 ** 53 - 1) < PENDING_MERGE_MS
             and isinstance(pending.get("request"), dict))
    if fresh:
        node_request = pending["request"]
        node = str(pending.get("node") or "")
        base = node_request["branch"]
    else:
        tips, default = await _branch_tips(
            env, context["servedOwner"], context["repo"])
        if tips is None:
            return _error(
                "mirror_unavailable",
                "No core answered for this repository. Try again shortly.",
                503, retryable=True, degraded=True)
        base = session_base(record) or default or "main"
        head_oid, base_oid = tips.get(branch, ""), tips.get(base, "")
        if not head_oid:
            return _error(
                "branch_not_found",
                _missing_branch_message(row, record, branch), 404,
                retryable=True)
        if not base_oid or len(base_oid) != len(head_oid):
            return _error("no_base", base + " was not found on ForkMesh.",
                          409)
        request_id = clean_string(data.get("requestId"), 80).strip()
        if not REQUEST_ID_RE.fullmatch(request_id):
            request_id = _new_request_id()
        node_request = {
            "schemaVersion": 1,
            "type": "forkmesh.branch-merge-v1",
            "requestId": request_id,
            "branch": base,
            "base": branch,
            "expectedBaseOid": head_oid,
            "expectedHeadOid": base_oid,
        }
        endpoint, unavailable = await _core_endpoint(
            env, request, mirror, MERGE_OPERATION)
        if endpoint is None:
            return _error(
                "merge_node_unavailable",
                "No core can update this branch right now. Try again in a "
                "minute.", 503, retryable=True,
                degraded=unavailable != "no_merge_executor")
        node = endpoint["node"]
        record["update"] = {"state": "updating", "request": node_request,
                            "node": node, "startedAt": int(Date.now())}
        await _save(env, row, record)
    endpoint, _ = await _core_endpoint(env, request, mirror,
                                       MERGE_OPERATION, node)
    if endpoint is None:
        return _error(
            "merge_node_unavailable",
            "The core updating this branch is not answering yet.", 503,
            retryable=True, degraded=True)
    upstream, result = await _https_mirror_merge_proxy(
        env, endpoint, mirror, node_request, "branch-merge")
    if result is None:
        return _error(
            "merge_status_pending",
            "The update result is not known yet. Try again.", 503,
            retryable=True, degraded=True)
    if upstream == 202 or result.get("status") == "processing":
        return json_response({
            "ok": True, "status": "processing",
            "requestId": node_request["requestId"],
            "message": "Updating " + branch + " from " + base + "…",
        }, status=202, cache_control="no-store")
    now = int(Date.now())
    if result.get("status") == "merged":
        await _publish_core_merge(env, mirror, endpoint, result,
                                  context["actor"])
        tip = str(result.get("baseAfter") or "")
        message = "Updated " + branch + " from " + base + "."
        record["update"] = {"state": "updated", "base": base, "head": tip,
                            "at": now, "message": message}
        stats = diff_stats_projection(record.get("diffStats"))
        stats["behind"] = 0
        stats["known"] = False
        record["diffStats"] = stats
        if (record.get("merge") or {}).get("state") == "conflict":
            record["merge"] = {}
        await _save(env, row, record, touch=True)
        await _audit(env, context, "agent_branch_update", row, "success",
                     {"branch": branch, "base": base})
        await _ring_cloud_agent_readers(env, record)
        return json_response({
            "ok": True, "status": "updated", "branch": branch, "base": base,
            "head": tip, "requestId": node_request["requestId"],
            "message": message,
        }, cache_control="no-store")
    error = str(result.get("error") or "failed")
    if error == "already_merged":
        message = branch + " is already up to date with " + base + "."
        record["update"] = {"state": "updated", "base": base, "at": now,
                            "message": message}
        await _save(env, row, record)
        return json_response({
            "ok": True, "status": "already", "branch": branch, "base": base,
            "requestId": node_request["requestId"], "message": message,
        }, cache_control="no-store")
    conflicts = []
    if error == "merge_conflict":
        conflicts = await _merge_conflicts(
            env, context["servedOwner"], context["repo"], base, branch)
        message = (branch + " conflicts with " + base + ". Continue the "
                   "agent to resolve it, then update again.")
        record["merge"] = {"state": "conflict", "base": base,
                           "conflicts": conflicts, "message": message}
    elif error in ("stale_base", "stale_head"):
        message = "A branch moved while updating. Nothing changed; try again."
    else:
        message = "The core refused the update (" + error + ")."
    record["update"] = {}
    await _save(env, row, record, touch=True)
    await _audit(env, context, "agent_branch_update", row, "failed",
                 {"branch": branch, "reason": error})
    payload = {"ok": False, "status": "failed", "error": error,
               "message": message, "requestId": node_request["requestId"]}
    if conflicts:
        payload["conflicts"] = conflicts
    return json_response(payload, status=409, cache_control="no-store")


CORE_BRANCH_OPS = ("merge", "update", "delete")


def _core_op_request(op, record, base, head_oid, request_id):
    branch = record["branch"]
    if not base or base == branch:
        return None
    if op == "delete":
        return {
            "schemaVersion": 1, "type": "forkmesh.branch-delete-v1",
            "requestId": request_id, "branch": branch, "base": base,
            "expectedHeadOid": record["expectedOid"], "discard": True,
        }
    if op == "update":
        if not OID_RE.fullmatch(head_oid):
            return None
        return {
            "schemaVersion": 1, "type": "forkmesh.branch-merge-v1",
            "requestId": request_id, "branch": base, "base": branch,
            "expectedBaseOid": record["expectedOid"],
            "expectedHeadOid": head_oid,
        }
    node_request = {
        "schemaVersion": 1, "type": "forkmesh.branch-merge-v1",
        "requestId": request_id, "branch": branch, "base": base,
        "expectedBaseOid": record["baseOid"],
        "expectedHeadOid": record["expectedOid"],
    }
    if record.get("deleteBranch"):
        node_request["deleteBranch"] = True
    return node_request


BRANCH_TRASH_MAX_ENTRIES = 5000


async def core_branch_trash(env, request, owner, repo):
    served = await _served_owner(env, owner, repo)
    mirror, _reason = await _mirror_context(env, served, repo)
    if mirror is None:
        return None
    candidates = await _https_mirror_candidates(
        env, mirror, world_request_country(request),
        operation=DELETE_OPERATION)
    merged, answered, unanswered, truncated = {}, [], [], False
    for endpoint in candidates:
        node = str(endpoint.get("node") or "")
        if not node or node in answered or node in unanswered:
            continue
        listing = await _https_mirror_branch_trash_proxy(env, endpoint, mirror)
        if listing is None:
            unanswered.append(node)
            continue
        answered.append(node)
        truncated = truncated or listing["truncated"]
        for item in listing["entries"]:
            merged.setdefault(item["branch"], set()).add(item["oid"])
    if not answered:
        return None
    entries = [{"branch": branch, "oid": oid}
               for branch in sorted(merged) for oid in sorted(merged[branch])]
    return {
        "entries": entries[:BRANCH_TRASH_MAX_ENTRIES],
        "truncated": truncated or len(entries) > BRANCH_TRASH_MAX_ENTRIES,
        "cores": sorted(answered),
        "unanswered": sorted(unanswered),
    }


BRANCH_DELETE_MAX_TRIES = 6
BRANCH_DELETE_MAX_POLLS = 120
GIT_BRANCH_FORBIDDEN = frozenset(" ~^:?*[\\")
GIT_BRANCH_MAX_BYTES = 255


def git_branch_name(value):
    if not isinstance(value, str):
        return ""
    name = value.strip()
    if (
        not name
        or len(name.encode("utf-8")) > GIT_BRANCH_MAX_BYTES
        or name in ("HEAD", "@")
        or name.startswith(("-", "/"))
        or name.endswith(("/", "."))
        or "//" in name
        or ".." in name
        or "@{" in name
        or any(ord(ch) < 0x20 or ord(ch) == 0x7f
               or ch in GIT_BRANCH_FORBIDDEN for ch in name)
        or any(part.startswith(".") or part.endswith(".lock")
               for part in name.split("/"))
    ):
        return ""
    return name


async def branch_delete_target(env, request, owner, repo):
    served = await _served_owner(env, owner, repo)
    mirror, reason = await _mirror_context(env, served, repo)
    if mirror is None:
        return None, reason or "not_served"
    endpoint, _unavailable = await _core_endpoint(
        env, request, mirror, DELETE_OPERATION)
    if endpoint is None:
        return None, "not_served"
    tips, default = await _branch_tips(env, served, repo)
    if tips is None or not default or not tips.get(default):
        return None, "branches_unavailable"
    protected, problem = await _branch_protection.push_protection(
        env, served, repo)
    if problem:
        return None, problem
    return {"served": served, "mirror": mirror, "endpoint": endpoint,
            "tips": tips, "default": default,
            "protected": protected or []}, ""


async def _branch_delete_route(env, request, job, cache):
    if cache.get("mirror") and cache.get("endpoint"):
        return cache["mirror"], cache["endpoint"]
    served = await _served_owner(env, job["owner"], job["repo"])
    mirror, _reason = await _mirror_context(env, served, job["repo"])
    if mirror is None:
        return None, None
    endpoint, _unavailable = await _core_endpoint(
        env, request, mirror, DELETE_OPERATION, job.get("node", ""))
    if endpoint is None and job.get("node"):
        endpoint, _unavailable = await _core_endpoint(
            env, request, mirror, DELETE_OPERATION)
    if endpoint is None:
        return mirror, None
    cache.update({"mirror": mirror, "endpoint": endpoint})
    job["node"] = endpoint["node"]
    return mirror, endpoint


def _branch_delete_final(item, status, error, message):
    item.update({"status": status, "error": error, "message": message,
                 "phase": "", "polls": 0, "tries": 0})
    return item


def _branch_delete_failure(item, error):
    branch = item["branch"]
    messages = {
        "stale_head": branch + " moved since the page read it, so it was "
                      "kept. Refresh and try again.",
        "stale_base": "The default branch moved while deleting " + branch
                      + ", so it was kept. Try again.",
        "unsupported_branch": "The default branch cannot be deleted.",
        "merge_node_unavailable": "The core did not answer after "
                                  + str(BRANCH_DELETE_MAX_TRIES)
                                  + " tries, so " + branch + " was kept. "
                                  "Try again.",
    }
    return _branch_delete_final(item, "failed", error or "failed", messages.get(
        error, "The core refused to delete " + branch + " ("
        + (error or "failed") + ")."))


def _branch_delete_request(item, base, request_id):
    return {
        "schemaVersion": 1, "type": "forkmesh.branch-delete-v1",
        "requestId": request_id, "branch": item["branch"], "base": base,
        "expectedHeadOid": item["tip"], "discard": True,
    }


def _branch_delete_unanswered(item, cache):
    cache.clear()
    item["tries"] = int(item.get("tries") or 0) + 1
    if item["tries"] >= BRANCH_DELETE_MAX_TRIES:
        return "progress", _branch_delete_failure(
            item, "merge_node_unavailable")
    return "wait", None


async def _branch_delete_primary(env, request, job, item, mirror, endpoint,
                                 cache):
    if not item.get("requestId"):
        item["requestId"] = _new_request_id()
    status, result = await _https_mirror_merge_proxy(
        env, endpoint, mirror,
        _branch_delete_request(item, job["default"], item["requestId"]),
        "branch-delete")
    if result is None:
        return _branch_delete_unanswered(item, cache)
    item["tries"] = 0
    if status == 202 or result.get("status") == "processing":
        item["polls"] = int(item.get("polls") or 0) + 1
        if item["polls"] >= BRANCH_DELETE_MAX_POLLS:
            return "progress", _branch_delete_final(
                item, "pending", "", "Deleting " + item["branch"]
                + " is still running on " + endpoint["node"]
                + ". Refresh in a moment.")
        return "wait", None
    error = str(result.get("error") or "")
    if result.get("status") != "deleted" and error != "branch_not_found":
        return "progress", _branch_delete_failure(item, error)
    candidates = await _https_mirror_candidates(
        env, mirror, world_request_country(request),
        operation=DELETE_OPERATION)
    item.update({
        "phase": "fanout", "polls": 0, "tries": 0,
        "fanout": [str(entry.get("node") or "") for entry in candidates
                   if entry.get("node")
                   and entry.get("node") != endpoint["node"]][:16],
        "left": [],
    })
    return "progress", None


async def _branch_delete_fanout(env, request, job, item, mirror):
    if not item.get("fanout"):
        left = list(item.get("left") or [])
        _branch_delete_final(
            item, "deleted", "", "Deleted " + item["branch"] + "."
            if not left else "Deleted " + item["branch"] + "; "
            + ", ".join(left) + " still has it.")
        item["cleanupPending"] = left
        return "progress", item
    node = item["fanout"][0]
    candidates = await _https_mirror_candidates(
        env, mirror, world_request_country(request), sticky=node,
        operation=DELETE_OPERATION)
    endpoint = next((entry for entry in candidates
                     if entry.get("node") == node), None)

    def move_on(kept):
        item["fanout"] = item["fanout"][1:]
        item["polls"] = item["tries"] = 0
        if kept:
            item["left"] = list(item.get("left") or []) + [node]
        return "progress", None

    if endpoint is None:
        return move_on(True)
    status, result = await _https_mirror_merge_proxy(
        env, endpoint, mirror,
        _branch_delete_request(item, job["default"], _derived_request_id(
            item["requestId"], node)),
        "branch-delete")
    if result is None:
        item["tries"] = int(item.get("tries") or 0) + 1
        return move_on(True) if item["tries"] >= BRANCH_DELETE_MAX_TRIES \
            else ("wait", None)
    if status == 202 or result.get("status") == "processing":
        item["polls"] = int(item.get("polls") or 0) + 1
        return move_on(True) if item["polls"] >= BRANCH_DELETE_MAX_POLLS \
            else ("wait", None)
    return move_on(not (result.get("status") == "deleted"
                        or result.get("error") == "branch_not_found"))


async def branch_delete_job_step(env, request, job, cache):
    item = next((entry for entry in job["items"]
                 if entry.get("status") == "waiting"), None)
    if item is None:
        return "done", None
    job["current"] = item["branch"]
    mirror, endpoint = await _branch_delete_route(env, request, job, cache)
    if mirror is None or (endpoint is None and item.get("phase") != "fanout"):
        return _branch_delete_unanswered(item, cache)
    if item.get("phase") == "fanout":
        return await _branch_delete_fanout(env, request, job, item, mirror)
    return await _branch_delete_primary(
        env, request, job, item, mirror, endpoint, cache)


async def branch_delete_job_publish(env, request, job, cache, actor=""):
    deleted = [item for item in job["items"] if item["status"] == "deleted"]
    if not deleted:
        return False
    mirror, endpoint = await _branch_delete_route(env, request, job, cache)
    if mirror is None:
        return False
    try:
        await record_hosted_push_state(
            env, mirror["owner"], mirror["repo"], endpoint, pusher=actor)
    except Exception:
        pass
    try:
        await record_hosted_branch_count(
            env, mirror["owner"], mirror["repo"],
            max(int(job.get("branchCount") or 0) - len(deleted), 1))
    except Exception:
        pass
    try:
        await notify_repo_host(
            env, mirror["owner"], mirror["repo"], "branches")
    except Exception:
        pass
    return True


async def core_branch_op(env, request, owner, repo, record, data,
                         default_branch="", actor=""):
    op = record.get("op")
    if op not in CORE_BRANCH_OPS:
        return None
    served = await _served_owner(env, owner, repo)
    mirror, _reason = await _mirror_context(env, served, repo)
    if mirror is None:
        return None
    request_id = clean_string(data.get("requestId"), 80).strip()
    node = str(data.get("node") or "").strip().lower()
    resuming = bool(REQUEST_ID_RE.fullmatch(request_id)
                    and valid_node_name(node))
    if resuming:
        base = valid_branch(data.get("pinBase"))
        head_oid = clean_string(data.get("pinOid"), 64).strip().lower()
    else:
        tips, default = await _branch_tips(env, served, repo)
        if tips is None:
            return None
        default = default or default_branch
        request_id = _new_request_id()
        base = {"delete": default,
                "update": record.get("base") or default,
                "merge": record.get("base")}[op]
        head_oid = tips.get(base, "") if op == "update" else ""
        if op != "merge" and not tips.get(base or ""):
            return None
    moved = [record.get("branch", "")] + ([base] if op == "merge" else [])
    protected, problem = await _branch_protection.push_protection(
        env, served, repo)
    if problem or any(
            branch and _branch_protection.branch_is_protected(
                branch, protected) for branch in moved):
        return (503 if problem else 403), {
            "ok": False, "executed": True, "status": "failed",
            "error": problem or "branch_protected", "op": op,
            "branch": record.get("branch", ""),
            "message": "This branch is protected: it changes only by "
                       "merging an approved pull request."}
    node_request = _core_op_request(op, record, base, head_oid, request_id)
    if node_request is None:
        return None if not resuming else (409, {
            "ok": False, "executed": True, "status": "failed",
            "error": "invalid_request", "op": op, "branch": record["branch"],
            "message": "The request could not be resumed. Try again."})
    operation = DELETE_OPERATION if op == "delete" else MERGE_OPERATION
    mutation = "branch-delete" if op == "delete" else "branch-merge"
    endpoint, unavailable = await _core_endpoint(
        env, request, mirror, operation, node if resuming else "")
    if endpoint is None:
        if not resuming:
            return None
        return 503, {
            "ok": False, "executed": True, "status": "failed",
            "error": "merge_node_unavailable", "op": op,
            "branch": record["branch"], "retryable": True,
            "message": "The core running this request is not answering "
                       "yet. Try again."}
    upstream, result = await _https_mirror_merge_proxy(
        env, endpoint, mirror, node_request, mutation)
    if result is None:
        if not resuming:
            return None
        return 503, {
            "ok": False, "executed": True, "status": "failed",
            "error": "merge_status_pending", "op": op,
            "branch": record["branch"], "retryable": True,
            "message": "The result is not known yet. Try again."}
    branch = record["branch"]
    base = node_request["base"] if op != "update" else node_request["branch"]
    answer = {
        "ok": True, "executed": True, "op": op, "branch": branch,
        "base": base, "requestId": request_id, "node": endpoint["node"],
        "pinBase": base,
    }
    if op == "update":
        answer["pinOid"] = node_request["expectedHeadOid"]
    if upstream == 202 or result.get("status") == "processing":
        answer.update({
            "status": "processing",
            "message": {
                "merge": "Merging " + branch + " into " + base + "…",
                "update": "Updating " + branch + " from " + base + "…",
                "delete": "Deleting " + branch + "…",
            }[op],
        })
        return 202, answer
    status = str(result.get("status") or "")
    if status in ("merged", "deleted"):
        if status == "merged":
            await _publish_core_merge(env, mirror, endpoint, result, actor)
        deleted = result.get("branchDeleted") is True
        pending = []
        if deleted:
            pending, _done, refused, _processing = await _fan_out_cleanup(
                env, request, mirror, branch, node_request["base"],
                node_request["expectedHeadOid"] if op != "update"
                else node_request["expectedBaseOid"], request_id,
                {endpoint["node"]}, discard=op == "delete")
            pending = pending + refused
        merge_oid = str(result.get("baseAfter") or "")
        if op == "merge":
            message = _merge_message(branch, base, deleted, pending)
        elif op == "update":
            message = "Updated " + branch + " from " + base + "."
        else:
            message = "Deleted " + branch + "." if not pending else (
                "Deleted " + branch + "; " + ", ".join(pending)
                + " still has it.")
        answer.update({
            "status": {"merge": "merged", "update": "updated",
                       "delete": "deleted"}[op],
            "mergeOid": merge_oid, "branchDeleted": deleted,
            "cleanupPending": pending, "message": message,
        })
        return 200, answer
    error = str(result.get("error") or "failed")
    if error == "already_merged":
        answer.update({
            "status": "already",
            "message": (branch + " is already up to date with " + base + "."
                        if op == "update" else
                        branch + " is already merged into " + base + "."),
        })
        return 200, answer
    if error == "branch_not_found" and op == "delete":
        pending, _done, refused, _processing = await _fan_out_cleanup(
            env, request, mirror, branch, node_request["base"],
            node_request["expectedHeadOid"], request_id,
            {endpoint["node"]}, discard=True)
        pending = pending + refused
        answer.update({
            "status": "already", "cleanupPending": pending,
            "message": branch + " is already gone." if not pending else (
                branch + " is gone from " + endpoint["node"] + "; "
                + ", ".join(pending) + " still has it.")})
        return 200, answer
    conflicts = []
    if error == "merge_conflict":
        source, target = ((base, branch) if op == "update"
                          else (branch, base))
        conflicts = await _merge_conflicts(env, served, repo, target, source)
        message = (branch + " conflicts with " + base + ", so nothing "
                   "changed. Resolve it on the branch, then try again.")
    elif error in ("stale_base", "stale_head"):
        message = ("A branch moved since you looked. Nothing changed; "
                   "refresh and try again.")
    elif error == "branch_not_found":
        message = branch + " is not on ForkMesh."
    elif error == "unsupported_branch":
        message = "The default branch cannot be deleted."
    else:
        message = "The core refused it (" + error + ")."
    answer.update({"ok": False, "status": "failed", "error": error,
                   "message": message})
    if conflicts:
        answer["conflicts"] = conflicts
    return 409, answer


async def next_branch_name(env, org_bi, repo, slug):
    slug = branch_slug(slug)
    if not slug:
        return ""
    rows = await d1_all(
        env,
        "SELECT data FROM org_agent_sessions WHERE org_bi=? AND repo=? "
        "ORDER BY created_at DESC LIMIT ?",
        org_bi, repo, BRANCH_SCAN_LIMIT,
    )
    highest = 0
    taken = set()
    for item in rows or []:
        record = await decrypt_row(env, item.get("data"))
        branch = session_branch(record if isinstance(record, dict) else {})
        if not branch:
            continue
        taken.add(branch)
        match = NUMBERED_BRANCH_RE.match(branch)
        if match:
            highest = max(highest, int(match.group(1)))
    number = highest + 1
    candidate = "%s%d-%s" % (AGENT_PREFIX, number, slug)
    while candidate in taken:
        number += 1
        candidate = "%s%d-%s" % (AGENT_PREFIX, number, slug)
    return candidate
