import hashlib
import json
import re


JOB_TIMEOUT_MS = 90 * 60 * 1000
QUEUED_TTL_MS = 24 * 60 * 60 * 1000
KEEP_MS = 30 * 24 * 60 * 60 * 1000
MAX_ATTEMPTS = 2
SCAN_LIMIT = 50
SNAPSHOT_JOBS = 10
RUN_LIMIT = 20
RUNS_PER_JOB = 32
RUN_ID_STRIDE = 100
LOG_TAIL_MAX_BYTES = 16 * 1024
LOG_MAX_BYTES = 1024 * 1024
LOG_HEAD_BYTES = 768 * 1024
SNAPSHOT_TTL_MS = 60 * 1000
CATALOG_LIMIT = 64
INPUT_LIMIT = 25
INPUT_VALUE_MAX = 64 * 1024
SECRET_LIMIT = 256
PAYLOAD_MAX_BYTES = 1024 * 1024
OPTION_LIMIT = 32
MASK_MIN_BYTES = 4
RUN_STATUSES = ("queued", "running", "success", "failed", "cancelled",
                "skipped")
JOB_STATUSES = ("running", "completed", "failed")
ACTIVE_STATUSES = ("leased", "running", "cancelling")
ENDED_STATUSES = {"failed": "failed", "expired": "failed",
                  "cancelled": "cancelled"}
EVENTS = ("push", "schedule", "workflow_dispatch", "issues",
          "issue_comment", "pull_request")
INPUT_TYPES = ("string", "choice", "boolean")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPO_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
WORKFLOW_PATH_RE = re.compile(r"^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$")
INPUT_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]{0,99}$")
SECRET_NAME_RE = re.compile(r"^[A-Z_][A-Z0-9_]{0,63}$")
PUBLIC_RUN_FIELDS = ("id", "workflow", "workflowPath", "event", "commit", "ref",
                     "status", "createdAt", "startedAt", "finishedAt", "actor")
_ACTIVE_SQL = "('leased','running','cancelling')"


def valid_branch(branch):
    branch = str(branch or "")
    return bool(
        BRANCH_RE.fullmatch(branch) and ".." not in branch
        and "//" not in branch and "@{" not in branch
        and not branch.endswith((".", "/", ".lock")) and "/." not in branch)


def token_hash(token):
    return hashlib.sha256(str(token or "").encode("utf-8")).hexdigest()


def _text(value, limit):
    text = "".join(
        c for c in str(value or "") if c == "\n" or c == "\t"
        or (c.isprintable() and c not in "\x00\r"))
    return text[:limit]


def _line(value, limit):
    return _text(value, limit).replace("\n", " ").replace("\t", " ").strip()


def _int(value):
    if isinstance(value, bool):
        return 0
    try:
        return max(0, int(value))
    except (TypeError, ValueError, OverflowError):
        return 0


def _log_tail(value):
    raw = str(value or "").replace("\x00", "").encode("utf-8")
    if len(raw) <= LOG_TAIL_MAX_BYTES:
        return raw.decode("utf-8")
    return raw[-LOG_TAIL_MAX_BYTES:].decode("utf-8", "ignore")


def _log_full(value):
    raw = str(value or "").replace("\x00", "").encode("utf-8")
    if len(raw) <= LOG_MAX_BYTES:
        return raw.decode("utf-8")
    head, tail = LOG_HEAD_BYTES, LOG_MAX_BYTES - LOG_HEAD_BYTES
    return (raw[:head].decode("utf-8", "ignore")
            + "\n[%d bytes omitted]\n" % (len(raw) - head - tail)
            + raw[-tail:].decode("utf-8", "ignore"))


def mask(text, values):
    text = str(text or "")
    pieces = set()
    for value in values or ():
        value = str(value or "")
        for piece in [value] + value.splitlines():
            if len(piece.encode("utf-8")) >= MASK_MIN_BYTES:
                pieces.add(piece)
    for piece in sorted(pieces, key=len, reverse=True):
        text = text.replace(piece, "***")
    return text


def _json(value, fallback):
    try:
        parsed = json.loads(value) if isinstance(value, str) else value
    except ValueError:
        return fallback
    return parsed if isinstance(parsed, type(fallback)) else fallback


async def _open(runtime, value):
    if not value:
        return {}
    try:
        record = await runtime.open(value)
    except Exception:
        return {}
    return record if isinstance(record, dict) else {}


async def owner_has_runner(runtime, owner):
    rows = await runtime.d1_all(
        "SELECT allowed_owners FROM runner_nodes WHERE status='active'")
    for row in rows or []:
        try:
            owners = json.loads(row.get("allowed_owners") or "[]")
        except ValueError:
            continue
        if isinstance(owners, list) and owner in owners:
            return True
    return False


