import json
import re
from urllib.parse import quote


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


PREFIX = "/api/actions/job/"
TOKEN_RE = re.compile(r"^[0-9a-f]{64}$")
SEED_RE = re.compile(r"^[0-9a-fA-F]{64}$")
PKCS8_ED25519_PREFIX = bytes.fromhex("302e020100300506032b657004220420")
BODY_MAX_BYTES = 128 * 1024
TITLE_MAX = 240
LABEL_MAX = 60
LABEL_LIMIT = 20
ISSUE_ROUTE_RE = re.compile(
    r"^issues/([0-9]{1,9})(?:/(comments|labels|state|assignees))?$")
ASSIGNEE_LIMIT = 10
ASSIGNEE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
ISSUE_EVENTS = {
    "open": ("issues", "opened"),
    "edit": ("issues", "edited"),
    "title": ("issues", "edited"),
    "labels": ("issues", "labeled"),
    "status": ("issues", ""),
    "comment": ("issue_comment", "created"),
}


def repo_key(owner, repo):
    return str(owner or "").strip().lower() + "/" + str(repo or "").strip(
    ).lower()


def _error(error, status, message=""):
    body = {"error": error}
    if message:
        body["message"] = message
    return json_response(body, status=status, cache_control="no-store")


def _runtime(env):
    import runner_leases
    runner_leases._bind_runtime(globals())
    return runner_leases.WorkerRuntime(env, None)


def _bearer(request):
    try:
        value = str(request.headers.get("authorization") or "")
    except Exception:
        return ""
    if not value.lower().startswith("bearer "):
        return ""
    token = value[7:].strip()
    return token if TOKEN_RE.fullmatch(token) else ""


async def _job(env, request):
    import runner_actions
    token = _bearer(request)
    if not token:
        return None
    row = await d1_first(
        env, "SELECT id,owner,repo,branch,commit_oid,event,workflow,ref,actor,"
        "before_oid,payload FROM repo_action_jobs WHERE token_hash=? "
        "AND token_hash!='' AND status IN ('leased','running','cancelling') "
        "LIMIT 1", runner_actions.token_hash(token))
    return dict(row) if row else None


async def public_owner(env, owner, repo):
    import runner_actions
    return (await runner_actions.identity(_runtime(env), owner, repo))[1]


async def bot_identity(env):
    seed = str(getattr(env, "ACTIONS_BOT_SEED", "") or "").strip()
    if not SEED_RE.fullmatch(seed):
        return None
    der = PKCS8_ED25519_PREFIX + bytes.fromhex(seed)
    key = await js_crypto.subtle.importKey(
        "pkcs8", Uint8Array.new(_to_js(der)), to_js({"name": "Ed25519"}),
        True, _to_js(["sign"]))
    jwk = await js_crypto.subtle.exportKey("jwk", key)
    if hasattr(jwk, "to_py"):
        jwk = jwk.to_py()
    public_key = str(jwk["x"])
    private_key = str(jwk["d"])
    return public_key.rstrip("="), private_key.rstrip("=")


async def sign_issue_event(env, owner, repo, number, event, bot_name):
    identity = await bot_identity(env)
    if identity is None:
        return None
    public_key, private_key = identity
    event = dict(event)
    event.update({
        "author": public_key, "authorName": bot_name,
        "ts": int(Date.now()),
    })
    event.setdefault("id", "actions-%d-%s" % (
        event["ts"], _b64url_encode(_random_bytes(6))))
    body = "\n".join((
        event["type"], str(int(number)), public_key, str(event["ts"]),
        await sha256_hex(issue_event_content(event))))
    key = repo_key(owner, repo)
    message = ("forkmesh-issue-event-v2\n" + key + "\n" + body).encode()
    event["sig"] = await ed25519_sign(public_key, private_key, message)
    return event if event["sig"] else None


def _labels(value):
    if not isinstance(value, list):
        return []
    labels = []
    for item in value:
        name = item.get("name") if isinstance(item, dict) else item
        name = clean_string(name if isinstance(name, str) else "",
                            LABEL_MAX).strip()
        if name and name not in labels:
            labels.append(name)
    return labels[:LABEL_LIMIT]


