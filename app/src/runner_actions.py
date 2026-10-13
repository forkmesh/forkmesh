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
SNAPSHOT_TTL_MS = 60 * 1000
RUN_STATUSES = ("queued", "running", "success", "failed", "cancelled",
                "skipped")
JOB_STATUSES = ("running", "completed", "failed")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPO_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")


def valid_branch(branch):
    branch = str(branch or "")
    return bool(
        BRANCH_RE.fullmatch(branch) and ".." not in branch
        and "//" not in branch and "@{" not in branch
        and not branch.endswith((".", "/", ".lock")) and "/." not in branch)


def _text(value, limit):
    text = "".join(
        c for c in str(value or "") if c == "\n" or c == "\t"
        or (c.isprintable() and c not in "\x00\r"))
    return text[:limit]


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


async def enqueue(runtime, owner, repo, branch, commit="", node="",
                  pusher=""):
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    branch = str(branch or "").strip()
    commit = str(commit or "").strip().lower()
    node = str(node or "").strip().lower()
    if (not NAME_RE.fullmatch(owner) or not REPO_RE.fullmatch(repo)
            or not valid_branch(branch)):
        return 0
    if commit and not OID_RE.fullmatch(commit):
        commit = ""
    if not NAME_RE.fullmatch(node):
        node = ""
    now = runtime.now()
    if commit and await runtime.d1_first(
            "SELECT id FROM repo_action_jobs WHERE owner=? AND repo=? "
            "AND commit_oid=? LIMIT 1", owner, repo, commit):
        return 0
    queued = await runtime.d1_first(
        "SELECT id FROM repo_action_jobs WHERE owner=? AND repo=? "
        "AND status='queued' ORDER BY id DESC LIMIT 1", owner, repo)
    if queued:
        await runtime.d1_run(
            "UPDATE repo_action_jobs SET branch=?,commit_oid=?,target_node=?,"
            "pusher=?,updated_at=? WHERE id=? AND status='queued'",
            branch, commit, node, _text(pusher, 64), now, queued["id"])
        return int(queued["id"])
    await runtime.d1_run(
        "INSERT INTO repo_action_jobs (owner,repo,branch,commit_oid,"
        "target_node,pusher,status,created_at,updated_at) "
        "VALUES (?,?,?,?,?,?,'queued',?,?)",
        owner, repo, branch, commit, node, _text(pusher, 64), now, now)
    row = await runtime.d1_first(
        "SELECT id FROM repo_action_jobs WHERE owner=? AND repo=? "
        "AND status='queued' ORDER BY id DESC LIMIT 1", owner, repo)
    return int((row or {}).get("id") or 0)


async def sweep(runtime, now):
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status='failed',lease_id='',"
        "note='The runner did not finish this run in time.',updated_at=? "
        "WHERE status IN ('leased','running') AND expires_at<? "
        "AND attempts>=?", now, now, MAX_ATTEMPTS)
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status='queued',lease_id='',runner='',"
        "expires_at=0,updated_at=? WHERE status IN ('leased','running') "
        "AND expires_at<?", now, now)
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status='expired',"
        "note='No runner took this push within a day.',updated_at=? "
        "WHERE status='queued' AND created_at<?", now, now - QUEUED_TTL_MS)
    await runtime.d1_run(
        "DELETE FROM repo_action_jobs WHERE updated_at<? AND status NOT IN "
        "('queued','leased','running')", now - KEEP_MS)


async def _target(runtime, preferred, pool):
    for name in ([preferred] if preferred in pool else []) + sorted(pool):
        base = await runtime.gateway_url(name)
        if base:
            return name, base
    return "", ""


async def lease(runtime, runner, requested, maximum):
    import runner_leases
    now = runtime.now()
    await sweep(runtime, now)
    pool = runtime.pool_targets()
    if not pool or maximum <= 0:
        return []
    rows = await runtime.d1_all(
        "SELECT id,owner,repo,branch,commit_oid,target_node,attempts "
        "FROM repo_action_jobs WHERE status='queued' ORDER BY id LIMIT %d"
        % SCAN_LIMIT)
    jobs = []
    for row in rows or []:
        if len(jobs) >= maximum:
            break
        job_id = int(row.get("id") or 0)
        owner = str(row.get("owner") or "")
        repo = str(row.get("repo") or "")
        if not runner_leases._owner_allowed(runner, owner, requested):
            continue
        target, base = await _target(
            runtime, str(row.get("target_node") or ""), pool)
        if not target:
            continue
        lease_id = runtime.new_id()
        expires_at = now + JOB_TIMEOUT_MS
        read_grant = await runner_leases._grant(
            runtime, target, repo, "read", "", runner["name"],
            runtime.new_id(), now, expires_at, owner=owner)
        if not read_grant:
            continue
        await runtime.d1_run(
            "UPDATE repo_action_jobs SET status='leased',lease_id=?,runner=?,"
            "target_node=?,expires_at=?,attempts=attempts+1,updated_at=? "
            "WHERE id=? AND status='queued' AND NOT EXISTS (SELECT 1 FROM "
            "repo_action_jobs active WHERE active.owner=? AND active.repo=? "
            "AND active.status IN ('leased','running'))",
            lease_id, runner["name"], target, expires_at, now, job_id, owner,
            repo)
        claimed = await runtime.d1_first(
            "SELECT id FROM repo_action_jobs WHERE id=? AND lease_id=? "
            "AND status='leased'", job_id, lease_id)
        if not claimed:
            continue
        jobs.append({
            "kind": "actions",
            "jobId": job_id,
            "leaseId": lease_id,
            "repositoryOwner": owner,
            "repositoryName": repo,
            "organization": owner,
            "branch": str(row.get("branch") or ""),
            "commit": str(row.get("commit_oid") or ""),
            "cloneUrl": (base.rstrip("/") + "/v1/runner/" + owner + "/"
                         + repo + ".git"),
            "readGrant": read_grant,
            "grantExpiresAt": expires_at,
            "timeoutMs": JOB_TIMEOUT_MS,
        })
    return jobs