async def identity(runtime, owner, repo):
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    if not NAME_RE.fullmatch(owner) or not REPO_RE.fullmatch(repo):
        return owner, owner
    try:
        rows = await runtime.d1_all(
            "SELECT r.node_owner AS node_owner,o.name AS name FROM org_repos r "
            "JOIN orgs o ON o.org_bi=r.org_bi WHERE r.repo=? "
            "AND (r.node_owner=? OR o.name=?) LIMIT 4",
            repo.lower(), owner, owner)
    except Exception:
        rows = []
    pairs = {(str(row.get("node_owner") or "").strip().lower(),
              str(row.get("name") or "").strip().lower())
             for row in rows or []}
    if len(pairs) == 1:
        storage, public = pairs.pop()
        if NAME_RE.fullmatch(storage) and NAME_RE.fullmatch(public):
            return storage, public
    return owner, owner


async def _actions_off(runtime, owner, repo):
    check = getattr(runtime, "actions_off", None)
    return bool(check is not None and await check(owner, repo))


async def _push_protected(runtime, owner, repo, branch):
    check = getattr(runtime, "branch_protected", None)
    return bool(check is not None and await check(owner, repo, branch))


async def enqueue(runtime, owner, repo, branch, commit="", node="",
                  pusher="", event="push", workflow="", payload=None,
                  before="", trusted=True, ref="", inputs=None):
    repo = str(repo or "").strip()
    owner, _public = await identity(runtime, owner, repo)
    branch = str(branch or "").strip()
    commit = str(commit or "").strip().lower()
    node = str(node or "").strip().lower()
    event = str(event or "push").strip()
    workflow = str(workflow or "").strip()
    before = str(before or "").strip().lower()
    if (not NAME_RE.fullmatch(owner) or not REPO_RE.fullmatch(repo)
            or not valid_branch(branch) or event not in EVENTS
            or (workflow and not WORKFLOW_PATH_RE.fullmatch(workflow))
            or (event != "push" and not workflow)):
        return 0
    if await _actions_off(runtime, owner, repo):
        return 0
    if commit and not OID_RE.fullmatch(commit):
        commit = ""
    if not OID_RE.fullmatch(before) or set(before) == {"0"}:
        before = ""
    if not NAME_RE.fullmatch(node):
        node = ""
    ref = str(ref or "") or "refs/heads/" + branch
    actor = _text(pusher, 64)
    now = runtime.now()
    sealed = ""
    if payload and len(json.dumps(payload).encode("utf-8")) > (
            PAYLOAD_MAX_BYTES):
        return 0
    if payload or inputs:
        sealed = await runtime.seal({
            "event": payload if isinstance(payload, dict) else {},
            "inputs": inputs if isinstance(inputs, dict) else {},
        })
    if event == "push" and not workflow:
        if commit and await runtime.d1_first(
                "SELECT id FROM repo_action_jobs WHERE owner=? AND repo=? "
                "AND ((commit_oid=? AND event='push' AND workflow='') "
                "OR pushed_commit=?) LIMIT 1", owner, repo, commit, commit):
            return 0
        queued = await runtime.d1_first(
            "SELECT id FROM repo_action_jobs WHERE owner=? AND repo=? "
            "AND branch=? AND event='push' AND workflow='' "
            "AND status='queued' ORDER BY id DESC LIMIT 1",
            owner, repo, branch)
        if queued:
            await runtime.d1_run(
                "UPDATE repo_action_jobs SET commit_oid=?,target_node=?,"
                "pusher=?,actor=?,updated_at=? WHERE id=? AND status='queued'",
                commit, node, actor, actor, now, queued["id"])
            return int(queued["id"])
    await runtime.d1_run(
        "INSERT INTO repo_action_jobs (owner,repo,branch,commit_oid,"
        "target_node,pusher,status,created_at,updated_at,event,workflow,ref,"
        "actor,before_oid,payload,trusted) "
        "VALUES (?,?,?,?,?,?,'queued',?,?,?,?,?,?,?,?,?)",
        owner, repo, branch, commit, node, actor, now, now, event, workflow,
        ref, actor, before, sealed, 1 if trusted else 0)
    row = await runtime.d1_first(
        "SELECT id FROM repo_action_jobs WHERE owner=? AND repo=? "
        "AND event=? AND workflow=? AND status='queued' "
        "ORDER BY id DESC LIMIT 1", owner, repo, event, workflow)
    return int((row or {}).get("id") or 0)


async def enqueue_catalog(runtime, owner, repo, event, branch, payload,
                          actor="", trusted=True):
    repo = str(repo or "").strip()
    owner, _public = await identity(runtime, owner, repo)
    rows = await runtime.d1_all(
        "SELECT path,events FROM repo_action_workflows WHERE owner=? "
        "AND repo=? ORDER BY path", owner, repo)
    queued = []
    for row in rows or []:
        if event not in _json(row.get("events"), []):
            continue
        job = await enqueue(
            runtime, owner, repo, branch, "", "", actor, event=event,
            workflow=str(row.get("path") or ""), payload=payload,
            trusted=trusted)
        if job:
            queued.append(job)
    return queued