def _assignee_list(value):
    if not isinstance(value, list):
        return []
    names = []
    for item in value:
        name = clean_string(item if isinstance(item, str) else "",
                            MAX_NODE_NAME).strip()
        if name and name.lower() not in [n.lower() for n in names]:
            names.append(name)
    return names


def _assignee_names(value):
    if value is None:
        return [], ""
    if isinstance(value, str):
        value = [value]
    if not isinstance(value, list):
        return None, "add and remove are lists of account names."
    names = []
    for item in value:
        name = str(item if isinstance(item, str) else "").strip().lower()
        if name.startswith("@"):
            name = name[1:]
        if not ASSIGNEE_RE.fullmatch(name):
            return None, "%s is not an account name." % (
                str(item)[:MAX_NODE_NAME])
        if name not in names:
            names.append(name)
    return names, ""


def _status(response):
    if isinstance(response, dict):
        return int(response.get("status") or 200)
    return int(getattr(response, "status", 200) or 200)


async def _audit_assignees(env, job, number, bot, outcome, details):
    try:
        await _audit_sensitive_action(
            env, bot, "actions.issue_assignees", "issue",
            "%s/%s#%d" % (job["owner"], job["repo"], int(number)), outcome,
            dict(details, job=int(job.get("id") or 0),
                 workflow=str(job.get("workflow") or "")))
    except Exception:
        pass


async def change_assignees(env, request, job, number, data):
    add, error = _assignee_names(data.get("add"))
    if add is None:
        return _error("invalid_request", 400, error)
    remove, error = _assignee_names(data.get("remove"))
    if remove is None:
        return _error("invalid_request", 400, error)
    if not add and not remove:
        return _error("invalid_request", 400,
                      "Name at least one account to add or remove.")
    if set(add) & set(remove):
        return _error("invalid_request", 400,
                      "An account cannot be added and removed at once.")
    if len(add) + len(remove) > ASSIGNEE_LIMIT:
        return _error("invalid_request", 400,
                      "At most %d accounts per call." % ASSIGNEE_LIMIT)
    owner, repo = job["owner"], job["repo"]
    disabled = await _repo_feature_off(env, owner, repo, "issues")
    if disabled is not None:
        return disabled
    bot = await public_owner(env, owner, repo) + "-actions"
    issue = await read_issue(env, owner, repo, number)
    if issue is None:
        return _error("issue_not_found", 404,
                      "Issue #%d does not exist." % int(number))
    unknown = []
    for name in add:
        _, record = await _account_row(env, name)
        if not record:
            unknown.append(name)
    if unknown:
        await _audit_assignees(env, job, number, bot, "denied", {
            "reason": "unknown_account", "accounts": ",".join(unknown)})
        return _error("unknown_account", 422,
                      "No account named " + ", ".join(unknown) + ".")
    current = [name.lower() for name in issue.get("assignees") or []]
    assigned = [name for name in add if name not in current]
    unassigned = [name for name in remove if name in current]
    final = [name for name in issue.get("assignees") or []
             if name.lower() not in unassigned] + assigned
    for name, state in ([(n, "assigned") for n in assigned] +
                        [(n, "released") for n in unassigned]):
        answer = await _submit(env, request, job, number, {
            "type": "claim", "assignee": name, "claimState": state})
        if not 200 <= _status(answer) < 300:
            await _audit_assignees(env, job, number, bot, "failed", {
                "account": name, "state": state,
                "status": _status(answer)})
            return answer
    if assigned or unassigned:
        await _audit_assignees(env, job, number, bot, "success", {
            "assigned": ",".join(assigned),
            "unassigned": ",".join(unassigned)})
    return json_response({
        "ok": True, "number": int(number), "assignees": final,
        "assigned": assigned, "unassigned": unassigned,
        "changed": bool(assigned or unassigned)},
        cache_control="no-store")