def _runs(job_id, value, now):
    runs = []
    for index, item in enumerate(value if isinstance(value, list) else []):
        if len(runs) >= RUNS_PER_JOB:
            break
        if not isinstance(item, dict):
            continue
        workflow = _text(item.get("workflow"), 160).replace("\n", " ").strip()
        commit = str(item.get("commit") or "").strip().lower()
        ref = _text(item.get("ref"), 160).replace("\n", " ").strip()
        status = str(item.get("status") or "")
        if (not workflow or not OID_RE.fullmatch(commit) or not ref
                or status not in RUN_STATUSES):
            continue
        runs.append({
            "id": job_id * RUN_ID_STRIDE + index + 1,
            "workflow": workflow,
            "commit": commit,
            "ref": ref,
            "status": status,
            "createdAt": min(_int(item.get("createdAt")), now),
            "startedAt": min(_int(item.get("startedAt")), now),
            "finishedAt": min(_int(item.get("finishedAt")), now),
            "logTail": _log_tail(item.get("logTail")),
        })
    return runs


async def report(runtime, runner, data):
    job_id = _int(data.get("jobId"))
    lease_id = str(data.get("leaseId") or "")[:64]
    row = await runtime.d1_first(
        "SELECT id,owner,repo FROM repo_action_jobs WHERE id=? AND runner=? "
        "AND lease_id=? AND lease_id!='' AND status IN ('leased','running')",
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
    commit = str(data.get("commit") or "").strip().lower()
    record = {
        "runs": _runs(job_id, data.get("runs"), now),
        "commit": commit if OID_RE.fullmatch(commit) else "",
    }
    note = _text(data.get("message"), 300)
    sealed = await runtime.seal(record)
    terminal = status != "running"
    await runtime.d1_run(
        "UPDATE repo_action_jobs SET status=?,data=?,note=?,commit_oid="
        "CASE WHEN ?='' THEN commit_oid ELSE ? END,lease_id=?,updated_at=? "
        "WHERE id=? AND lease_id=?",
        status, sealed, note, record["commit"], record["commit"],
        "" if terminal else lease_id, now, job_id, lease_id)
    return runtime.response({"ok": True})


async def snapshot(runtime, owner, repo):
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    rows = await runtime.d1_all(
        "SELECT id,runner,status,commit_oid,branch,data,created_at,updated_at "
        "FROM repo_action_jobs WHERE owner=? AND repo=? ORDER BY id DESC "
        "LIMIT %d" % SNAPSHOT_JOBS, owner, repo)
    if not rows and not await owner_has_runner(runtime, owner):
        return None
    now = runtime.now()
    runs = []
    node = ""
    for row in rows:
        node = node or str(row.get("runner") or "")
        record = None
        if row.get("data"):
            try:
                record = await runtime.open(row.get("data"))
            except Exception:
                record = None
        stored = record.get("runs") if isinstance(record, dict) else None
        if isinstance(stored, list) and stored:
            runs.extend(stored)
        elif (str(row.get("status") or "") in ("queued", "leased", "running")
              and OID_RE.fullmatch(str(row.get("commit_oid") or ""))):
            runs.append({
                "id": int(row["id"]) * RUN_ID_STRIDE,
                "workflow": ".forkmesh workflows",
                "commit": str(row["commit_oid"]),
                "ref": "refs/heads/" + str(row.get("branch") or ""),
                "status": "running" if row["status"] == "running" else "queued",
                "createdAt": int(row.get("created_at") or 0),
                "startedAt": 0,
                "finishedAt": 0,
                "logTail": "",
            })
        if len(runs) >= RUN_LIMIT:
            break
    return {
        "ok": True,
        "node": node or "runner",
        "updatedAt": now,
        "expiresAt": now + SNAPSHOT_TTL_MS,
        "runs": runs[:RUN_LIMIT],
        "source": "runner",
        "controlAvailable": False,
    }