async def sweep(runtime, now):
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status='cancelled',lease_id='',"
        "token_hash='',updated_at=? WHERE status='cancelling' "
        "AND expires_at<?", now, now)
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status='failed',lease_id='',"
        "token_hash='',"
        "note='The runner did not finish this run in time.',updated_at=? "
        "WHERE status IN ('leased','running') AND expires_at<? "
        "AND attempts>=?", now, now, MAX_ATTEMPTS)
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status='queued',lease_id='',runner='',"
        "token_hash='',expires_at=0,updated_at=? "
        "WHERE status IN ('leased','running') AND expires_at<?", now, now)
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status='expired',"
        "note='No runner took this job within a day.',updated_at=? "
        "WHERE status='queued' AND created_at<?", now, now - QUEUED_TTL_MS)
    await runtime.d1_run(
        "DELETE FROM repo_action_logs WHERE job_id IN (SELECT id FROM "
        "repo_action_jobs WHERE updated_at<? AND status NOT IN "
        "('queued','leased','running','cancelling'))", now - KEEP_MS)
    await runtime.d1_run(
        "DELETE FROM repo_action_jobs WHERE updated_at<? AND status NOT IN "
        "('queued','leased','running','cancelling')", now - KEEP_MS)


async def _target(runtime, preferred, pool):
    for name in ([preferred] if preferred in pool else []) + sorted(pool):
        base = await runtime.gateway_url(name)
        if base:
            return name, base
    return "", ""


async def secrets_for(runtime, owner, repo, organization):
    rows = await runtime.d1_all(
        "SELECT owner,repo,name,value FROM repo_action_secrets "
        "WHERE (owner IN (?,?) AND repo=?) OR (owner=? AND repo='')",
        owner, organization, repo, organization)
    shared, linked, own = {}, {}, {}
    for row in rows or []:
        record = await _open(runtime, row.get("value"))
        value = record.get("value")
        if not isinstance(value, str) or not SECRET_NAME_RE.fullmatch(
                str(row.get("name") or "")):
            continue
        if not row.get("repo"):
            shared[str(row["name"])] = value
        elif row.get("owner") == owner:
            own[str(row["name"])] = value
        else:
            linked[str(row["name"])] = value
    merged = {**shared, **linked, **own}
    return dict(sorted(merged.items())[:SECRET_LIMIT])


def _origin_host(origin):
    text = str(origin or "")
    if "://" in text:
        text = text.split("://", 1)[1]
    return text.split("/", 1)[0].split(":", 1)[0].lower()


def _event_payload(row, record, full_name):
    event = str(row.get("event") or "push")
    if event == "push" and not record.get("event"):
        return {
            "ref": str(row.get("ref") or ""),
            "before": str(row.get("before_oid") or ""),
            "after": str(row.get("commit_oid") or ""),
            "repository": {"full_name": full_name},
            "pusher": {"name": str(row.get("actor") or "")},
        }
    payload = dict(record.get("event") or {})
    payload.setdefault("repository", {"full_name": full_name})
    return payload