async def read_issue(env, owner, repo, number):
    paths = _forkbot_issue_json_candidates(number)
    data = await _forkbot_repo_gateway_json(
        env, owner, repo,
        "blobs?" + "&".join("path=" + quote(p, safe="") for p in paths))
    blobs = data.get("blobs") if isinstance(data, dict) else None
    if not isinstance(blobs, dict):
        return None
    for path in paths:
        text = _forkbot_blob_text(blobs.get(path))
        if not text:
            continue
        try:
            record = json.loads(text)
        except Exception:
            continue
        if not isinstance(record, dict):
            continue
        events = record.get("events") if isinstance(
            record.get("events"), list) else []
        opened = next((e for e in events if isinstance(e, dict)
                       and e.get("type") == "open"), {})
        state = str(record.get("status") or record.get("state") or "open")
        return {
            "number": int(number),
            "title": clean_string(
                record.get("title") or opened.get("title") or "", TITLE_MAX),
            "body": str(opened.get("body") or record.get("body") or ""),
            "state": "closed" if state.lower() == "closed" else "open",
            "labels": _labels(record.get("labels")),
            "assignees": _assignee_list(record.get("assignees")),
            "author": clean_string(
                record.get("authorName") or opened.get("authorName")
                or record.get("author") or opened.get("author") or "",
                MAX_NODE_NAME),
        }
    return None


def _issue_object(issue):
    return {
        "number": issue["number"], "title": issue["title"],
        "body": issue["body"], "state": issue["state"],
        "labels": [{"name": name} for name in issue["labels"]],
        "user": {"login": issue["author"]},
    }


async def issue_event_payload(env, owner, repo, number, event, item):
    kind = str(event.get("type") or "")
    mapped = ISSUE_EVENTS.get(kind)
    if not mapped or int(number or 0) <= 0:
        return None
    name, action = mapped
    verified = event.get("authorVerified") is True and bool(
        clean_string(event.get("authorName") or "", MAX_NODE_NAME).strip())
    sender = clean_string(
        (event.get("authorName") if verified else "") or
        event.get("author") or "", MAX_NODE_NAME)
    user = {"login": sender, "verified": verified}
    meta = item.get("meta") if isinstance(item.get("meta"), dict) else {}
    if kind == "open":
        issue = {"number": int(number),
                 "title": clean_string(event.get("title") or "", TITLE_MAX),
                 "body": str(event.get("body") or ""), "state": "open",
                 "labels": _labels(meta.get("labels")), "author": sender}
    else:
        issue = await read_issue(env, owner, repo, number) or {
            "number": int(number), "title": item.get("titleIfNew") or "",
            "body": "", "state": "open", "labels": [], "author": ""}
    payload = {"action": action, "sender": dict(user)}
    if kind == "labels":
        new = _labels(event.get("labels"))
        added = [x for x in new if x not in issue["labels"]]
        removed = [x for x in issue["labels"] if x not in new]
        if not added and not removed:
            return None
        payload["action"] = "labeled" if added else "unlabeled"
        payload["label"] = {"name": (added or removed)[0]}
        issue["labels"] = new
    if kind == "title":
        issue["title"] = clean_string(event.get("title") or "", TITLE_MAX)
    if kind == "status":
        state = "closed" if event.get("status") == "closed" else "open"
        payload["action"] = "closed" if state == "closed" else "reopened"
        issue["state"] = state
    if kind == "comment":
        payload["comment"] = {
            "id": str(event.get("id") or ""),
            "body": str(event.get("body") or ""),
            "user": dict(user), "verified": verified}
    payload["issue"] = _issue_object(issue)
    return name, payload


async def queue_issue_event(env, owner, repo, number, event, item):
    import runner_actions
    owner, org = await runner_actions.identity(_runtime(env), owner, repo)
    if str(event.get("authorName") or "").strip().lower() == org + "-actions":
        return []
    rows = await d1_all(
        env, "SELECT events FROM repo_action_workflows WHERE owner=? "
        "AND repo=?", owner, repo)
    wanted = set()
    for row in rows or []:
        try:
            wanted.update(json.loads(row.get("events") or "[]"))
        except ValueError:
            continue
    mapped = ISSUE_EVENTS.get(str(event.get("type") or ""))
    if not mapped or mapped[0] not in wanted:
        return []
    built = await issue_event_payload(env, owner, repo, number, event, item)
    if not built:
        return []
    name, payload = built
    runtime = _runtime(env)
    branch = await runtime.default_branch(owner, repo)
    return await runner_actions.enqueue_catalog(
        runtime, owner, repo, name, branch, payload,
        payload["sender"]["login"])


