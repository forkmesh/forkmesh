"""Phase 5 runner tier: agent-job leases for machines that hold no repositories.

A *runner* only executes website agent jobs. It keeps no repositories, runs
no gateway, and publishes nothing. For each job it is handed two router-signed
grants (``edge_routing.runner_grant_message``): a read grant to clone the one
repository from its core node's gateway, and a push grant for exactly one
``refs/heads/agent/<...>`` branch of that repository. The gateway enforces the
ref scope; this module decides who gets which job.

Endpoints (all JSON, ``Cache-Control: no-store``):

``POST /api/runners/register``
    Account-session authenticated. Body ``{name, publicKey, scope,
    allowedOwners, ts, proof}`` where ``proof`` is the runner key's Ed25519
    signature over ``forkmesh-runner-register-v1\\n<name>\\n<publicKey>\\n
    <registering account>\\n<ts>`` — the registrant proves it holds the
    runner's key. Registration is deliberately the simplest possible choice
    (no link code): an app admin may register ``pool`` runners for any
    non-empty owner list (an owner is served only once listed: the opt-in);
    every account, admins included, may register only a
    ``personal`` runner whose ``allowedOwners`` is exactly its own name. A name
    registered to someone else, an existing account/claimed node name, or a
    configured pool target name is refused. Re-registering your own name
    rotates its key and settings.

``POST /api/runners/revoke``
    Account-session authenticated, body ``{name}``; the registrant (or an
    admin, for a pool runner) sets the runner's status to ``revoked``. Its
    signatures stop verifying at once; its leases requeue when their grants
    expire.

``POST /api/runners/claim``
    Runner-signed (see ``_signed_caller``). Body ``{max, providers,
    allowedOwners}``. Leases up to ``max`` (1..4) queued ``org_agent_jobs``
    rows the runner may serve and answers each with its grants.

``POST /api/runners/report``
    Runner-signed. Same body as the node result route plus ``jobId``; applied
    through ``route_handlers._apply_org_agent_job_result`` only when the
    caller holds that job's current runner lease.

``POST /api/runners/security-incident``
    Runner-signed. The runner's root IDS (deploy/core/ids.py, docs/IDS.md)
    reports an incident ``{severity, detector, summary, detail}``; it is
    recorded in ``security_incidents`` (the core hosts page the admins,
    forkmesh_host.ids_alerts). ``severity: "lockdown"`` also revokes the
    calling runner at once (its signatures stop verifying; a runner can only
    ever revoke itself this way). At most 30 incidents per runner per hour.

``POST /api/runners/pool-status``
    Signed by a *core* node named in ``RUNNER_POOL_TARGET_NODES`` with its
    claimed node key; the burst autoscaler's view of the pool queue.

``GET /api/runners``, ``/api/runners/poll``, ``/api/runners/login/*`` and
``/api/runners/<name>/login[/...]``
    Web sign-in of a runner's provider CLIs; see ``runner_logins.py``. The
    claim body may carry ``availability``/``activeLogins`` and its answer
    ``logins``/``fastPollMs`` (``runner_logins.runner_extras``).

Signed requests reuse the router's canonical per-request capability message
(``edge_routing.request_message``) with the caller's own node name and key,
the exact body's SHA-256, a single-use request id, and a 60 s window.

A job is eligible for runner R when all of these hold:

* its record's ``runner`` pin is empty or R's name (a pin to another runner
  is never eligible);
* its target node is R itself, or a configured pool target and R is a pool
  runner (a personal runner takes pool-target jobs only when pinned to it,
  or, for a ``steer``, when it held the session's latest lease);
* its owner (record ``organization``, else the target node) is allowed by R
  (personal: the registrant, who also requested the job; pool: listed) and
  by the ``allowedOwners`` the runner sent, if any;
* its provider is one the runner offered;
* ``start`` jobs get their named ``agent/`` branch or
  ``refs/heads/agent/<session>``; ``continue`` jobs only when their branch
  is under ``agent/``; ``steer`` jobs on the session's latest ref, by the
  runner that held that lease, or by any eligible runner once that one is
  revoked or unseen for ``STEER_HANDOVER_MS``;
* no other job of the same target node and repository is ``leased`` (the
  node route's FIFO invariant), enforced by a conditional queued->leased CAS
  and a re-read.

Lease rows are kept (with an empty lease id) after a terminal report so a
later ``steer`` can find the session's runner and ref.
"""

import base64
import hashlib
import json
import re

try:  # Worker: entry.py already imports it lazily; tests import it directly.
    import edge_routing
except Exception:  # pragma: no cover
    edge_routing = None


PREFIX = "/api/runners/"
MAX_BODY_BYTES = 64 * 1024
ACTIONS_REPORT_MAX_BYTES = 8 * 1024 * 1024
REQUEST_WINDOW_MS = 60_000
REGISTER_WINDOW_MS = 5 * 60 * 1000
MAX_CLAIM = 4
CLAIM_SCAN_LIMIT = 50
CLAIM_WAIT_MAX_MS = 20_000
CLAIM_WAIT_TICK_MS = 250
JOB_TIMEOUT_MS = 2 * 60 * 60 * 1000
STEER_HANDOVER_MS = 10 * 60 * 1000
RELEASED_LEASE_KEEP_MS = 30 * 24 * 60 * 60 * 1000
MAX_ALLOWED_OWNERS = 64
PROVIDERS = ("claude-code", "codex")
TERMINAL = ("completed", "failed", "rejected")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
PUBKEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
SIG_RE = re.compile(r"^[A-Za-z0-9_-]{86}$")
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{12,80}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
ATTACHMENT_ID_RE = re.compile(r"^[0-9a-f]{32}$")


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by the Worker adapter below."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _b64url(raw):
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def register_message(name, public_key, registered_by, ts):
    return "\n".join((
        "forkmesh-runner-register-v1", str(name), str(public_key),
        str(registered_by), str(ts)))


