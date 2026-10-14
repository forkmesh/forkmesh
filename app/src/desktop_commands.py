import hashlib
import re


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


SIGNING_DOMAIN = "forkmesh-desktop-command-v1"
MAX_PENDING_DESKTOP_COMMANDS = 50
DESKTOP_COMMAND_RETENTION_MS = 24 * 60 * 60 * 1000
DESKTOP_COMMAND_MAX_SKEW_MS = 5 * 60 * 1000

_PULL_TARGET_RE = re.compile(r"^pull:([1-9][0-9]{0,8})$")
_RUN_TARGET_RE = re.compile(r"^run:([A-Za-z0-9][A-Za-z0-9._-]{0,127})$")
_RELEASE_TARGET_RE = re.compile(
    r"^release:([A-Za-z0-9][A-Za-z0-9._+-]{0,127})$")

COMMANDS = {
    "pull.merge": (_PULL_TARGET_RE, "pullNumber"),
    "pull.merge_delete": (_PULL_TARGET_RE, "pullNumber"),
    "pull.fix_agent": (_PULL_TARGET_RE, "pullNumber"),
    "pull.close": (_PULL_TARGET_RE, "pullNumber"),
    "pull.reopen": (_PULL_TARGET_RE, "pullNumber"),
    "pull.delete": (_PULL_TARGET_RE, "pullNumber"),
    "action.rerun": (_RUN_TARGET_RE, "runId"),
    "release.draft": (_RELEASE_TARGET_RE, "tag"),
    "release.notes": (_RELEASE_TARGET_RE, "tag"),
    "release.create": (_RELEASE_TARGET_RE, "tag"),
    "release.publish": (_RELEASE_TARGET_RE, "tag"),
}