async def _body(request):
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES)
    except Exception:
        return None
    return data if isinstance(data, dict) else None


async def _submit(env, request, job, number, event, extra=None):
    owner, repo = job["owner"], job["repo"]
    disabled = await _repo_feature_off(env, owner, repo, "issues")
    if disabled is not None:
        return disabled
    bot = await public_owner(env, owner, repo) + "-actions"
    signed = await sign_issue_event(env, owner, repo, number, event, bot)
    if signed is None:
        return _error("bot_unavailable", 503,
                      "The instance has no Actions bot key.")
    data = {"number": number, "event": signed, **(extra or {})}
    repo_bi = await blind_index(env, owner + "/" + repo)
    return await _accept_issue_submission(
        env, request, owner, repo, repo_bi, data,
        (repo_key(owner, repo),), actions_job=True)


async def _event(env, job):
    import runner_actions
    record = {}
    if job.get("payload"):
        try:
            record = await decrypt_row(env, job["payload"]) or {}
        except Exception:
            record = {}
    full_name = await public_owner(env, job["owner"], job["repo"]) + "/" + (
        job["repo"])
    return runner_actions._event_payload(
        job, record if isinstance(record, dict) else {}, full_name)


async def route(env, request, path):
    await ensure_schema(env)
    job = await _job(env, request)
    if job is None:
        return _error("unauthorized", 401,
                      "The job token is missing, wrong or expired.")
    action = str(path or "")[len(PREFIX):].strip("/")
    method = method_name(request)
    if action == "event":
        if method != "GET":
            return _error("method_not_allowed", 405)
        return json_response(await _event(env, job),
                             cache_control="no-store")
    if action == "issues":
        if method != "POST":
            return _error("method_not_allowed", 405)
        data = await _body(request)
        title = clean_string((data or {}).get("title") or "", TITLE_MAX)
        if data is None or not title.strip():
            return _error("invalid_request", 400, "An issue needs a title.")
        body = str(data.get("body") or "")
        return await _submit(
            env, request, job, 0,
            {"type": "open", "title": title, "body": body,
             "attachments": []},
            {"titleIfNew": title,
             "meta": {"labels": _labels(data.get("labels"))}})
    match = ISSUE_ROUTE_RE.fullmatch(action)
    if not match:
        return _error("not_found", 404)
    number = int(match.group(1))
    sub = match.group(2) or ""
    if number <= 0:
        return _error("not_found", 404)
    if not sub:
        if method != "GET":
            return _error("method_not_allowed", 405)
        issue = await read_issue(env, job["owner"], job["repo"], number)
        if issue is None:
            return _error("not_found", 404)
        return json_response(issue, cache_control="no-store")
    if method != "POST":
        return _error("method_not_allowed", 405)
    data = await _body(request)
    if data is None:
        return _error("invalid_json", 400)
    if sub == "comments":
        body = str(data.get("body") or "")
        if not body.strip():
            return _error("invalid_request", 400, "A comment needs a body.")
        return await _submit(env, request, job, number,
                             {"type": "comment", "body": body,
                              "attachments": []})
    if sub == "assignees":
        return await change_assignees(env, request, job, number, data)
    if sub == "state":
        state = data.get("state")
        if state not in ("open", "closed"):
            return _error("invalid_request", 400,
                          "state is open or closed.")
        return await _submit(env, request, job, number,
                             {"type": "status", "status": state})
    add, remove = _labels(data.get("add")), _labels(data.get("remove"))
    issue = await read_issue(env, job["owner"], job["repo"], number)
    if issue is None:
        return _error("not_found", 404)
    labels = [x for x in issue["labels"] if x not in remove]
    labels += [x for x in add if x not in labels]
    return await _submit(env, request, job, number,
                         {"type": "labels", "labels": labels[:LABEL_LIMIT]})