def _error(runtime, code, status, message=""):
    body = {"ok": False, "error": code}
    if message:
        body["message"] = message
    return runtime.response(body, status)


def _owners(value):
    if not isinstance(value, list):
        return None
    result = []
    for item in value[:MAX_ALLOWED_OWNERS + 1]:
        owner = str(item or "").strip().lower()
        if not NAME_RE.fullmatch(owner):
            return None
        if owner not in result:
            result.append(owner)
    return result if len(result) <= MAX_ALLOWED_OWNERS else None


def _json(raw):
    try:
        value = json.loads(raw.decode("utf-8")) if raw else {}
    except (UnicodeDecodeError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def _session_ref(session_id):
    tail = re.sub(r"[^A-Za-z0-9._-]+", "-", str(session_id or ""))
    tail = tail.strip("-.")[:120]
    ref = "refs/heads/agent/" + tail
    return ref if edge_routing.runner_agent_ref_valid(ref) else ""


async def handle(runtime, path):
    await runtime.ensure_schema()
    action = str(path or "")[len(PREFIX):].strip("/")
    if str(path or "").rstrip("/") == PREFIX.rstrip("/"):
        action = ""
    import runner_setup
    setup_route = runner_setup.match(action)
    if setup_route is not None:
        return await runner_setup.handle(runtime, setup_route)
    import runner_ssh_setup
    ssh_route = runner_ssh_setup.match(action)
    if ssh_route is not None:
        return await runner_ssh_setup.handle(runtime, ssh_route)
    # Web sign-in of a runner's provider CLI (runner_logins.py).
    import runner_logins
    login_route = runner_logins.match(action)
    if login_route is not None:
        return await runner_logins.handle(runtime, login_route)
    if action not in ("register", "revoke", "claim", "report", "pool-status",
                      "security-incident", "mail-log-note", "stream",
                      "attachment", "actions-report"):
        return _error(runtime, "not_found", 404)
    if runtime.method() != "POST":
        return _error(runtime, "method_not_allowed", 405)
    raw = await runtime.body_bytes(
        ACTIONS_REPORT_MAX_BYTES if action == "actions-report"
        else MAX_BODY_BYTES)
    if raw is None:
        return _error(runtime, "request_too_large", 413)
    if action == "register":
        return await _register(runtime, raw)
    if action == "revoke":
        return await _revoke(runtime, raw)
    if action == "pool-status":
        return await _pool_status(runtime, path, raw)
    runner = await _signed_caller(runtime, PREFIX + action, raw)
    if runner is None:
        return _error(runtime, "unauthorized", 401,
                      "The runner signature, time, or request id is invalid.")
    data = _json(raw)
    if data is None:
        return _error(runtime, "invalid_json", 400)
    if action == "claim":
        return await _claim(runtime, runner, data)
    if action == "security-incident":
        return await _security_incident(runtime, runner, data)
    if action == "mail-log-note":
        return await _mail_log_note(runtime, runner, data)
    if action == "stream":
        return await _stream(runtime, runner, data)
    if action == "attachment":
        return await _attachment(runtime, runner, data)
    if action == "actions-report":
        import runner_actions
        return await runner_actions.report(runtime, runner, data)
    return await _report(runtime, runner, data)


# --- registration ---------------------------------------------------------

async def _account(runtime, data):
    _bi, record = await runtime.session(data)
    return str((record or {}).get("name") or "").strip().lower()


async def _register(runtime, raw):
    data = _json(raw)
    if data is None:
        return _error(runtime, "invalid_json", 400)
    actor = await _account(runtime, data)
    if not actor:
        return _error(runtime, "unauthorized", 401)
    name = str(data.get("name") or "").strip().lower()
    public_key = str(data.get("publicKey") or "").strip()
    scope = str(data.get("scope") or "personal").strip().lower()
    owners = _owners(data.get("allowedOwners", []))
    proof = str(data.get("proof") or "").strip()
    try:
        ts = int(data.get("ts"))
    except (TypeError, ValueError):
        ts = 0
    if (not NAME_RE.fullmatch(name) or not PUBKEY_RE.fullmatch(public_key)
            or scope not in ("pool", "personal") or owners is None
            or not SIG_RE.fullmatch(proof) or ts <= 0):
        return _error(runtime, "invalid_registration", 400)
    now = runtime.now()
    if abs(now - ts) > REGISTER_WINDOW_MS:
        return _error(runtime, "stale_registration", 400)
    admin = await runtime.is_admin(actor)
    if scope == "pool" and not admin:
        return _error(runtime, "forbidden", 403,
                      "Only an administrator registers pool runners.")
    if scope == "personal" and owners != [actor]:
        return _error(runtime, "forbidden", 403,
                      "A personal runner may serve only your own account.")
    if not owners:
        # Pool runners too: a runner runs other people's code with its
        # provider login, so each owner it serves is listed explicitly.
        return _error(runtime, "invalid_registration", 400,
                      "List the owners this runner serves (allowedOwners).")

    message = register_message(name, public_key, actor, ts)
    if not await runtime.verify(public_key, proof, message.encode("utf-8")):
        return _error(runtime, "invalid_proof", 401)
    existing = await runtime.d1_first(
        "SELECT name,registered_by FROM runner_nodes WHERE name=?", name)
    if existing and existing.get("registered_by") != actor:
        return _error(runtime, "name_taken", 409)
    if not existing and (
            name in runtime.pool_targets() or await runtime.name_taken(name)):
        return _error(runtime, "name_taken", 409)
    await runtime.d1_run(
        "INSERT INTO runner_nodes (name,public_key,registered_by,scope,"
        "allowed_owners,status,created_at,updated_at,last_seen_at) "
        "VALUES (?,?,?,?,?,'active',?,?,0) ON CONFLICT(name) DO UPDATE SET "
        "public_key=excluded.public_key,scope=excluded.scope,"
        "allowed_owners=excluded.allowed_owners,status='active',"
        "updated_at=excluded.updated_at",
        name, public_key, actor, scope, json.dumps(owners), now, now)
    await runtime.audit(actor, "runner.register", name)
    return runtime.response({"ok": True, "runner": {
        "name": name, "scope": scope, "allowedOwners": owners,
        "publicKey": public_key}}, 201 if not existing else 200)


async def _revoke(runtime, raw):
    data = _json(raw)
    if data is None:
        return _error(runtime, "invalid_json", 400)
    actor = await _account(runtime, data)
    if not actor:
        return _error(runtime, "unauthorized", 401)
    name = str(data.get("name") or "").strip().lower()
    row = await runtime.d1_first(
        "SELECT registered_by,scope FROM runner_nodes WHERE name=?", name)
    if not row or not await _may_revoke(runtime, actor, row):
        return _error(runtime, "not_found", 404)
    await runtime.d1_run(
        "UPDATE runner_nodes SET status='revoked',updated_at=? WHERE name=?",
        runtime.now(), name)
    await runtime.audit(actor, "runner.revoke", name)
    return runtime.response({"ok": True, "name": name, "status": "revoked"})


async def _may_revoke(runtime, actor, row):
    if row.get("registered_by") == actor:
        return True
    return row.get("scope") == "pool" and await runtime.is_admin(actor)


# --- signed requests ------------------------------------------------------

async def _verify_signed(runtime, path, raw, keys_for):
    """Return the verified signer name, or "" (keys_for(name) -> keys)."""
    name = runtime.header("x-forkmesh-node").strip().lower()
    request_id = runtime.header("x-forkmesh-request-id").strip()
    body_sha = runtime.header("x-forkmesh-body-sha256").strip().lower()
    signature = runtime.header("x-forkmesh-signature").strip()
    try:
        issued_at = int(runtime.header("x-forkmesh-issued-at"))
    except (TypeError, ValueError):
        return ""
    now = runtime.now()
    if (not NAME_RE.fullmatch(name) or not REQUEST_ID_RE.fullmatch(request_id)
            or not SHA256_RE.fullmatch(body_sha) or not SIG_RE.fullmatch(signature)
            or abs(now - issued_at) > REQUEST_WINDOW_MS
            or hashlib.sha256(raw).hexdigest() != body_sha):
        return ""
    message = edge_routing.request_message(
        name, "POST", path, body_sha, request_id, issued_at)
    if not message:
        return ""
    verified = False
    for key in await keys_for(name):
        if key and await runtime.verify(key, signature, message.encode("utf-8")):
            verified = True
            break
    if not verified:
        return ""
    await runtime.d1_run(
        "DELETE FROM runner_request_nonces WHERE expires_at<?", now)
    result = await runtime.d1_run(
        "INSERT INTO runner_request_nonces (runner,request_id,expires_at) "
        "VALUES (?,?,?) ON CONFLICT(runner,request_id) DO NOTHING",
        name, request_id, issued_at + 2 * REQUEST_WINDOW_MS)
    if runtime.changes(result) != 1:
        return ""
    return name


async def _signed_caller(runtime, path, raw):
    rows = {}

    async def runner_keys(name):
        row = await runtime.d1_first(
            "SELECT name,public_key,scope,allowed_owners,status,registered_by,"
            "actions_trusted FROM runner_nodes WHERE name=? AND status='active'",
            name)
        if row:
            rows[name] = row
            return [str(row.get("public_key") or "")]
        return []

    name = await _verify_signed(runtime, path, raw, runner_keys)
    if not name:
        return None
    row = rows[name]
    try:
        owners = json.loads(row.get("allowed_owners") or "[]")
    except ValueError:
        owners = []
    return {"name": name, "scope": row.get("scope"),
            "publicKey": str(row.get("public_key") or ""),
            "registeredBy": str(row.get("registered_by") or ""),
            "allowedOwners": _owners(owners) or [],
            "actionsTrusted": bool(int(row.get("actions_trusted") or 0))}


# --- claim ----------------------------------------------------------------

async def _requeue_expired(runtime, now):
    expired = await runtime.d1_all(
        "SELECT job_id,lease_id FROM runner_job_leases "
        "WHERE lease_id!='' AND expires_at<?", now)
    for row in expired:
        await runtime.d1_run(
            "UPDATE org_agent_jobs SET status='queued',lease_id='',updated_at=? "
            "WHERE id=? AND lease_id=? AND status IN ('leased','running')",
            now, row["job_id"], row["lease_id"])
        await runtime.d1_run(
            "UPDATE runner_job_leases SET lease_id='',expires_at=0 "
            "WHERE job_id=? AND lease_id=?", row["job_id"], row["lease_id"])
    await runtime.d1_run(
        "DELETE FROM runner_job_leases WHERE lease_id='' AND created_at<?",
        now - RELEASED_LEASE_KEEP_MS)


async def _latest_session_lease(runtime, session_id):
    return await runtime.d1_first(
        "SELECT runner,ref FROM runner_job_leases WHERE session_id=? "
        "ORDER BY created_at DESC, job_id DESC LIMIT 1", session_id)


def _owner_allowed(runner, owner, requested):
    # Every runner, pool runners included, serves only the owners on its
    # own row: an owner opts in by being listed (audit 2026-09-24 H4). A
    # pool runner registered with no owners serves nobody.
    allowed = runner.get("allowedOwners") or []
    if owner not in allowed:
        return False
    if runner.get("scope") != "pool" and owner != runner.get("registeredBy"):
        return False
    return requested is None or owner in requested


def _requester_allowed(runner, record):
    if runner.get("scope") == "pool":
        return True
    requester = str(record.get("requestedBy") or "").strip().lower()
    return bool(requester) and requester == runner.get("registeredBy")


async def _job_ref(runtime, runner, record, session_id):
    """The one ref this job may push, or "" when a runner may not take it."""
    kind = str(record.get("kind") or "")
    if kind == "start":
        named = "refs/heads/" + str(record.get("branch") or "")
        if record.get("branch") and edge_routing.runner_agent_ref_valid(named):
            return named
        return _session_ref(session_id)
    if kind == "continue":
        ref = "refs/heads/" + str(record.get("branch") or "")
        return ref if edge_routing.runner_agent_ref_valid(ref) else ""
    if kind == "steer":
        prior = await _latest_session_lease(runtime, session_id)
        if not prior:
            return ""
        ref = str(prior.get("ref") or "")
        if prior.get("runner") == runner["name"]:
            return ref
        if (edge_routing.runner_agent_ref_valid(ref)
                and await _runner_gone(runtime, prior.get("runner"))):
            return ref
    return ""


async def _runner_gone(runtime, name):
    row = await runtime.d1_first(
        "SELECT status,last_seen_at FROM runner_nodes WHERE name=?",
        str(name or ""))
    if not row or str(row.get("status") or "") != "active":
        return True
    last_seen = int(row.get("last_seen_at") or 0)
    return runtime.now() - last_seen > STEER_HANDOVER_MS


async def _gateway_owner(runtime, target, repo):
    try:
        rows = await runtime.d1_all(
            "SELECT o.name AS name FROM org_repos r JOIN orgs o "
            "ON o.org_bi=r.org_bi WHERE r.node_owner=? AND r.repo=? LIMIT 2",
            target, str(repo or "").lower())
    except Exception:
        return target
    names = {str(row.get("name") or "").strip().lower() for row in rows or []}
    if len(names) == 1:
        name = names.pop()
        if NAME_RE.fullmatch(name):
            return name
    return target


async def _grant(runtime, target, repo, access, ref, runner, grant_id,
                 issued_at, expires_at, owner=""):
    message = edge_routing.runner_grant_message(
        target, owner or target, repo, access, ref, runner, grant_id, issued_at,
        expires_at)
    if not message:
        return ""
    signature = await runtime.sign_router(message)
    if not signature:
        return ""
    return _b64url(message.encode("utf-8")) + "." + signature


DEFAULT_PROVIDER_MODELS = {"claude-code": "claude-opus-5-5"}


async def _model_grant(runtime, runner, record, row, owner, repo, ref,
                       lease_id, issued_at, expires_at):
    if runner.get("scope") != "pool":
        return ""
    provider = str(record.get("provider") or row.get("provider") or "")
    model = str(record.get("model") or "") or DEFAULT_PROVIDER_MODELS.get(
        provider, "")
    message = edge_routing.agent_model_grant_message(
        runner["name"], runner.get("publicKey"), owner, repo, ref, provider,
        model, lease_id, issued_at, expires_at)
    if not message:
        return ""
    signature = await runtime.sign_router(message)
    if not signature:
        return ""
    return _b64url(message.encode("utf-8")) + "." + signature


async def _claim(runtime, runner, data):
    try:
        maximum = int(data.get("max") or 1)
    except (TypeError, ValueError):
        maximum = 1
    maximum = max(1, min(MAX_CLAIM, maximum))
    try:
        actions_max = int(data.get("actionsMax") or 0)
    except (TypeError, ValueError):
        actions_max = 0
    actions_max = max(0, min(MAX_CLAIM, actions_max))
    providers = data.get("providers")
    if isinstance(providers, list) and (providers or actions_max):
        providers = [str(p) for p in providers if str(p) in PROVIDERS]
    else:
        providers = list(PROVIDERS)
    requested = None
    if data.get("allowedOwners") not in (None, []):
        requested = _owners(data.get("allowedOwners"))
        if requested is None:
            return _error(runtime, "invalid_allowed_owners", 400)
    try:
        wait_ms = int(data.get("waitMs") or 0)
    except (TypeError, ValueError):
        wait_ms = 0
    wait_ms = max(0, min(CLAIM_WAIT_MAX_MS, wait_ms))
    now = runtime.now()
    await _requeue_expired(runtime, now)
    await runtime.d1_run(
        "UPDATE runner_nodes SET last_seen_at=? WHERE name=?",
        now, runner["name"])
    pool = sorted(runtime.pool_targets())
    targets = [runner["name"]] + [t for t in pool if t != runner["name"]]
    import runner_logins
    blocked = await runner_logins.blocked_providers(runtime, runner, data)
    providers = [p for p in providers if p not in blocked]
    mark = await _queue_mark(runtime, runner, targets) if wait_ms else None
    jobs = await _lease_jobs(runtime, runner, pool, targets, providers,
                             requested, maximum) if providers else []
    if actions_max and len(jobs) < maximum:
        import runner_actions
        jobs += await runner_actions.lease(
            runtime, runner, requested, min(actions_max, maximum - len(jobs)))
    if not jobs and mark is not None:
        await _await_queue_change(runtime, runner, targets, mark, now + wait_ms)
    extras = await runner_logins.runner_extras(runtime, runner, data)
    return runtime.response(
        {"ok": True, "jobs": jobs, "waitMs": wait_ms, **extras})


async def _queue_mark(runtime, runner, targets):
    marks = ",".join("?" * len(targets))
    try:
        row = await runtime.d1_first(
            "SELECT (SELECT COUNT(*) || ':' || COALESCE(MAX(id),0) || ':' || "
            "COALESCE(MAX(updated_at),0) FROM org_agent_jobs WHERE status IN "
            "('queued','leased') AND target_node IN (%s)) AS jobs,"
            "(SELECT COUNT(*) FROM runner_login_sessions WHERE runner=? AND "
            "state IN ('pending','running')) AS logins,"
            "(SELECT COUNT(*) FROM runner_account_requests WHERE runner=? AND "
            "state='pending') AS accounts,"
            "(SELECT COUNT(*) || ':' || COALESCE(MAX(id),0) FROM "
            "repo_action_jobs WHERE status='queued') AS actions" % marks,
            *targets, runner["name"], runner["name"])
    except Exception:
        return None
    if not row or int(row.get("logins") or 0) or int(row.get("accounts") or 0):
        return None
    return str(row.get("jobs") or "") + "/" + str(row.get("actions") or "")


async def _await_queue_change(runtime, runner, targets, mark, deadline):
    while True:
        left = deadline - runtime.now()
        if left <= 0:
            return
        if runtime.stopping():
            return
        await runtime.sleep(min(CLAIM_WAIT_TICK_MS, left))
        if runtime.stopping():
            return
        if await _queue_mark(runtime, runner, targets) != mark:
            return


async def _lease_jobs(runtime, runner, pool, targets, providers, requested,
                      maximum):
    now = runtime.now()
    rows = await runtime.d1_all(
        "SELECT id,session_id,target_node,repo,provider,data "
        "FROM org_agent_jobs WHERE status='queued' AND target_node IN (%s) "
        "ORDER BY id LIMIT %d" % (",".join("?" * len(targets)),
                                  CLAIM_SCAN_LIMIT),
        *targets)
    jobs = []
    for row in rows:
        if len(jobs) >= maximum:
            break
        job_id = int(row.get("id") or 0)
        target = str(row.get("target_node") or "")
        repo = str(row.get("repo") or "")
        session_id = str(row.get("session_id") or "")
        try:
            record = await runtime.open(row.get("data"))
        except Exception:
            record = None
        if not job_id or not isinstance(record, dict):
            continue
        pin = str(record.get("runner") or "").strip().lower()
        if pin and pin != runner["name"]:
            continue
        if not pin and str(record.get("kind") or "") == "steer":
            prior = await _latest_session_lease(runtime, session_id)
            if prior and prior.get("runner") == runner["name"]:
                pin = runner["name"]
        if target != runner["name"] and not (
                target in pool and (runner["scope"] == "pool" or pin)):
            continue
        owner = str(record.get("organization") or target).strip().lower()
        if not _owner_allowed(runner, owner, requested):
            continue
        if not _requester_allowed(runner, record):
            continue
        if str(record.get("provider") or row.get("provider")) not in providers:
            continue
        ref = await _job_ref(runtime, runner, record, session_id)
        if not ref:
            continue
        # The clone source is always the target's own gateway; a job queued
        # for the runner's own name therefore needs that name to have a
        # registered endpoint (BYO jobs are normally pinned on a core target).
        base = await runtime.gateway_url(target)
        if not base:
            continue
        lease_id = runtime.new_id()
        expires_at = now + JOB_TIMEOUT_MS
        served_owner = await _gateway_owner(runtime, target, repo)
        read_grant = await _grant(runtime, target, repo, "read", "",
                                  runner["name"], runtime.new_id(), now,
                                  expires_at, owner=served_owner)
        push_id = runtime.new_id()
        push_grant = await _grant(runtime, target, repo, "push", ref,
                                  runner["name"], push_id, now, expires_at,
                                  owner=served_owner)
        if not read_grant or not push_grant:
            continue
        await runtime.d1_run(
            "UPDATE org_agent_jobs SET status='leased',lease_id=?,updated_at=? "
            "WHERE id=? AND target_node=? AND status='queued' AND NOT EXISTS ("
            "SELECT 1 FROM org_agent_jobs active WHERE active.target_node=? "
            "AND active.repo=? AND active.status='leased')",
            lease_id, now, job_id, target, target, repo)
        claimed = await runtime.d1_first(
            "SELECT id FROM org_agent_jobs WHERE id=? AND target_node=? "
            "AND repo=? AND status='leased' AND lease_id=?",
            job_id, target, repo, lease_id)
        if not claimed:
            continue
        await runtime.d1_run(
            "INSERT INTO runner_job_leases (job_id,runner,session_id,lease_id,"
            "grant_id,target_node,repo,ref,expires_at,created_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?) ON CONFLICT(job_id) DO UPDATE SET "
            "runner=excluded.runner,lease_id=excluded.lease_id,"
            "grant_id=excluded.grant_id,ref=excluded.ref,"
            "expires_at=excluded.expires_at,created_at=excluded.created_at",
            job_id, runner["name"], session_id, lease_id, push_id, target,
            repo, ref, expires_at, now)
        model_grant = await _model_grant(
            runtime, runner, record, row, served_owner, repo, ref, lease_id,
            now, expires_at)
        jobs.append({
            "jobId": job_id,
            "leaseId": lease_id,
            "kind": str(record.get("kind") or ""),
            "sessionId": session_id,
            "repositoryOwner": served_owner,
            "repositoryName": repo,
            "organization": owner,
            "provider": str(record.get("provider") or row.get("provider")),
            "prompt": str(record.get("prompt") or ""),
            "model": str(record.get("model") or ""),
            "issueNumber": int(record.get("issueNumber") or 0),
            "createPr": record.get("createPr") is True,
            "requestedBy": str(record.get("requestedBy") or ""),
            "title": str(record.get("title") or ""),
            "branch": ref[len("refs/heads/"):],
            "baseBranch": str(record.get("baseBranch") or ""),
            "securityCheck": record.get("securityCheck") or {},
            "attachments": _attachment_meta(record.get("attachments")),
            "cloneUrl": (base.rstrip("/") + "/v1/runner/" + served_owner + "/"
                         + repo + ".git"),
            "readGrant": read_grant,
            "pushGrant": push_grant,
            "pushRef": ref,
            "grantExpiresAt": expires_at,
            "timeoutMs": JOB_TIMEOUT_MS,
            **({"modelGrant": model_grant} if model_grant else {}),
        })
    return jobs


def _attachment_meta(value):
    items = []
    for item in value if isinstance(value, list) else []:
        if not isinstance(item, dict):
            continue
        try:
            size = int(item.get("size") or 0)
        except (TypeError, ValueError):
            continue
        items.append({
            "id": str(item.get("id") or ""),
            "name": str(item.get("name") or ""),
            "mediaType": str(item.get("mediaType") or ""),
            "size": size,
            "sha256": str(item.get("sha256") or ""),
        })
    return items


# --- attachments ----------------------------------------------------------

async def _attachment(runtime, runner, data):
    try:
        job_id = int(data.get("jobId") or 0)
    except (TypeError, ValueError):
        job_id = 0
    lease_id = str(data.get("leaseId") or "")[:64]
    attachment_id = str(data.get("attachmentId") or "").strip().lower()
    row = None
    if job_id > 0 and lease_id and ATTACHMENT_ID_RE.fullmatch(attachment_id):
        row = await runtime.d1_first(
            "SELECT j.session_id,j.org_bi,j.data FROM runner_job_leases l "
            "JOIN org_agent_jobs j ON j.id=l.job_id AND j.lease_id=l.lease_id "
            "WHERE l.job_id=? AND l.runner=? AND l.lease_id=? "
            "AND l.lease_id!='' AND l.expires_at>=? "
            "AND j.status IN ('leased','running')",
            job_id, runner["name"], lease_id, runtime.now())
    if not row:
        return _error(runtime, "lease_not_found", 409,
                      "This runner does not hold that job's current lease.")
    try:
        record = await runtime.open(row.get("data"))
    except Exception:
        record = None
    meta = next((
        item for item in _attachment_meta(
            record.get("attachments") if isinstance(record, dict) else None)
        if item["id"] == attachment_id), None)
    image = b""
    if meta:
        image = await runtime.read_attachment(
            str(row.get("session_id") or ""), str(row.get("org_bi") or ""),
            attachment_id)
    if (not meta or not image or len(image) != meta["size"]
            or hashlib.sha256(image).hexdigest() != meta["sha256"]):
        return _error(runtime, "attachment_unavailable", 410,
                      "This job's attachment is no longer stored on the relay.")
    return runtime.bytes_response(image)


# --- report ---------------------------------------------------------------

async def _report(runtime, runner, data):
    try:
        job_id = int(data.get("jobId") or 0)
    except (TypeError, ValueError):
        job_id = 0
    lease_id = str(data.get("leaseId") or "")[:64]
    row = await runtime.d1_first(
        "SELECT job_id,target_node,repo,lease_id FROM runner_job_leases "
        "WHERE job_id=? AND runner=? AND lease_id=? AND lease_id!=''",
        job_id, runner["name"], lease_id) if job_id > 0 and lease_id else None
    if not row:
        return _error(runtime, "lease_not_found", 409,
                      "This runner does not hold that job's current lease.")
    response = await runtime.apply_result(
        row["target_node"], row["repo"], job_id,
        dict(data, runnerName=runner["name"]))
    status = str(data.get("status") or "").lower()
    if 200 <= runtime.status_of(response) < 300 and status in TERMINAL:
        import runner_logins
        try:
            await runner_logins.note_job_result(runtime, runner, job_id, data)
        except Exception:
            pass
    if 200 <= runtime.status_of(response) < 300 and (
            status in TERMINAL
            or str(data.get("securityVerdict") or "").lower() != "approved"):
        await runtime.d1_run(
            "UPDATE runner_job_leases SET lease_id='',expires_at=0 "
            "WHERE job_id=? AND lease_id=?", job_id, lease_id)
    return response


async def _stream(runtime, runner, data):
    try:
        job_id = int(data.get("jobId") or 0)
    except (TypeError, ValueError):
        job_id = 0
    lease_id = str(data.get("leaseId") or "")[:64]
    row = await runtime.d1_first(
        "SELECT job_id,session_id FROM runner_job_leases WHERE job_id=? AND "
        "runner=? AND lease_id=? AND lease_id!=''",
        job_id, runner["name"], lease_id) if job_id > 0 and lease_id else None
    if not row or not row.get("session_id"):
        return _error(runtime, "lease_not_found", 409,
                      "This runner does not hold that job's current lease.")
    import agent_streams
    status, body = await agent_streams.accept(
        runtime, str(row["session_id"]), job_id, data)
    return runtime.response(body, status)


# --- intrusion detection (docs/IDS.md) ------------------------------------

INCIDENT_SEVERITIES = ("info", "alert", "lockdown", "test")
INCIDENTS_PER_HOUR = 30
MAIL_LOG_NOTES_PER_HOUR = 120
# The runner IDS's own incident id, kept so `next_ids.py test-fire r1` can
# follow it; never trusted to overwrite a row (a taken id gets a new one).
INCIDENT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{8,64}$")


def _incident_text(value, limit):
    return "".join(c for c in str(value or "") if c.isprintable())[:limit]


async def _security_incident(runtime, runner, data):
    severity = str(data.get("severity") or "").strip().lower()
    detector = _incident_text(data.get("detector"), 60)
    summary = _incident_text(data.get("summary"), 300)
    detail = data.get("detail") if isinstance(data.get("detail"), dict) else {}
    if severity not in INCIDENT_SEVERITIES or not detector or not summary:
        return _error(runtime, "invalid_incident", 400)
    now = runtime.now()
    name = runner["name"]
    if severity == "lockdown":
        detail = dict(detail, lockdown={
            "revoke": [True, "the app revoked runner %s" % name]})
    detail_text = json.dumps(detail, sort_keys=True)[:4000]
    recent = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM security_incidents WHERE node_id=? "
        "AND created_ms>=?", name, now - 3600 * 1000)
    if int((recent or {}).get("n") or 0) >= INCIDENTS_PER_HOUR:
        return _error(runtime, "rate_limited", 429)
    incident_id = str(data.get("id") or "")
    if not INCIDENT_ID_RE.match(incident_id) or await runtime.d1_first(
            "SELECT id FROM security_incidents WHERE id=?", incident_id):
        incident_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO security_incidents (id,node_id,created_ms,severity,"
        "detector,summary,detail) VALUES (?,?,?,?,?,?,?)",
        incident_id, name, now, severity, detector, summary, detail_text)
    revoked = False
    if severity == "lockdown":
        await runtime.d1_run(
            "UPDATE runner_nodes SET status='revoked',updated_at=? "
            "WHERE name=?", now, name)
        await runtime.audit(name, "runner.ids_lockdown", name)
        revoked = True
    return runtime.response({"ok": True, "id": incident_id,
                             "revoked": revoked})


