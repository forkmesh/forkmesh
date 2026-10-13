import base64
import hashlib
import ipaddress
import json
import re

import runner_leases as leases
import runner_setup


ID_RE = re.compile(r"^[0-9a-f]{32}$")
HOST_RE = re.compile(
    r"^(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)*"
    r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")
USER_RE = re.compile(r"^[a-z_][a-z0-9_.-]{0,31}$")
KEY_TYPES = ("ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
             "ecdsa-sha2-nistp521", "rsa-sha2-512", "rsa-sha2-256", "ssh-rsa")
KEY_BLOB_RE = re.compile(r"^[A-Za-z0-9+/]{16,2048}={0,2}$")
PRIVATE_KEY_RE = re.compile(
    r"^-----BEGIN (?:OPENSSH|RSA|EC|DSA|ENCRYPTED)? ?PRIVATE KEY-----\r?\n"
    r"[\s\S]{16,16000}\r?\n-----END (?:OPENSSH|RSA|EC|DSA|ENCRYPTED)? ?"
    r"PRIVATE KEY-----\s*$")
MAX_PASSWORD = 1024
MAX_PASSPHRASE = 1024
PROBE_TTL_MS = 10 * 60 * 1000
RUN_STALE_MS = 25 * 60 * 1000
MAX_PROBES_PER_HOUR = 20
HOUR_MS = 60 * 60 * 1000
KEEP_MS = 24 * 60 * 60 * 1000
LOG_LINES = 80
LOG_CHARS = 8000
FLUSH_MS = 1500
WIREGUARD = ipaddress.ip_network("10.77.0.0/24")
TOKEN_SCRUB = re.compile(r"fmrt_[0-9a-f]{8,}")
ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07|\x1b[@-Z\\-_]")
INSTALLS = (
    "the forkmesh-mirror-node binary from the signed ForkMesh release, run "
    "as a runner (it serves no repositories)",
    "a forkmesh-runner systemd service and an unprivileged forkmesh-job user "
    "that agent jobs run as",
    "Node.js and the Claude Code CLI, when they are missing",
    "firewall rules that keep agent jobs off this machine's private network",
)


def match(action):
    parts = action.split("/") if action else []
    if len(parts) != 2 or parts[0] != "ssh":
        return None
    if parts[1] in ("probe", "install"):
        return (parts[1],)
    return ("status", parts[1])


def fingerprint(blob):
    try:
        raw = base64.b64decode(blob, validate=True)
    except (ValueError, TypeError):
        return ""
    digest = base64.b64encode(hashlib.sha256(raw).digest()).decode("ascii")
    return "SHA256:" + digest.rstrip("=")


def public_address(value):
    try:
        address = ipaddress.ip_address(str(value))
    except ValueError:
        return None
    if address.version == 6 and address.ipv4_mapped is not None:
        address = address.ipv4_mapped
    if not address.is_global or address in WIREGUARD:
        return None
    return address


def scrub(text, secrets=()):
    text = ANSI.sub("", str(text or ""))
    text = TOKEN_SCRUB.sub("fmrt_[hidden]", text)
    for secret in secrets:
        if secret and len(secret) >= 4:
            text = text.replace(secret, "[hidden]")
    return "".join(ch for ch in text if ch.isprintable() or ch == "\t")


def append_log(log, line):
    lines = (log.split("\n") if log else []) + [line[:400]]
    joined = "\n".join(lines[-LOG_LINES:])
    return joined[-LOG_CHARS:]


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
    raw = b""
    if data is None:
        return _error(runtime, "invalid_json", 400)
    try:
        actor = await leases._account(runtime, data)
        if not actor:
            return _error(runtime, "unauthorized", 401)
        if kind == "probe":
            return await _probe(runtime, actor, data)
        if kind == "install":
            return await _install(runtime, actor, data)
        setup_id = route[1]
        if not ID_RE.fullmatch(setup_id):
            return _error(runtime, "not_found", 404)
        return await _status(runtime, actor, setup_id)
    finally:
        if isinstance(data, dict):
            data.clear()


def _helper(runtime):
    helper = getattr(runtime, "ssh_helper", None)
    socket_path = getattr(runtime, "ssh_helper_socket", None)
    if helper is None or (callable(socket_path) and not socket_path()):
        return None
    return helper


async def _probe(runtime, actor, data):
    helper = _helper(runtime)
    if helper is None:
        return _error(runtime, "ssh_setup_unavailable", 503,
                      "Server-side SSH setup is not available here. Use "
                      "ForkMesh Desktop, or run the one-line command yourself.")
    host = str(data.get("host") or "").strip().lower().rstrip(".")
    try:
        port = int(data.get("port") or 22)
    except (TypeError, ValueError):
        port = 0
    if not (HOST_RE.fullmatch(host) or public_address(host)) or not (
            0 < port < 65536):
        return _error(runtime, "invalid_host", 400,
                      "Enter a host name or public IP address and a port.")
    now = runtime.now()
    await runtime.d1_run(
        "DELETE FROM runner_ssh_setups WHERE updated_at<?", now - KEEP_MS)
    recent = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM runner_ssh_setups WHERE owner=? AND "
        "created_at>=?", actor, now - HOUR_MS)
    if int((recent or {}).get("n") or 0) >= MAX_PROBES_PER_HOUR:
        return _error(runtime, "rate_limited", 429,
                      "Too many SSH checks this hour. Try again later.")
    addresses = await runtime.resolve_host(host)
    checked = [public_address(item) for item in addresses or []]
    if not checked or any(item is None for item in checked):
        return _error(runtime, "address_not_public", 400,
                      "That host does not resolve to public addresses only. "
                      "ForkMesh connects to public servers; set up a machine "
                      "on a private network from ForkMesh Desktop or with "
                      "the one-line command.")
    address = str(sorted(checked, key=lambda item: item.version)[0])
    keys = []
    error = ""
    async for message in helper({"op": "keyscan", "address": address,
                                 "port": port}):
        if message.get("error"):
            error = str(message["error"])[:200]
        for item in message.get("keys") or []:
            key_type = str((item or {}).get("type") or "")
            blob = str((item or {}).get("key") or "")
            print_ = fingerprint(blob)
            if key_type in KEY_TYPES and KEY_BLOB_RE.fullmatch(blob) and print_:
                keys.append({"type": key_type, "key": blob,
                             "fingerprint": print_})
    if not keys:
        return _error(runtime, "ssh_unreachable", 502,
                      "No SSH server answered at %s port %d. %s" % (
                          address, port, scrub(error)))
    keys.sort(key=lambda item: KEY_TYPES.index(item["type"]))
    setup_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO runner_ssh_setups (id,owner,host,address,port,host_keys,"
        "state,created_at,updated_at,expires_at) VALUES "
        "(?,?,?,?,?,?,'probed',?,?,?)", setup_id, actor, host, address, port,
        json.dumps(keys), now, now, now + PROBE_TTL_MS)
    return runtime.response({
        "ok": True,
        "id": setup_id,
        "host": host,
        "address": address,
        "port": port,
        "hostKeys": [{"type": item["type"],
                      "fingerprint": item["fingerprint"]} for item in keys],
        "installs": list(INSTALLS),
        "expiresAt": now + PROBE_TTL_MS,
    })


