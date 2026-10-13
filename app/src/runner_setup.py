import hashlib
import json
import re

import runner_leases as leases


TOKEN_PREFIX = "fmrt_"
TOKEN_RE = re.compile(r"^fmrt_[0-9a-f]{64}$")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
TOKEN_TTL_MS = 30 * 60 * 1000
MAX_OPEN_TOKENS = 5
MAX_TOKENS_PER_HOUR = 20
HOUR_MS = 60 * 60 * 1000
KEEP_MS = 24 * 60 * 60 * 1000
DEFAULT_INSTALL_URL = "https://forkmesh.com/install-runner.sh"
DEFAULT_APP_URL = "https://app.forkmesh.com"
VIA = ("command", "ssh", "desktop")


def match(action):
    parts = action.split("/") if action else []
    if parts == ["setup-tokens"]:
        return ("mint",)
    if parts == ["register-token"]:
        return ("register",)
    if len(parts) == 2 and parts[0] == "setup-tokens":
        return ("status", parts[1])
    if len(parts) == 3 and parts[0] == "setup-tokens" and parts[2] == "revoke":
        return ("revoke", parts[1])
    return None


def token_hash(token):
    return hashlib.sha256(str(token).encode("utf-8")).hexdigest()


def register_message(name, public_key, digest, ts):
    return "\n".join((
        "forkmesh-runner-register-token-v1", str(name), str(public_key),
        str(digest), str(ts)))


def shell_quote(value):
    return "'" + str(value).replace("'", "'\\''") + "'"


def install_url(runtime):
    value = getattr(runtime, "runner_install_url", None)
    url = value() if callable(value) else ""
    return url if re.fullmatch(r"https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?/[A-Za-z0-9._/-]*",
                               url or "") else DEFAULT_INSTALL_URL


def app_url(runtime):
    value = getattr(runtime, "runner_app_url", None)
    url = value() if callable(value) else ""
    return url if re.fullmatch(r"https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?",
                               url or "") else DEFAULT_APP_URL


def install_command(runtime, token, name):
    return ("curl -fsSL %s | sudo bash -s -- --token %s --name %s --app %s"
            % (install_url(runtime), token, name, app_url(runtime)))


def _error(runtime, code, status, message=""):
    return leases._error(runtime, code, status, message)


async def handle(runtime, route):
    kind = route[0]
    method = runtime.method()
    want = "GET" if kind == "status" else "POST"
    if method != want:
        return _error(runtime, "method_not_allowed", 405)
    raw = b""
    if want == "POST":
        raw = await runtime.body_bytes(leases.MAX_BODY_BYTES)
        if raw is None:
            return _error(runtime, "request_too_large", 413)
    data = leases._json(raw) if raw else {}
    if data is None:
        return _error(runtime, "invalid_json", 400)
    if kind == "register":
        return await _register(runtime, data)
    actor = await leases._account(runtime, data)
    if not actor:
        return _error(runtime, "unauthorized", 401)
    if kind == "mint":
        return await _mint(runtime, actor, data)
    token_id = route[1]
    if not ID_RE.fullmatch(token_id):
        return _error(runtime, "not_found", 404)
    row = await runtime.d1_first(
        "SELECT * FROM runner_setup_tokens WHERE id=? AND owner=?",
        token_id, actor)
    if not row:
        return _error(runtime, "not_found", 404)
    if kind == "status":
        return runtime.response({"ok": True, "setup": await _public(
            runtime, row)})
    return await _revoke(runtime, actor, row)


async def sweep(runtime, now):
    await runtime.d1_run(
        "DELETE FROM runner_setup_tokens WHERE expires_at<?", now - KEEP_MS)


async def check_name(runtime, actor, name):
    if not leases.NAME_RE.fullmatch(name):
        return "invalid_name", 400, (
            "Runner names use lowercase letters, digits and dashes, start "
            "with a letter, and are at most 63 characters.")
    if name in runtime.pool_targets():
        return "name_taken", 409, "That name belongs to a core node."
    existing = await runtime.d1_first(
        "SELECT registered_by,scope FROM runner_nodes WHERE name=?", name)
    if existing:
        if existing.get("registered_by") != actor or existing.get(
                "scope") != "personal":
            return "name_taken", 409, "Another runner already uses that name."
        return None
    if await runtime.name_taken(name):
        return "name_taken", 409, (
            "That name is an account or node name. Pick another.")
    return None


async def mint(runtime, actor, name, via="command"):
    now = runtime.now()
    await sweep(runtime, now)
    problem = await check_name(runtime, actor, name)
    if problem:
        return None, problem
    open_row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM runner_setup_tokens WHERE owner=? AND "
        "state='pending' AND expires_at>=?", actor, now)
    if int((open_row or {}).get("n") or 0) >= MAX_OPEN_TOKENS:
        return None, ("too_many_tokens", 429,
                      "You have %d unused setup commands. Use or cancel one "
                      "first, or wait for them to expire." % MAX_OPEN_TOKENS)
    recent = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM runner_setup_tokens WHERE owner=? AND "
        "created_at>=?", actor, now - HOUR_MS)
    if int((recent or {}).get("n") or 0) >= MAX_TOKENS_PER_HOUR:
        return None, ("rate_limited", 429,
                      "Too many setup commands this hour. Try again later.")
    token = TOKEN_PREFIX + runtime.new_id() + runtime.new_id()
    token_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO runner_setup_tokens (id,token_hash,owner,runner,via,"
        "state,created_at,expires_at,used_at) VALUES (?,?,?,?,?,'pending',"
        "?,?,0)", token_id, token_hash(token), actor, name,
        via if via in VIA else "command", now, now + TOKEN_TTL_MS)
    await runtime.audit(actor, "runner.setup_token.create", name)
    return {"id": token_id, "token": token, "runner": name,
            "expiresAt": now + TOKEN_TTL_MS}, None