async def _mail_log_note(runtime, runner, data):
    fields = {}
    for key, limit in (("kind", 40), ("to", 254), ("subject", 200),
                       ("result", 20), ("error", 300), ("from", 254),
                       ("body", 400), ("via", 40), ("source", 80),
                       ("messageId", 200)):
        value = data.get(key)
        if value is not None:
            fields[key] = _incident_text(value, limit)
    try:
        fields["status"] = max(0, min(999, int(data.get("status") or 0)))
    except (TypeError, ValueError):
        fields["status"] = 0
    if not fields.get("result"):
        return _error(runtime, "invalid_note", 400)
    now = runtime.now()
    name = runner["name"]
    recent = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM mail_log_node_notes WHERE node=? "
        "AND created_at>=?", name, now - 3600 * 1000)
    if int((recent or {}).get("n") or 0) >= MAIL_LOG_NOTES_PER_HOUR:
        return _error(runtime, "rate_limited", 429)
    await runtime.d1_run(
        "INSERT INTO mail_log_node_notes (created_at,node,data) VALUES (?,?,?)",
        now, name, json.dumps(fields, sort_keys=True)[:4000])
    return runtime.response({"ok": True})


# --- pool status (core autoscaler) ----------------------------------------

async def _pool_status(runtime, path, raw):
    pool = sorted(runtime.pool_targets())

    async def core_keys(name):
        return await runtime.node_keys(name) if name in pool else []

    if not await _verify_signed(runtime, PREFIX + "pool-status", raw,
                                core_keys):
        return _error(runtime, "unauthorized", 401)
    now = runtime.now()
    personal = {
        str(r.get("name")) for r in await runtime.d1_all(
            "SELECT name FROM runner_nodes WHERE scope='personal'")}
    queued = 0
    oldest = 0
    if pool:
        rows = await runtime.d1_all(
            "SELECT id,data,created_at FROM org_agent_jobs WHERE "
            "status='queued' AND target_node IN (%s) ORDER BY id LIMIT 500"
            % ",".join("?" * len(pool)), *pool)
        for row in rows:
            try:
                record = await runtime.open(row.get("data"))
            except Exception:
                record = None
            pin = str((record or {}).get("runner") or "").strip().lower()
            if pin and pin in personal:
                continue
            queued += 1
            created = int(row.get("created_at") or 0)
            if created and (not oldest or created < oldest):
                oldest = created
    runners = []
    for row in await runtime.d1_all(
            "SELECT name,scope,last_seen_at FROM runner_nodes "
            "WHERE scope='pool' AND status='active' ORDER BY name"):
        active = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM runner_job_leases WHERE runner=? "
            "AND lease_id!='' AND expires_at>=?", row["name"], now)
        runners.append({
            "name": row["name"], "scope": row["scope"],
            "lastSeenAt": int(row.get("last_seen_at") or 0),
            "activeLeases": int((active or {}).get("n") or 0),
        })
    return runtime.response({"ok": True, "queued": queued,
                             "oldestQueuedAt": oldest, "runners": runners})