async def _install(runtime, actor, data):
    helper = _helper(runtime)
    if helper is None:
        return _error(runtime, "ssh_setup_unavailable", 503)
    setup_id = str(data.get("id") or "")
    if not ID_RE.fullmatch(setup_id):
        return _error(runtime, "not_found", 404)
    row = await runtime.d1_first(
        "SELECT * FROM runner_ssh_setups WHERE id=? AND owner=?",
        setup_id, actor)
    if not row:
        return _error(runtime, "not_found", 404)
    now = runtime.now()
    if row.get("state") != "probed" or int(row.get("expires_at") or 0) < now:
        return _error(runtime, "setup_expired", 409,
                      "Check the server again: host-key checks last 10 "
                      "minutes and are used once.")
    try:
        keys = json.loads(row.get("host_keys") or "[]")
    except ValueError:
        keys = []
    confirmed = str(data.get("fingerprint") or "").strip()
    key = next((item for item in keys if isinstance(item, dict)
                and item.get("fingerprint") == confirmed), None)
    if not key:
        return _error(runtime, "fingerprint_mismatch", 400,
                      "Confirm one of the host-key fingerprints shown.")
    user = str(data.get("username") or "").strip()
    password = data.get("password")
    private_key = data.get("privateKey")
    passphrase = data.get("passphrase")
    password = password if isinstance(password, str) else ""
    private_key = private_key if isinstance(private_key, str) else ""
    passphrase = passphrase if isinstance(passphrase, str) else ""
    if not USER_RE.fullmatch(user):
        return _error(runtime, "invalid_username", 400,
                      "Enter the SSH user name (for example root).")
    if bool(password) == bool(private_key):
        return _error(runtime, "invalid_credentials", 400,
                      "Enter either a password or a private key.")
    if len(password) > MAX_PASSWORD or len(passphrase) > MAX_PASSPHRASE or (
            private_key and not PRIVATE_KEY_RE.fullmatch(private_key)):
        return _error(runtime, "invalid_credentials", 400,
                      "The password or private key is not in a form "
                      "ForkMesh accepts.")
    name = str(data.get("name") or "").strip().lower()
    minted, problem = await runner_setup.mint(runtime, actor, name, "ssh")
    if problem:
        return _error(runtime, *problem)
    result = await runtime.d1_run(
        "UPDATE runner_ssh_setups SET state='running',fingerprint=?,runner=?,"
        "token_id=?,updated_at=?,message='Connecting',log='' WHERE id=? AND "
        "state='probed'", confirmed, name, minted["id"], now, setup_id)
    if runtime.changes(result) != 1:
        return _error(runtime, "setup_expired", 409)
    helper_request = {
        "op": "install",
        "address": row["address"],
        "port": int(row["port"]),
        "hostKey": {"type": key["type"], "key": key["key"]},
        "user": user,
        "password": password,
        "privateKey": private_key,
        "passphrase": passphrase,
        "installUrl": runner_setup.install_url(runtime),
        "appUrl": runner_setup.app_url(runtime),
        "token": minted["token"],
        "name": name,
    }
    secrets = [minted["token"], password, passphrase]
    password = private_key = passphrase = ""
    await runtime.audit(actor, "runner.ssh_setup.start", name)
    runtime.spawn(run_install(runtime, setup_id, helper_request, secrets))
    return runtime.response({
        "ok": True, "id": setup_id, "state": "running", "runner": name,
        "setupTokenId": minted["id"], "installs": list(INSTALLS)}, 202)