async def lease(runtime, runner, requested, maximum):
    import runner_leases
    now = runtime.now()
    await sweep(runtime, now)
    pool = runtime.pool_targets()
    if not pool or maximum <= 0:
        return []
    rows = await runtime.d1_all(
        "SELECT id,owner,repo,branch,commit_oid,target_node,attempts,event,"
        "workflow,ref,actor,before_oid,payload,trusted "
        "FROM repo_action_jobs WHERE status='queued' ORDER BY id LIMIT %d"
        % SCAN_LIMIT)
    jobs = []
    for row in rows or []:
        if len(jobs) >= maximum:
            break
        job_id = int(row.get("id") or 0)
        owner = str(row.get("owner") or "")
        repo = str(row.get("repo") or "")
        branch = str(row.get("branch") or "")
        event = str(row.get("event") or "push")
        workflow = str(row.get("workflow") or "")
        owner, public_owner = await identity(runtime, owner, repo)
        if await _actions_off(runtime, owner, repo):
            await runtime.d1_run(
                "UPDATE repo_action_jobs SET status='cancelled',updated_at=? "
                "WHERE id=? AND status='queued'", now, job_id)
            continue
        if not (runner_leases._owner_allowed(runner, public_owner, requested)
                or runner_leases._owner_allowed(runner, owner, requested)):
            continue
        target, base = await _target(
            runtime, str(row.get("target_node") or ""), pool)
        if not target:
            continue
        trusted = bool(
            _int(row.get("trusted")) and runner.get("actionsTrusted")
            and event != "pull_request")
        lease_id = runtime.new_id()
        expires_at = now + JOB_TIMEOUT_MS
        read_grant = await runner_leases._grant(
            runtime, target, repo, "read", "", runner["name"],
            runtime.new_id(), now, expires_at, owner=public_owner)
        if not read_grant:
            continue
        push_grant = ""
        token = ""
        if trusted and not await _push_protected(
                runtime, owner, repo, branch):
            push_grant = await runner_leases._grant(
                runtime, target, repo, "actions-push", "refs/heads/" + branch,
                runner["name"], runtime.new_id(), now, expires_at,
                owner=public_owner)
        if trusted:
            token = runtime.new_id() + runtime.new_id()
        await runtime.d1_run(
            "UPDATE repo_action_jobs SET status='leased',lease_id=?,runner=?,"
            "target_node=?,expires_at=?,attempts=attempts+1,updated_at=?,"
            "token_hash=? WHERE id=? AND status='queued' AND NOT EXISTS "
            "(SELECT 1 FROM repo_action_jobs active WHERE active.owner=? "
            "AND active.repo=? AND active.workflow=? AND active.status IN "
            + _ACTIVE_SQL + ")",
            lease_id, runner["name"], target, expires_at, now,
            token_hash(token) if token else "", job_id, owner, repo,
            workflow)
        claimed = await runtime.d1_first(
            "SELECT id FROM repo_action_jobs WHERE id=? AND lease_id=? "
            "AND status='leased'", job_id, lease_id)
        if not claimed:
            continue
        record = await _open(runtime, row.get("payload"))
        if token:
            await runtime.d1_run(
                "UPDATE repo_action_jobs SET payload=? WHERE id=?",
                await runtime.seal({**record, "token": token}), job_id)
        full_name = public_owner + "/" + repo
        job = {
            "kind": "actions",
            "jobId": job_id,
            "leaseId": lease_id,
            "repositoryOwner": public_owner,
            "repositoryName": repo,
            "organization": public_owner,
            "branch": branch,
            "commit": str(row.get("commit_oid") or ""),
            "cloneUrl": (base.rstrip("/") + "/v1/runner/" + public_owner
                         + "/" + repo + ".git"),
            "readGrant": read_grant,
            "grantExpiresAt": expires_at,
            "timeoutMs": JOB_TIMEOUT_MS,
            "event": event,
            "workflow": workflow,
            "ref": str(row.get("ref") or "") or "refs/heads/" + branch,
            "before": str(row.get("before_oid") or ""),
            "actor": str(row.get("actor") or ""),
            "eventPayload": _event_payload(row, record, full_name),
            "inputs": dict(record.get("inputs") or {}),
            "trusted": trusted,
        }
        if trusted:
            origin = runtime.public_origin()
            bot = public_owner + "-actions"
            job.update({
                "secrets": await secrets_for(runtime, owner, repo,
                                             public_owner),
                "jobToken": token,
                "apiUrl": origin,
                "botName": bot,
                "botEmail": bot + "@users.noreply." + (
                    _origin_host(origin) or "localhost"),
            })
            if push_grant:
                job["pushGrant"] = push_grant
                job["pushRef"] = "refs/heads/" + branch
        jobs.append(job)
    return jobs


def _runs(job_id, value, now, masks=(), default_event="push"):
    runs = []
    logs = {}
    for index, item in enumerate(value if isinstance(value, list) else []):
        if len(runs) >= RUNS_PER_JOB:
            break
        if not isinstance(item, dict):
            continue
        workflow = _line(item.get("workflow"), 160)
        commit = str(item.get("commit") or "").strip().lower()
        ref = _line(item.get("ref"), 160)
        status = str(item.get("status") or "")
        if (not workflow or not OID_RE.fullmatch(commit) or not ref
                or status not in RUN_STATUSES):
            continue
        event = str(item.get("event") or default_event)
        path = str(item.get("workflowPath") or "")
        run = {
            "id": job_id * RUN_ID_STRIDE + index + 1,
            "workflow": workflow,
            "commit": commit,
            "ref": ref,
            "status": status,
            "createdAt": min(_int(item.get("createdAt")), now),
            "startedAt": min(_int(item.get("startedAt")), now),
            "finishedAt": min(_int(item.get("finishedAt")), now),
            "logTail": _log_tail(mask(item.get("logTail"), masks)),
            "event": event if event in EVENTS else default_event,
        }
        if WORKFLOW_PATH_RE.fullmatch(path):
            run["workflowPath"] = path
        runs.append(run)
        if isinstance(item.get("log"), str):
            logs[index] = _log_full(mask(item["log"], masks))
    return runs, logs