async def _mint(runtime, actor, data):
    name = str(data.get("name") or "").strip().lower()
    via = str(data.get("via") or "command").strip().lower()
    minted, problem = await mint(runtime, actor, name, via)
    if problem:
        return _error(runtime, *problem)
    body = {
        "ok": True,
        "id": minted["id"],
        "token": minted["token"],
        "runner": minted["runner"],
        "expiresAt": minted["expiresAt"],
        "ttlMs": TOKEN_TTL_MS,
        "installUrl": install_url(runtime),
        "appUrl": app_url(runtime),
        "command": install_command(runtime, minted["token"], name),
    }
    return runtime.response(body, 201)


async def _public(runtime, row):
    now = runtime.now()
    state = str(row.get("state") or "pending")
    if state == "pending" and int(row.get("expires_at") or 0) < now:
        state = "expired"
    body = {
        "id": row["id"],
        "runner": row.get("runner") or "",
        "via": row.get("via") or "command",
        "state": state,
        "createdAt": int(row.get("created_at") or 0),
        "expiresAt": int(row.get("expires_at") or 0),
        "usedAt": int(row.get("used_at") or 0),
    }
    if state == "used":
        runner = await runtime.d1_first(
            "SELECT status,last_seen_at FROM runner_nodes WHERE name=?",
            row.get("runner") or "")
        body["runnerStatus"] = (runner or {}).get("status") or ""
        body["lastSeenAt"] = int((runner or {}).get("last_seen_at") or 0)
    return body


async def _revoke(runtime, actor, row):
    await runtime.d1_run(
        "UPDATE runner_setup_tokens SET state='revoked' WHERE id=? AND "
        "state='pending'", row["id"])
    await runtime.audit(actor, "runner.setup_token.revoke",
                        row.get("runner") or "")
    fresh = await runtime.d1_first(
        "SELECT * FROM runner_setup_tokens WHERE id=?", row["id"])
    return runtime.response({"ok": True, "setup": await _public(
        runtime, fresh)})


async def _register(runtime, data):
    token = str(data.get("token") or "").strip()
    name = str(data.get("name") or "").strip().lower()
    public_key = str(data.get("publicKey") or "").strip()
    proof = str(data.get("proof") or "").strip()
    try:
        ts = int(data.get("ts"))
    except (TypeError, ValueError):
        ts = 0
    if (not TOKEN_RE.fullmatch(token) or not leases.NAME_RE.fullmatch(name)
            or not leases.PUBKEY_RE.fullmatch(public_key)
            or not leases.SIG_RE.fullmatch(proof) or ts <= 0):
        return _error(runtime, "invalid_registration", 400)
    now = runtime.now()
    if abs(now - ts) > leases.REGISTER_WINDOW_MS:
        return _error(runtime, "stale_registration", 400,
                      "This machine's clock is more than 5 minutes off.")
    digest = token_hash(token)
    row = await runtime.d1_first(
        "SELECT * FROM runner_setup_tokens WHERE token_hash=?", digest)
    if (not row or row.get("state") != "pending"
            or int(row.get("expires_at") or 0) < now):
        return _error(runtime, "invalid_token", 401,
                      "The setup token is unknown, used, cancelled or "
                      "expired. Make a new command on the Runners page.")
    if row.get("runner") != name:
        return _error(runtime, "invalid_token", 401,
                      "The setup token was made for another runner name.")
    owner = str(row.get("owner") or "")
    message = register_message(name, public_key, digest, ts)
    if not await runtime.verify(public_key, proof, message.encode("utf-8")):
        return _error(runtime, "invalid_proof", 401)
    problem = await check_name(runtime, owner, name)
    if problem:
        return _error(runtime, *problem)
    result = await runtime.d1_run(
        "UPDATE runner_setup_tokens SET state='used',used_at=? WHERE id=? "
        "AND state='pending' AND expires_at>=?", now, row["id"], now)
    if runtime.changes(result) != 1:
        return _error(runtime, "invalid_token", 401,
                      "The setup token was already used.")
    existing = await runtime.d1_first(
        "SELECT name FROM runner_nodes WHERE name=?", name)
    owners = [owner]
    await runtime.d1_run(
        "INSERT INTO runner_nodes (name,public_key,registered_by,scope,"
        "allowed_owners,status,created_at,updated_at,last_seen_at) "
        "VALUES (?,?,?,'personal',?,'active',?,?,0) ON CONFLICT(name) DO "
        "UPDATE SET public_key=excluded.public_key,scope='personal',"
        "allowed_owners=excluded.allowed_owners,status='active',"
        "updated_at=excluded.updated_at",
        name, public_key, owner, json.dumps(owners), now, now)
    await runtime.audit(owner, "runner.register_token", name)
    return runtime.response({"ok": True, "runner": {
        "name": name, "scope": "personal", "allowedOwners": owners,
        "publicKey": public_key}, "appUrl": app_url(runtime)},
        201 if not existing else 200)