async def run_install(runtime, setup_id, helper_request, secrets):
    log = ""
    last = 0
    state = "failed"
    message = "The setup stopped without an answer."
    try:
        async for event in _helper(runtime)(helper_request):
            helper_request.clear()
            if "out" in event:
                line = scrub(event.get("out"), secrets).rstrip()
                if line:
                    log = append_log(log, line)
            if "exit" in event:
                code = event.get("exit")
                if code == 0:
                    state, message = "succeeded", "Installed. The runner registers itself and appears in the list."
                else:
                    message = scrub(event.get("error") or (
                        "The installer exited with status %s." % code), secrets)[:240]
                break
            now = runtime.now()
            if now - last >= FLUSH_MS:
                last = now
                await runtime.d1_run(
                    "UPDATE runner_ssh_setups SET log=?,updated_at=?,"
                    "message='Installing' WHERE id=? AND state='running'",
                    log, now, setup_id)
    except Exception as exc:
        message = scrub("The SSH setup failed: %s" % type(exc).__name__)
    finally:
        helper_request.clear()
        secrets = None
    await runtime.d1_run(
        "UPDATE runner_ssh_setups SET state=?,message=?,log=?,updated_at=? "
        "WHERE id=? AND state='running'", state, message, log, runtime.now(),
        setup_id)


async def _status(runtime, actor, setup_id):
    row = await runtime.d1_first(
        "SELECT * FROM runner_ssh_setups WHERE id=? AND owner=?",
        setup_id, actor)
    if not row:
        return _error(runtime, "not_found", 404)
    now = runtime.now()
    if row.get("state") == "running" and int(
            row.get("updated_at") or 0) < now - RUN_STALE_MS:
        await runtime.d1_run(
            "UPDATE runner_ssh_setups SET state='failed',updated_at=?,"
            "message='The setup stopped reporting.' WHERE id=? AND "
            "state='running'", now, setup_id)
        row = await runtime.d1_first(
            "SELECT * FROM runner_ssh_setups WHERE id=?", setup_id)
    try:
        keys = json.loads(row.get("host_keys") or "[]")
    except ValueError:
        keys = []
    body = {
        "id": row["id"],
        "host": row.get("host") or "",
        "address": row.get("address") or "",
        "port": int(row.get("port") or 22),
        "state": row.get("state") or "",
        "message": row.get("message") or "",
        "log": row.get("log") or "",
        "runner": row.get("runner") or "",
        "fingerprint": row.get("fingerprint") or "",
        "hostKeys": [{"type": item.get("type"),
                      "fingerprint": item.get("fingerprint")}
                     for item in keys if isinstance(item, dict)],
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
    }
    if row.get("token_id"):
        token = await runtime.d1_first(
            "SELECT state FROM runner_setup_tokens WHERE id=?",
            row["token_id"])
        body["registered"] = (token or {}).get("state") == "used"
    return runtime.response({"ok": True, "setup": body})


async def helper_stream(path, helper_request, timeout=None):
    import asyncio
    if not path:
        raise RuntimeError("no helper")
    if timeout is None:
        timeout = (55 if helper_request.get("op") == "instance" else 25) * 60
    reader, writer = await asyncio.wait_for(
        asyncio.open_unix_connection(path, limit=1 << 20), 5)
    try:
        writer.write(json.dumps(helper_request).encode("utf-8") + b"\n")
        helper_request.clear()
        await writer.drain()
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            left = deadline - loop.time()
            if left <= 0:
                yield {"exit": -1, "error": "The setup took too long."}
                return
            line = await asyncio.wait_for(reader.readline(), left)
            if not line:
                return
            try:
                message = json.loads(line)
            except ValueError:
                continue
            if isinstance(message, dict):
                yield message
                if "exit" in message or "keys" in message or "error" in message:
                    if "out" not in message:
                        return
    finally:
        writer.close()


async def resolve_host(host):
    import asyncio
    import socket
    literal = public_address(host)
    if literal is not None:
        return [str(literal)]
    loop = asyncio.get_running_loop()
    try:
        answers = await asyncio.wait_for(loop.getaddrinfo(
            host, None, type=socket.SOCK_STREAM), 5)
    except (OSError, asyncio.TimeoutError):
        return []
    return sorted({item[4][0] for item in answers})