def _clean_input(item):
    if not isinstance(item, dict):
        return None
    name = str(item.get("name") or "")
    if not INPUT_NAME_RE.fullmatch(name):
        return None
    kind = str(item.get("type") or "string")
    if kind not in INPUT_TYPES:
        kind = "string"
    options = item.get("options")
    options = [_line(o, 200) for o in options[:OPTION_LIMIT]
               if isinstance(o, (str, int, float)) and not isinstance(
                   o, bool)] if isinstance(options, list) else []
    default = item.get("default")
    if isinstance(default, bool):
        default = "true" if default else "false"
    return {
        "name": name,
        "description": _line(item.get("description"), 300),
        "required": item.get("required") is True,
        "default": _text(default if default is not None else "",
                         INPUT_VALUE_MAX),
        "type": kind,
        "options": options if kind == "choice" else [],
    }


def _input_items(value):
    if isinstance(value, dict):
        return [dict(spec, name=name) for name, spec in value.items()
                if isinstance(spec, dict)]
    return value if isinstance(value, list) else []


def clean_catalog(value):
    import actions_cron
    catalog = []
    seen = set()
    for item in value if isinstance(value, list) else []:
        if len(catalog) >= CATALOG_LIMIT:
            break
        if not isinstance(item, dict):
            continue
        path = str(item.get("path") or "")
        if not WORKFLOW_PATH_RE.fullmatch(path) or path in seen:
            continue
        seen.add(path)
        events = [e for e in EVENTS if e in (item.get("events") or [])]
        schedules = [
            _line(s, actions_cron.MAX_EXPRESSION)
            for s in (item.get("schedules") or [])
            if isinstance(s, str) and actions_cron.valid(s)
        ][:actions_cron.MAX_SCHEDULES]
        inputs = []
        for spec in _input_items(item.get("inputs"))[:INPUT_LIMIT]:
            cleaned = _clean_input(spec)
            if cleaned and cleaned["name"] not in [i["name"] for i in inputs]:
                inputs.append(cleaned)
        catalog.append({
            "path": path,
            "name": _line(item.get("name"), 160),
            "events": events,
            "schedules": schedules,
            "inputs": inputs,
        })
    return catalog


async def store_catalog(runtime, owner, repo, catalog, commit, now):
    owner, _public = await identity(runtime, owner, repo)
    paths = [item["path"] for item in catalog]
    if paths:
        await runtime.d1_run(
            "DELETE FROM repo_action_workflows WHERE owner=? AND repo=? "
            "AND path NOT IN (%s)" % ",".join("?" * len(paths)),
            owner, repo, *paths)
    else:
        await runtime.d1_run(
            "DELETE FROM repo_action_workflows WHERE owner=? AND repo=?",
            owner, repo)
    for item in catalog:
        await runtime.d1_run(
            "INSERT INTO repo_action_workflows (owner,repo,path,name,events,"
            "schedules,inputs,commit_oid,last_slot,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?,0,?) ON CONFLICT(owner,repo,path) DO "
            "UPDATE SET name=excluded.name,events=excluded.events,"
            "schedules=excluded.schedules,inputs=excluded.inputs,"
            "commit_oid=excluded.commit_oid,updated_at=excluded.updated_at",
            owner, repo, item["path"], item["name"],
            json.dumps(item["events"]), json.dumps(item["schedules"]),
            json.dumps(item["inputs"], sort_keys=True), commit, now)


async def catalog(runtime, owner, repo):
    owner, _public = await identity(runtime, owner, repo)
    rows = await runtime.d1_all(
        "SELECT path,name,events,schedules,inputs FROM repo_action_workflows "
        "WHERE owner=? AND repo=? ORDER BY path LIMIT %d" % CATALOG_LIMIT,
        owner, repo)
    return [{
        "path": str(row.get("path") or ""),
        "name": str(row.get("name") or ""),
        "events": _json(row.get("events"), []),
        "schedules": _json(row.get("schedules"), []),
        "inputs": [i for i in _input_items(_json(row.get("inputs"), []))
                   if isinstance(i, dict)],
    } for row in rows or []]


