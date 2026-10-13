import re


PREFIX = "forkmesh-deploy-approval-v1"
KINDS = ("pull", "branch")
MAX_SKEW_MS = 15 * 60 * 1000
DEFAULT_REPOSITORY = "forkmesh/forkmesh"
TERMINAL = ("deployed", "failed", "skipped", "superseded", "expired")
KEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
SIG_RE = re.compile(r"^[A-Za-z0-9_-]{86}$")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
REQUEST_RE = re.compile(r"^[A-Za-z0-9_.:-]{8,80}$")
TARGET_RE = re.compile(r"^[A-Za-z0-9_./-]{1,120}$")
NAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")
REPO_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def approval_message(kind, owner, repo, target, base, head, request_id, ts):
    return "\n".join((
        PREFIX, str(kind), str(owner or "").strip().lower(), str(repo or ""),
        str(target or ""), str(base or ""), str(head or ""),
        str(request_id or ""), str(int(ts)),
    )).encode("utf-8")


def parse(value):
    if not isinstance(value, dict) or set(value) != {
            "publicKey", "ts", "signature"}:
        return None
    key = value.get("publicKey")
    signature = value.get("signature")
    ts = value.get("ts")
    if (not isinstance(key, str) or not KEY_RE.fullmatch(key)
            or not isinstance(signature, str) or not SIG_RE.fullmatch(signature)
            or isinstance(ts, bool) or not isinstance(ts, int) or ts <= 0):
        return None
    return {"publicKey": key, "ts": ts, "signature": signature}


def repository(env):
    value = str(getattr(env, "SELF_DEPLOY_REPOSITORY", "") or "").strip()
    return (value or DEFAULT_REPOSITORY).lower()


async def verify(env, actor, approval, kind, owner, repo, target, base, head,
                 request_id, now):
    approval = parse(approval)
    if (approval is None or kind not in KINDS
            or (owner.lower() + "/" + repo).lower() != repository(env)
            or abs(now - approval["ts"]) > MAX_SKEW_MS
            or not TARGET_RE.fullmatch(str(target or ""))
            or not REQUEST_RE.fullmatch(str(request_id or ""))):
        return None
    if approval["publicKey"] not in await _owner_signing_pubkeys(env, actor):
        return None
    message = approval_message(kind, owner, repo, target, base, head,
                               request_id, approval["ts"])
    if not await ed25519_verify(
            approval["publicKey"], approval["signature"], message):
        return None
    return {
        "kind": kind, "owner": owner.lower(), "repo": repo,
        "target": str(target), "expectedBase": str(base or ""),
        "expectedHead": str(head or ""), "requestId": str(request_id),
        "actor": actor, **approval,
    }


async def record(env, approved, result, now):
    if not isinstance(approved, dict) or not isinstance(result, dict):
        return 0
    base_before = str(result.get("baseBefore") or "").lower()
    head = str(result.get("head") or "").lower()
    base_after = str(result.get("baseAfter") or "").lower()
    if not (OID_RE.fullmatch(base_before) and OID_RE.fullmatch(head)
            and OID_RE.fullmatch(base_after)):
        return 0
    if approved["kind"] == "pull" and (
            base_before != approved["expectedBase"]
            or head != approved["expectedHead"]):
        return 0
    await ensure_schema(env)
    await d1_run(
        env,
        "INSERT OR IGNORE INTO deploy_approvals (request_id,kind,owner,repo,"
        "target,expected_base,expected_head,base_before,head,base_after,"
        "public_key,signature,signed_at,actor,status,created_at,updated_at) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,'pending',?,?)",
        approved["requestId"], approved["kind"], approved["owner"],
        approved["repo"], approved["target"], approved["expectedBase"],
        approved["expectedHead"], base_before, head, base_after,
        approved["publicKey"], approved["signature"], approved["ts"],
        approved["actor"], now, now)
    row = await d1_first(
        env, "SELECT id FROM deploy_approvals WHERE request_id=?",
        approved["requestId"])
    return int((row or {}).get("id") or 0)


async def recent(env, limit=10):
    await ensure_schema(env)
    rows = await d1_all(
        env,
        "SELECT id,kind,owner,repo,target,base_after,actor,status,message,"
        "created_at,updated_at FROM deploy_approvals ORDER BY id DESC "
        "LIMIT ?", int(limit))
    out = []
    for row in rows or []:
        nodes = await d1_all(
            env,
            "SELECT node,status,message,updated_at FROM deploy_approval_nodes "
            "WHERE approval_id=? ORDER BY node", row["id"])
        out.append({
            "id": int(row["id"]), "kind": row["kind"],
            "repository": row["owner"] + "/" + row["repo"],
            "target": row["target"], "commit": row["base_after"],
            "approvedBy": row["actor"], "status": row["status"],
            "message": row.get("message") or "",
            "createdAt": int(row.get("created_at") or 0),
            "updatedAt": int(row.get("updated_at") or 0),
            "nodes": [{
                "node": n["node"], "status": n["status"],
                "message": n.get("message") or "",
                "updatedAt": int(n.get("updated_at") or 0),
            } for n in nodes or []],
        })
    return out


def announcement(row):
    commit = str(row.get("base_after") or "")[:12]
    status = str(row.get("status") or "")
    message = str(row.get("message") or "")
    if status == "deployed":
        return ("Deployed " + commit + " to the cores",
                "Merged from your phone and live on every core.")
    if status == "failed":
        return ("Deploy of " + commit + " failed",
                message or "A core refused the release; it rolled back.")
    return ("Deploy of " + commit + " " + status,
            message or "The cores did not deploy this merge.")


async def announce(env):
    await ensure_schema(env)
    rows = await d1_all(
        env,
        "SELECT id,base_after,actor,status,message FROM deploy_approvals "
        "WHERE announced=0 AND status IN (%s) ORDER BY id LIMIT 20"
        % ",".join("?" * len(TERMINAL)), *TERMINAL)
    sent = 0
    for row in rows or []:
        title, body = announcement(row)
        recipients = list(await _operator_alerts._operator_alert_admins(env)
                          or [])
        actor = str(row.get("actor") or "")
        if actor and actor not in recipients:
            recipients.append(actor)
        for recipient in recipients:
            await enqueue_notification(
                env, recipient, "operational_alert", title, body=body,
                href="/dashboard", source="self-deploy",
                dedupe="self-deploy:%d" % int(row["id"]),
                meta={"approval": int(row["id"]), "status": row["status"]})
        await d1_run(
            env, "UPDATE deploy_approvals SET announced=1 WHERE id=?",
            row["id"])
        sent += 1
    return sent