# --- Worker adapter -------------------------------------------------------

_BACKGROUND = set()


class WorkerRuntime:
    """Binds ``handle`` to entry.py's D1, crypto and session primitives.

    Every name used here (d1_first, ed25519_verify, ...) is supplied by
    ``_bind_runtime`` from the Worker entrypoint's globals.
    """

    def __init__(self, env, request):
        self.env = env
        self.request = request
        self._raw = None

    def method(self):
        return method_name(self.request)  # noqa: F821

    def header(self, name):
        try:
            return str(self.request.headers.get(name) or "")
        except Exception:
            return ""

    def now(self):
        return int(Date.now())  # noqa: F821

    async def actions_off(self, owner, repo):
        return await _branch_protection.actions_off(  # noqa: F821
            self.env, owner, repo)

    async def branch_protected(self, owner, repo, branch):
        protection = _branch_protection  # noqa: F821
        refs, problem = await protection.push_protection(
            self.env, owner, repo)
        return bool(problem) or protection.branch_is_protected(branch, refs)

    def stopping(self):
        event = getattr(self.env, "HOST_STOPPING", None)
        return event is not None and event.is_set()

    async def sleep(self, ms):
        import asyncio
        event = getattr(self.env, "HOST_STOPPING", None)
        if event is None:
            await asyncio.sleep(ms / 1000)
            return
        try:
            await asyncio.wait_for(event.wait(), ms / 1000)
        except asyncio.TimeoutError:
            pass

    def new_id(self):
        raw = js_crypto.getRandomValues(Uint8Array.new(16))  # noqa: F821
        return bytes(raw.to_py()).hex()

    def response(self, data, status=200):
        return json_response(  # noqa: F821
            data, status=status, cache_control="no-store")

    @staticmethod
    def bytes_response(data):
        return _route_handlers._org_agent_attachment_response(data)  # noqa: F821

    async def read_attachment(self, session_id, org_bi, attachment_id):
        data, _created_at = await (
            _route_handlers._org_agent_read_attachment(  # noqa: F821
                self.env, session_id, org_bi, attachment_id))
        return data

    def cached_response(self, data, max_age):
        return json_response(  # noqa: F821
            data, status=200,
            cache_control="private, max-age=%d" % int(max_age),
            extra_headers={"vary": "Cookie, Authorization"})

    @staticmethod
    def status_of(response):
        try:
            return int(response.status)
        except Exception:
            return 0

    async def ensure_schema(self):
        await ensure_schema(self.env)  # noqa: F821

    def query(self, name):
        try:
            from urllib.parse import parse_qs, urlparse
            values = parse_qs(urlparse(str(self.request.url)).query).get(name)
            return values[0] if values else ""
        except Exception:
            return ""

    async def body_bytes(self, limit):
        raw = str(await self.request.text()).encode("utf-8")
        return raw if len(raw) <= limit else None

    async def session(self, data):
        return await _account_session_record(  # noqa: F821
            self.env, self.request, data if isinstance(data, dict) else {})

    async def is_admin(self, name):
        return await _is_admin(self.env, name)  # noqa: F821

    async def name_taken(self, name):
        _bi, record = await _account_row(self.env, name)  # noqa: F821
        if record:
            return True
        return bool(await _claimed_node_signing_pubkeys(  # noqa: F821
            self.env, name))

    async def node_keys(self, name):
        return list(await _claimed_node_signing_pubkeys(  # noqa: F821
            self.env, name) or [])

    async def verify(self, public_key, signature, message):
        return bool(await ed25519_verify(  # noqa: F821
            public_key, signature, message))

    async def sign_router(self, message):
        public_key = _https_mirror_router_public_key(self.env)  # noqa: F821
        seed = _https_mirror_router_seed(self.env)  # noqa: F821
        if not public_key or not seed:
            return ""
        return str(await ed25519_sign(  # noqa: F821
            public_key, seed, message.encode("utf-8")) or "")

    def runner_install_url(self):
        return str(getattr(self.env, "RUNNER_INSTALL_URL", "") or "")

    def runner_app_url(self):
        return str(getattr(self.env, "APP_ORIGIN", "") or "").rstrip("/")

    def setting(self, name):
        return str(getattr(self.env, name, "") or "").strip()

    def public_origin(self):
        return (_public_base_url(self.env)  # noqa: F821
                or self.runner_app_url()).rstrip("/")

    async def default_branch(self, owner, repo):
        import core_push
        core_push._bind_runtime(globals())
        return await core_push._default_branch(self.env, owner, repo)

    def ssh_helper_socket(self):
        return str(getattr(self.env, "RUNNER_SSH_HELPER_SOCKET", "") or "")

    def ssh_helper(self, request):
        import runner_ssh_setup
        return runner_ssh_setup.helper_stream(self.ssh_helper_socket(), request)

    async def resolve_host(self, host):
        import runner_ssh_setup
        return await runner_ssh_setup.resolve_host(host)

    def spawn(self, coroutine):
        import asyncio
        task = asyncio.ensure_future(coroutine)
        _BACKGROUND.add(task)
        task.add_done_callback(_BACKGROUND.discard)
        return task

    def pool_targets(self):
        raw = str(getattr(self.env, "RUNNER_POOL_TARGET_NODES", "") or "")
        return {
            item.strip().lower() for item in raw.split(",")
            if NAME_RE.fullmatch(item.strip().lower())}

    async def gateway_url(self, node):
        row = await d1_first(  # noqa: F821
            self.env,
            "SELECT base_url,abuse_blocked FROM mirror_https_endpoints "
            "WHERE node_name=?", node)
        if not row or int(row.get("abuse_blocked") or 0):
            return ""
        return edge_routing.normalize_base_url(row.get("base_url")) or ""

    async def admit_push_state(self, owner, repo, node):
        node = str(node or "").strip().lower()
        context = await _https_mirror_public_context(  # noqa: F821
            self.env, owner, repo)
        if not context or not node:
            return ""
        for endpoint in await _https_mirror_request_candidates(  # noqa: F821
                self.env, context, ""):
            if str(endpoint.get("node") or "").strip().lower() == node:
                return await _admit_core_push_state(  # noqa: F821
                    self.env, owner, repo, endpoint)
        return ""

    async def apply_result(self, target_node, repo, job_id, data):
        return await _route_handlers._apply_org_agent_job_result(  # noqa: F821
            self.env, target_node, repo, job_id, data)

    async def open(self, value):
        return await decrypt_row(self.env, value)  # noqa: F821

    async def seal(self, value):
        return await encrypt_row(self.env, value)  # noqa: F821

    async def notify(self, account, topic):
        await notify_account_event(self.env, account, topic)  # noqa: F821

    async def alert_runner_auth(self, runner, provider, state, title, body,
                                href, dedupe, email=False, extra=(),
                                admins=True):
        now = self.now()
        recipients = list(
            await _operator_alerts._operator_alert_admins(  # noqa: F821
                self.env) or []) if admins else []
        for name in extra or ():
            name = str(name or "").strip().lower()
            if NAME_RE.fullmatch(name) and name not in recipients:
                recipients.append(name)
        pings = 0
        for recipient in recipients:
            if await enqueue_notification(  # noqa: F821
                    self.env, recipient, "operational_alert", title,
                    body=body, href=href, source="runner-auth",
                    dedupe=dedupe, ts=now,
                    meta={"runner": runner, "provider": provider,
                          "state": state}):
                pings += 1
        if email and admins:
            for address in await _repository_monitor_admin_emails(  # noqa: F821
                    self.env) or []:
                await _send_email(  # noqa: F821
                    self.env, address, "ForkMesh: " + title,
                    body + "\n\nRunners: " + href,
                    email_kind="operational")
        return pings

    async def audit(self, actor, action, target):
        await _audit_sensitive_action(  # noqa: F821
            self.env, actor, action, "runner", target, "success")

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)  # noqa: F821

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)  # noqa: F821

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)  # noqa: F821

    @staticmethod
    def changes(result):
        try:
            return int(result.meta.changes)
        except Exception:
            try:
                converted = result.to_py() if hasattr(result, "to_py") else result
                return int((converted.get("meta") or {}).get("changes") or 0)
            except Exception:
                return 0


async def route(env, request, path):
    return await handle(WorkerRuntime(env, request), path)