async def report(runtime, runner, data):
    job_id = _int(data.get("jobId"))
    lease_id = str(data.get("leaseId") or "")[:64]
    row = await runtime.d1_first(
        "SELECT id,owner,repo,branch,status,event,payload,target_node,"
        "token_hash FROM "
        "repo_action_jobs WHERE id=? AND runner=? AND lease_id=? "
        "AND lease_id!='' AND status IN " + _ACTIVE_SQL,
        job_id, runner["name"], lease_id) if job_id and lease_id else None
    if not row:
        return runtime.response({
            "error": "lease_not_found",
            "message": "This runner does not hold that Actions job's lease.",
        }, 409)
    status = str(data.get("status") or "").lower()
    if status not in JOB_STATUSES:
        return runtime.response({"error": "invalid_status"}, 400)
    now = runtime.now()
    repo = str(row["repo"])
    owner, public = await identity(runtime, str(row["owner"]), repo)
    cancelling = row.get("status") == "cancelling"
    terminal = status != "running"
    record = await _open(runtime, row.get("payload"))
    masks = []
    if record.get("token"):
        masks.append(record["token"])
    secrets = await secrets_for(runtime, owner, repo, public)
    masks.extend(secrets.values())
    commit = str(data.get("commit") or "").strip().lower()
    runs, logs = _runs(job_id, data.get("runs"), now, masks,
                       str(row.get("event") or "push"))
    stored = {
        "runs": runs,
        "commit": commit if OID_RE.fullmatch(commit) else "",
    }
    for index, text in logs.items():
        await runtime.d1_run(
            "INSERT INTO repo_action_logs (job_id,run_index,data,updated_at) "
            "VALUES (?,?,?,?) ON CONFLICT(job_id,run_index) DO UPDATE SET "
            "data=excluded.data,updated_at=excluded.updated_at",
            job_id, index, await runtime.seal({"log": text}), now)
    if isinstance(data.get("catalog"), list) and str(
            row.get("branch") or "") == await runtime.default_branch(
                owner, repo):
        await store_catalog(runtime, owner, repo,
                            clean_catalog(data["catalog"]),
                            stored["commit"], now)
    pushed = str(data.get("pushedCommit") or "").strip().lower()
    if not OID_RE.fullmatch(pushed):
        pushed = ""
    note = _text(mask(data.get("message"), masks), 300)
    sealed = await runtime.seal(stored)
    if cancelling:
        status = "cancelled" if terminal else "cancelling"
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status=?,data=?,note=?,commit_oid="
        "CASE WHEN ?='' THEN commit_oid ELSE ? END,lease_id=?,updated_at=?,"
        "pushed_commit=CASE WHEN ?='' THEN pushed_commit ELSE ? END,"
        "token_hash=CASE WHEN ? THEN '' ELSE token_hash END "
        "WHERE id=? AND lease_id=?",
        status, sealed, note, stored["commit"], stored["commit"],
        "" if terminal else lease_id, now, pushed, pushed,
        1 if terminal else 0, job_id, lease_id)
    if terminal and row.get("token_hash"):
        await _admit_pushed_state(runtime, owner, repo,
                                  str(row.get("target_node") or ""))
    return runtime.response({"ok": True, "cancel": cancelling})


async def _admit_pushed_state(runtime, owner, repo, node):
    admit = getattr(runtime, "admit_push_state", None)
    if not callable(admit) or not node:
        return ""
    try:
        return await admit(owner, repo, node) or ""
    except Exception:
        return ""


async def pushing(runtime, owner, repo, node):
    node = str(node or "").strip().lower()
    repo = str(repo or "").strip()
    if not NAME_RE.fullmatch(node) or not REPO_RE.fullmatch(repo):
        return False
    owner, _public = await identity(runtime, owner, repo)
    if not NAME_RE.fullmatch(owner):
        return False
    row = await runtime.d1_first(
        "SELECT id FROM repo_action_jobs WHERE owner=? AND lower(repo)=? "
        "AND target_node=? AND token_hash!='' AND lease_id!='' "
        "AND expires_at>? AND status IN " + _ACTIVE_SQL + " LIMIT 1",
        owner, repo.lower(), node, runtime.now())
    return bool(row)


async def cancel(runtime, owner, repo, run_id):
    owner, _public = await identity(runtime, owner, repo)
    job_id = _int(run_id) // RUN_ID_STRIDE
    row = await runtime.d1_first(
        "SELECT id,status FROM repo_action_jobs WHERE id=? AND owner=? "
        "AND repo=?", job_id, owner, repo) if job_id else None
    if not row:
        return "run_not_found"
    now = runtime.now()
    if row["status"] == "queued":
        await runtime.d1_run(
            "UPDATE repo_action_jobs SET status='cancelled',updated_at=? "
            "WHERE id=? AND status='queued'", now, job_id)
        return ""
    if row["status"] in ("leased", "running"):
        await runtime.d1_run(
            "UPDATE repo_action_jobs SET status='cancelling',updated_at=? "
            "WHERE id=? AND status IN ('leased','running')", now, job_id)
        return ""
    return "not_cancellable"