TABLES = (
    """CREATE TABLE IF NOT EXISTS desktop_commands (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        repo_bi TEXT NOT NULL,
        sig_digest TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        queued_at INTEGER NOT NULL,
        done_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_desktop_commands_repo "
    "ON desktop_commands(repo_bi, done_at, queued_at)",
)

_TABLES_READY = set()


async def ensure_tables(env):
    key = id(env)
    if key in _TABLES_READY:
        return
    for statement in TABLES:
        await d1_run(env, statement)
    _TABLES_READY.add(key)


def canonical_command(owner, repo, command, target, ts):
    return "\n".join((SIGNING_DOMAIN, owner, repo, command, target, ts))


def command_payload(command, target):
    spec = COMMANDS.get(command)
    if not spec:
        return None
    pattern, field = spec
    match = pattern.match(target)
    if not match:
        return None
    value = match.group(1)
    if field == "pullNumber":
        return {field: int(value)}
    return {field: value}


def _now_ms():
    return int(Date.now())


async def _pending_count(env, repo_bi, now):
    row = await d1_first(
        env,
        "SELECT COUNT(*) AS c FROM desktop_commands "
        "WHERE repo_bi=? AND done_at=0 AND queued_at>=?",
        repo_bi, now - DESKTOP_COMMAND_RETENTION_MS,
    )
    return int((row or {}).get("c") or 0)


async def _queue_command(env, request, owner, repo):
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    account = str(data.get("ownerAccount") or "").strip().lower()
    command = str(data.get("command") or "").strip()
    target = str(data.get("target") or "").strip()
    ts = str(data.get("ts") or "").strip()
    sig = str(data.get("sig") or "").strip()
    if not account:
        return json_response({"error": "not_authorized"}, status=403)
    if account == owner.strip().lower():
        queue_owner = owner
    elif await _org_write_allowed(env, owner, repo, account):
        queue_owner = account
    else:
        return json_response({"error": "not_authorized"}, status=403)
    signed_owner = await _public_repository_owner(env, request, owner, repo)
    payload = command_payload(command, target)
    if payload is None:
        return json_response({"error": "unsupported_command"}, status=400)
    if not ts.isdigit() or not sig or len(sig) > 200:
        return json_response({"error": "signature_required"}, status=401)
    now = _now_ms()
    if abs(now - int(ts)) > DESKTOP_COMMAND_MAX_SKEW_MS:
        return json_response({"error": "stale_request"}, status=401)
    canonical = canonical_command(
        signed_owner, repo, command, target, ts).encode()
    signer = ""
    for public_key in await _owner_signing_pubkeys(env, account):
        if await ed25519_verify(public_key, sig, canonical):
            signer = public_key
            break
    if not signer:
        return json_response({"error": "invalid_signature"}, status=401)

    await ensure_tables(env)
    repo_bi = await blind_index(env, queue_owner + "/" + repo)
    sig_digest = hashlib.sha256(sig.encode()).hexdigest()
    await d1_run(
        env,
        "DELETE FROM desktop_commands WHERE queued_at<?",
        now - DESKTOP_COMMAND_RETENTION_MS,
    )
    seen = await d1_first(
        env, "SELECT id FROM desktop_commands WHERE sig_digest=?", sig_digest)
    if seen:
        return json_response({"error": "replayed_request"}, status=409)
    pending = await _pending_count(env, repo_bi, now)
    if pending >= MAX_PENDING_DESKTOP_COMMANDS:
        return json_response({"error": "command_queue_full"}, status=429)
    record = {
        "command": command,
        "target": target,
        "payload": payload,
        "ts": ts,
        "sig": sig,
        "signer": signer,
        "ownerAccount": account,
        "repoOwner": signed_owner,
        "queuedAt": now,
    }
    try:
        await d1_run(
            env,
            "INSERT INTO desktop_commands "
            "(repo_bi, sig_digest, data, queued_at) VALUES (?,?,?,?)",
            repo_bi, sig_digest, await encrypt_row(env, record), now,
        )
    except Exception:
        return json_response({"error": "replayed_request"}, status=409)
    await notify_repo_host(env, queue_owner, repo, "desktop-commands")
    return json_response({"ok": True, "queued": pending + 1})


async def _list_commands(env, request, owner, repo):
    if not await _authorize_owner(env, request, owner):
        return json_response({"error": "unauthorized"}, status=401)
    await ensure_tables(env)
    repo_bi = await blind_index(env, owner + "/" + repo)
    now = _now_ms()
    rows = await d1_all(
        env,
        "SELECT id, data FROM desktop_commands "
        "WHERE repo_bi=? AND done_at=0 AND queued_at>=? "
        "ORDER BY id ASC LIMIT ?",
        repo_bi, now - DESKTOP_COMMAND_RETENTION_MS,
        MAX_PENDING_DESKTOP_COMMANDS,
    )
    commands = []
    for row in rows or []:
        record = await decrypt_row(env, row.get("data"))
        if not record:
            continue
        record["id"] = int(row.get("id") or 0)
        record["canonical"] = canonical_command(
            record.get("repoOwner") or owner, repo, record.get("command", ""),
            record.get("target", ""), record.get("ts", ""))
        commands.append(record)
    return json_response({
        "ok": True,
        "commands": commands,
        "ackPath": "/api/repo/%s/%s/desktop-commands/ack" % (owner, repo),
    })


async def desktop_commands_handler(env, request, owner, repo):
    method = method_name(request)
    if method == "POST":
        return await _queue_command(env, request, owner, repo)
    if method == "GET":
        return await _list_commands(env, request, owner, repo)
    return json_response({"error": "method_not_allowed"}, status=405)


async def desktop_commands_ack_handler(env, request, owner, repo):
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    if not await _authorize_owner(env, request, owner):
        return json_response({"error": "unauthorized"}, status=401)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    raw_ids = data.get("ids") if isinstance(data, dict) else None
    if not isinstance(raw_ids, list):
        return json_response({"error": "ids_required"}, status=400)
    ids = []
    for value in raw_ids[:MAX_PENDING_DESKTOP_COMMANDS]:
        try:
            value = int(value)
        except (TypeError, ValueError):
            continue
        if value > 0 and value not in ids:
            ids.append(value)
    if not ids:
        return json_response({"ok": True, "acknowledged": 0})
    await ensure_tables(env)
    repo_bi = await blind_index(env, owner + "/" + repo)
    await d1_run(
        env,
        "UPDATE desktop_commands SET done_at=? "
        "WHERE repo_bi=? AND done_at=0 AND id IN (%s)"
        % ",".join("?" for _ in ids),
        _now_ms(), repo_bi, *ids,
    )
    return json_response({"ok": True, "acknowledged": len(ids)})