async def rerun(runtime, owner, repo, run_id, actor):
    owner, _public = await identity(runtime, owner, repo)
    run_id = _int(run_id)
    job_id = run_id // RUN_ID_STRIDE
    row = await runtime.d1_first(
        "SELECT id,branch,commit_oid,status,event,workflow,ref,before_oid,"
        "payload,trusted,data FROM repo_action_jobs WHERE id=? AND owner=? "
        "AND repo=?", job_id, owner, repo) if job_id else None
    if not row:
        return 0, "run_not_found"
    if row["status"] in ("queued",) + ACTIVE_STATUSES:
        return 0, "run_active"
    workflow = str(row.get("workflow") or "")
    if not workflow and run_id % RUN_ID_STRIDE:
        stored = await _open(runtime, row.get("data"))
        for run in stored.get("runs") or []:
            if run.get("id") == run_id:
                workflow = str(run.get("workflowPath") or "")
    record = await _open(runtime, row.get("payload"))
    now = runtime.now()
    sealed = ""
    if record.get("event") or record.get("inputs"):
        sealed = await runtime.seal({
            "event": record.get("event") or {},
            "inputs": record.get("inputs") or {}})
    await runtime.d1_run(
        "INSERT INTO repo_action_jobs (owner,repo,branch,commit_oid,"
        "target_node,pusher,status,created_at,updated_at,event,workflow,ref,"
        "actor,before_oid,payload,trusted) "
        "VALUES (?,?,?,?,'',?,'queued',?,?,?,?,?,?,?,?,?)",
        owner, repo, row["branch"], str(row.get("commit_oid") or ""),
        _text(actor, 64), now, now, str(row.get("event") or "push"),
        workflow, str(row.get("ref") or ""), _text(actor, 64),
        str(row.get("before_oid") or ""), sealed, _int(row.get("trusted")))
    created = await runtime.d1_first(
        "SELECT id FROM repo_action_jobs WHERE owner=? AND repo=? "
        "AND status='queued' ORDER BY id DESC LIMIT 1", owner, repo)
    return int((created or {}).get("id") or 0), ""


def clean_inputs(spec, given):
    spec = {str(item.get("name")): item for item in _input_items(spec)}
    given = given if isinstance(given, dict) else {}
    unknown = sorted(set(given) - set(spec))
    if unknown:
        return None, "input_invalid", "Unknown input: " + str(unknown[0])[:100]
    values = {}
    for name, item in spec.items():
        value = given.get(name)
        kind = item.get("type") or "string"
        if isinstance(value, bool):
            value = "true" if value else "false"
        elif isinstance(value, (int, float)):
            value = str(value)
        elif value is not None and not isinstance(value, str):
            return None, "input_invalid", "Input %s is not text." % name
        if value is None or value == "":
            value = str(item.get("default") or "")
        if len(value) > INPUT_VALUE_MAX:
            return None, "input_invalid", "Input %s is too long." % name
        if item.get("required") and value == "":
            return None, "input_required", "Input %s is required." % name
        if kind == "boolean":
            value = value.lower() if value else "false"
            if value not in ("true", "false"):
                return None, "input_invalid", (
                    "Input %s must be true or false." % name)
        if kind == "choice" and value not in (item.get("options") or []):
            return None, "input_invalid", (
                "Input %s must be one of its options." % name)
        values[name] = value
    return values, "", ""


async def dispatch(runtime, owner, repo, workflow, ref, inputs, actor):
    ref = str(ref or "")
    branch = ref[len("refs/heads/"):] if ref.startswith("refs/heads/") else ref
    ref = "refs/heads/" + branch
    if not valid_branch(branch):
        return 0, "ref_not_found", "The branch is not valid."
    owner, _public = await identity(runtime, owner, repo)
    entry = next((item for item in await catalog(runtime, owner, repo)
                  if item["path"] == workflow), None)
    if entry is None:
        return 0, "workflow_not_found", "The workflow is not known yet."
    if "workflow_dispatch" not in entry["events"]:
        return 0, "not_manual", "The workflow has no workflow_dispatch."
    values, reason, error = clean_inputs(entry["inputs"], inputs)
    if values is None:
        return 0, reason, error
    job = await enqueue(
        runtime, owner, repo, branch, "", "", actor,
        event="workflow_dispatch", workflow=workflow,
        payload={"inputs": values, "ref": ref, "sender": {"login": actor}},
        inputs=values, ref=ref)
    if not job:
        return 0, "invalid_request", "The run could not be queued."
    return job, "", ""


async def run_log(runtime, owner, repo, run_id):
    owner, public = await identity(runtime, owner, repo)
    run_id = _int(run_id)
    job_id = run_id // RUN_ID_STRIDE
    index = run_id % RUN_ID_STRIDE - 1
    if not job_id or index < 0:
        return None
    if not await runtime.d1_first(
            "SELECT id FROM repo_action_jobs WHERE id=? AND owner=? "
            "AND repo=?", job_id, owner, repo):
        return None
    row = await runtime.d1_first(
        "SELECT data,updated_at FROM repo_action_logs WHERE job_id=? "
        "AND run_index=?", job_id, index)
    if not row:
        return {"log": "", "updatedAt": 0}
    record = await _open(runtime, row.get("data"))
    masks = (await secrets_for(runtime, owner, repo, public)).values()
    return {"log": mask(record.get("log"), masks),
            "updatedAt": int(row.get("updated_at") or 0)}


async def snapshot(runtime, owner, repo, always=False, masked=True):
    repo = str(repo or "").strip()
    owner, public = await identity(runtime, owner, repo)
    rows = await runtime.d1_all(
        "SELECT id,runner,status,commit_oid,branch,data,created_at,updated_at,"
        "event,workflow,ref,note,actor FROM repo_action_jobs WHERE owner=? "
        "AND repo=? ORDER BY id DESC LIMIT %d" % SNAPSHOT_JOBS, owner, repo)
    if not rows and not always and not await owner_has_runner(
            runtime, public) and not await owner_has_runner(runtime, owner):
        return None
    now = runtime.now()
    runs = []
    node = ""
    masks = ((await secrets_for(runtime, owner, repo, public)).values()
             if masked and rows else ())
    for row in rows:
        node = node or str(row.get("runner") or "")
        event = str(row.get("event") or "push")
        actor = _shown_actor(runtime, _line(row.get("actor"), 64))
        record = await _open(runtime, row.get("data"))
        stored = record.get("runs")
        if isinstance(stored, list) and stored:
            for run in stored:
                if isinstance(run, dict):
                    item = {"event": event, "actor": actor, **run}
                    if masks and item.get("logTail"):
                        item["logTail"] = mask(item["logTail"], masks)
                    runs.append(item)
        elif str(row.get("status") or "") in (
                ("queued",) + ACTIVE_STATUSES + tuple(ENDED_STATUSES)):
            commit = str(row.get("commit_oid") or "")
            ended = row["status"] in ENDED_STATUSES
            runs.append({
                "id": int(row["id"]) * RUN_ID_STRIDE,
                "workflow": str(row.get("workflow") or "")
                or ".forkmesh workflows",
                "commit": commit if OID_RE.fullmatch(commit) else "",
                "ref": str(row.get("ref") or "")
                or "refs/heads/" + str(row.get("branch") or ""),
                "status": ENDED_STATUSES[row["status"]] if ended
                else "running" if row["status"] in ACTIVE_STATUSES[1:]
                else "queued",
                "createdAt": int(row.get("created_at") or 0),
                "startedAt": 0,
                "finishedAt": int(row.get("updated_at") or 0) if ended else 0,
                "logTail": _log_tail(_line(row.get("note"), 300) + "\n")
                if ended and row.get("note") else "",
                "event": event,
                "actor": actor,
            })
        if len(runs) >= RUN_LIMIT:
            break
    local = _local_runner(runtime, node)
    return {
        "ok": True,
        "node": node or "runner",
        "local": local,
        "host": _site_host(runtime) if local else "",
        "updatedAt": now,
        "expiresAt": now + SNAPSHOT_TTL_MS,
        "runs": runs[:RUN_LIMIT],
        "source": "runner",
        "controlAvailable": bool(runtime.setting("ACTIONS_INSTANCE_RUNNER")),
        "workflows": await catalog(runtime, owner, repo),
    }


def _shown_actor(runtime, actor):
    node = runtime.setting("INSTANCE_FLAGSHIP_NODE")
    name = " ".join(runtime.setting("INSTANCE_NAME").split())[:60]
    if actor and node and name and actor.lower() == node.lower():
        return name
    return actor


def _site_host(runtime):
    origin = runtime.runner_app_url() if hasattr(runtime, "runner_app_url") else ""
    host = origin.split("://", 1)[-1].split("/", 1)[0]
    return host if re.fullmatch(r"[A-Za-z0-9.-]{1,253}(:[0-9]{1,5})?", host) else ""


def _local_runner(runtime, node):
    own = runtime.setting("ACTIONS_INSTANCE_RUNNER").lower()
    return bool(own) and (not node or node.lower() == own)


def public_summary(full):
    runs = [{key: run[key] for key in PUBLIC_RUN_FIELDS if key in run}
            for run in full.get("runs") or () if isinstance(run, dict)]
    workflows = [{"path": item.get("path", ""), "name": item.get("name", ""),
                  "events": list(item.get("events") or ())}
                 for item in full.get("workflows") or ()
                 if isinstance(item, dict)]
    return {
        "ok": True,
        "access": "public",
        "node": "",
        "local": bool(full.get("local")),
        "host": str(full.get("host") or "") if full.get("local") else "",
        "updatedAt": full.get("updatedAt", 0),
        "expiresAt": full.get("expiresAt", 0),
        "runs": runs,
        "source": "runner",
        "controlAvailable": False,
        "workflows": workflows,
    }
