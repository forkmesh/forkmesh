from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import os
import re
import sys
from pathlib import Path

from . import autoscale_policy as policy
from . import autoscaler as _autoscaler
from . import error_log
from . import vultr_guard

PREFIX = "/api/fleet/"
MAX_BODY = 65536
SIGNED_WINDOW_MS = 60_000
RELEASE_RE = re.compile(r"^[0-9a-f]{12}$")
KEY_RE = re.compile(r"^[A-Za-z0-9+/]{42}[AEIMQUYcgkosw048]=$")
PUB_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
RELEASES = Path("/opt/forkmesh-next/app/releases")
RELEASE_ROOT = Path(__file__).resolve().parents[3]
IDS_NODE_KEY_PREFIX = "fmnext-ids-node-key-v1:"
AUTO_NAME_RE = re.compile(r"^[ab][1-9]$")


class ApiError(Exception):
    def __init__(self, status, code):
        super().__init__(code)
        self.status = status
        self.code = code


def ids_node_key(submaster, name):
    if not AUTO_NAME_RE.match(str(name or "")):
        raise ApiError(403, "not_an_autoscaled_name")
    if len(str(submaster or "")) < 32:
        raise ApiError(503, "no_ids_submaster")
    return hmac.new(str(submaster).encode("utf-8"),
                    (IDS_NODE_KEY_PREFIX + name).encode("utf-8"),
                    hashlib.sha256).hexdigest()


def rqlite_auth_json(user, password):
    return json.dumps([{"username": user, "password": password, "perms": ["all"]},
                       {"username": "*", "perms": ["notify"]}])


def _import_release_tools(root=None):
    root = Path(root or RELEASE_ROOT)
    for path in (root / "deploy" / "core", root / "tools"):
        if str(path) not in sys.path:
            sys.path.insert(0, str(path))
    import render
    return render


def build_bundle(record, env, root=None):
    root = Path(root or RELEASE_ROOT)
    render = _import_release_tools(root)
    import next_deploy
    import next_fleet
    public = json.loads((root / "deploy" / "fleet-state.public.json").read_text())
    name = record["name"]
    entry = {"kind": "app" if record["pool"] == "app" else "burst",
             "instance_id": record["instance_id"],
             "public_ipv4": record["public_ipv4"]}
    state = dict(public, autoscaled={name: entry})
    for key, field in (("wg_pubkeys", "wg_pubkey"), ("ids_pubkeys", "ids_pubkey")):
        if record.get(field):
            state.setdefault(key, {})[name] = record[field]
    fleet = render.load_fleet(
        root / "deploy" / "fleet.toml", state=state,
        instances_path=root / "deploy" / "fleet-instances.public.json")
    files, manifest = next_fleet.render_node(fleet, name)
    submaster = str(getattr(env, "AUTOSCALE_IDS_SUBMASTER", "") or "")
    credentials = {}
    if render.ids_enabled(fleet):
        credentials[render.IDS_KEY_CREDENTIAL] = ids_node_key(submaster, name)
    bundle = {"name": name, "role": manifest["role"],
              "files": {path: [base64.b64encode(content.encode()).decode(), mode]
                        for path, (content, mode) in files.items()},
              "scripts": {"files": next_fleet.files_script(
                  manifest, "/tmp/fmnext-join-files.tar.gz"),
                  "start": next_fleet.start_script(manifest, start_ids=False)}}
    if record["pool"] == "app":
        password = str(getattr(env, "RQLITE_PASSWORD", "") or "")
        mesh = str(getattr(env, "FORKMESH_MESH_SECRET", "") or "")
        replica_certsync = str(getattr(env, "AUTOSCALE_CERTSYNC_SECRET", "") or "")
        if len(password) < 16 or len(mesh) < 16 or len(replica_certsync) < 32:
            raise ApiError(503, "bootstrap_secrets_missing")
        user = (fleet.get("rqlite") or {}).get("user", "fmnext")
        for secret in render.app_credential_names(fleet):
            value = str(getattr(env, secret, "") or "")
            if not value:
                raise ApiError(503, "bootstrap_secrets_missing")
            credentials[secret] = value
        credentials["rqlite-auth.json"] = rqlite_auth_json(user, password)
        credentials["FORKMESH_MESH_SECRET"] = mesh
        credentials[render.CERTSYNC_SECRET_NAME] = replica_certsync
        bundle["curlrc"] = 'user = "%s:%s"\n' % (user, password)
        rev = str(getattr(env, "BUILD_REV", "") or os.environ.get(
            "FORKMESH_VAR_BUILD_REV", "")).strip()
        if not RELEASE_RE.match(rev):
            raise ApiError(503, "no_release_rev")
        sha_file = RELEASES / ("%s.tar.gz.sha256" % rev)
        if not sha_file.is_file():
            raise ApiError(503, "release_tarball_missing")
        sha = sha_file.read_text().split()[0]
        bundle["release"] = {"rev": rev, "sha256": sha,
                             "upload": next_deploy.upload_script(rev, sha),
                             "activate": next_deploy.activate_script(
                                 rev, render.served_app_host(fleet), timeout=600,
                                 vault=False)}
    else:
        router = str(getattr(env, "MIRROR_ROUTER_PUBLIC_KEY", "") or "")
        if not PUB_RE.match(router):
            raise ApiError(503, "no_router_public_key")
        bundle["daemon"] = {"script": next_fleet.daemon_script(fleet, name, "next"),
                            "routerPublicKey": router}
    bundle["credentials"] = {k: base64.b64encode(v.encode()).decode()
                             for k, v in credentials.items()}
    return bundle


class FleetApi:
    def __init__(self, runtime, guard_factory=None, clock=_autoscaler.now_ms,
                 bundle_builder=build_bundle):
        self.runtime = runtime
        membership = getattr(getattr(runtime, "durable", None), "membership", None)
        self.store = (_autoscaler.Store(membership.rqlite, clock)
                      if membership else None)
        self.guard_factory = guard_factory or (lambda: vultr_guard.VultrGuard(
            str(getattr(runtime.env, "VULTR_API_KEY", "") or "")))
        self.clock = clock
        self.bundle_builder = bundle_builder

    def _env(self, name):
        return str(getattr(self.runtime.env, name, "") or "")

    async def handle(self, method, path, headers, body, client_ip):
        if self.store is None:
            raise ApiError(404, "not_found")
        if self.runtime.sealed:
            raise ApiError(503, "sealed")
        action = path[len(PREFIX):]
        if action.startswith("release/") and method == "GET":
            return await self.release(action[len("release/"):], headers, client_ip)
        if method != "POST":
            raise ApiError(405, "method_not_allowed")
        try:
            data = json.loads(body or b"{}")
        except ValueError:
            raise ApiError(400, "invalid_json")
        if not isinstance(data, dict):
            raise ApiError(400, "invalid_json")
        if action == "join":
            return 200, await self.join(data, client_ip)
        if action == "joined":
            return 200, await self.joined(data, client_ip)
        if action == "autoscale/settings":
            return 200, await self.put_settings(data)
        if action == "autoscale/status":
            await self.check_signed(policy.STATUS_KIND, data, ("ts", "nonce"))
            return 200, await self.status()
        if action == "autoscale/command":
            return 200, await self.command(data)
        raise ApiError(404, "not_found")

    async def _record_for_token(self, token, states):
        claims = _autoscaler.check_token(self._env("DATA_KEY"), token, self.clock())
        if claims is None:
            raise ApiError(403, "bad_token")
        record = await self.store.node(claims["n"])
        if record is None or record["label"] != claims.get("l"):
            raise ApiError(403, "unknown_node")
        if not hmac.compare_digest(record.get("token_hash") or "",
                                   _autoscaler.token_hash(token)):
            raise ApiError(403, "bad_token")
        if record["state"] not in states:
            raise ApiError(409, "wrong_state")
        return record

    async def _check_source(self, record, client_ip):
        guard = self.guard_factory()
        live = await asyncio.to_thread(guard.get, record)
        if live is None or not client_ip or live.get("main_ip") != client_ip:
            raise ApiError(403, "source_mismatch")
        return live

    async def join(self, data, client_ip):
        token = str(data.get("token") or "")
        record = await self._record_for_token(token, ("booting",))
        if record.get("token_used_ms"):
            raise ApiError(403, "token_used")
        await self._check_source(record, client_ip)
        fields = {"public_ipv4": client_ip, "token_used_ms": self.clock()}
        ids_key = str(data.get("idsPublicKey") or "")
        if not PUB_RE.match(ids_key):
            raise ApiError(400, "bad_ids_key")
        fields["ids_pubkey"] = ids_key
        if record["pool"] == "app":
            wg_key = str(data.get("wgPublicKey") or "")
            if not KEY_RE.match(wg_key):
                raise ApiError(400, "bad_wg_key")
            fields["wg_pubkey"] = wg_key
        claimed = await self.store.execute(
            "UPDATE autoscale_nodes SET token_used_ms=?, updated_ms=? WHERE name=? "
            "AND token_used_ms IS NULL AND state='booting'", self.clock(),
            self.clock(), record["name"])
        if claimed != 1:
            raise ApiError(403, "token_used")
        fields["state"] = "joining"
        await self.store.update_node(record["name"], **fields)
        record.update(fields)
        bundle = await asyncio.to_thread(self.bundle_builder, record,
                                         self.runtime.env)
        if bundle.get("release"):
            await self.store.update_node(record["name"], rev=bundle["release"]["rev"])
        await self.store.audit(record["pool"], "join", "%s joined from %s" % (
            record["name"], client_ip), node=record["name"])
        return dict(bundle, ok=True)

    async def joined(self, data, client_ip):
        token = str(data.get("token") or "")
        record = await self._record_for_token(token, ("joining",))
        if record.get("public_ipv4") != client_ip:
            raise ApiError(403, "source_mismatch")
        if record["pool"] == "runner":
            public = str(data.get("runnerPublicKey") or "")
            if not PUB_RE.match(public):
                raise ApiError(400, "bad_runner_key")
            now = self.clock()
            await self.store.execute(
                "INSERT INTO runner_nodes (name,public_key,registered_by,scope,"
                "allowed_owners,status,created_at,updated_at) VALUES "
                "(?,?,?,?,?,?,?,?) ON CONFLICT(name) DO UPDATE SET "
                "public_key=excluded.public_key, status='active', "
                "allowed_owners='[]', updated_at=excluded.updated_at",
                "fmnext-" + record["name"], public, "autoscaler", "pool", "[]",
                "active", now, now)
            await self.store.update_node(record["name"], runner_pubkey=public)
        await self.store.audit(record["pool"], "joined", "%s finished its setup"
                               % record["name"], node=record["name"])
        return {"ok": True}

    async def release(self, rev, headers, client_ip):
        rev = rev.removesuffix(".tar.gz")
        if not RELEASE_RE.match(rev):
            raise ApiError(404, "not_found")
        token = headers.get("x-fmnext-join-token") or ""
        record = await self._record_for_token(token, ("joining",))
        if record.get("public_ipv4") != client_ip or record.get("rev") != rev:
            raise ApiError(403, "source_mismatch")
        path = RELEASES / ("%s.tar.gz" % rev)
        if not path.is_file():
            raise ApiError(404, "release_tarball_missing")
        data = await asyncio.to_thread(path.read_bytes)
        return 200, data

    async def check_signed(self, kind, data, fields):
        public = _autoscaler.operator_key(self.runtime.env)
        if not public or not policy.verify_document(kind, data, public, fields):
            raise ApiError(403, "bad_signature")
        ts = int(data.get("ts") or 0)
        now = self.clock()
        if abs(now - ts) > SIGNED_WINDOW_MS:
            raise ApiError(403, "stale")
        nonce = str(data.get("nonce") or "")
        if not re.fullmatch(r"[0-9a-f]{16,64}", nonce):
            raise ApiError(400, "bad_nonce")
        await self.store.execute("DELETE FROM autoscale_nonces WHERE expires_ms<?", now)
        inserted = await self.store.execute(
            "INSERT INTO autoscale_nonces (nonce, expires_ms) VALUES (?, ?) "
            "ON CONFLICT(nonce) DO NOTHING", nonce, now + 2 * SIGNED_WINDOW_MS)
        if inserted != 1:
            raise ApiError(403, "replayed")

    async def put_settings(self, data):
        public = _autoscaler.operator_key(self.runtime.env)
        try:
            settings = policy.verify_settings(data, public)
        except policy.PolicyError as error:
            raise ApiError(403, str(error)[:120])
        signed_at = int(data.get("signedAt") or 0)
        if abs(self.clock() - signed_at) > 10 * 60_000:
            raise ApiError(403, "stale")
        document = {"settings": data["settings"], "signedAt": signed_at,
                    "signature": data["signature"]}
        if await self.store.put_settings_doc(document) != 1:
            raise ApiError(409, "not_newer")
        await self.store.audit("-", "settings", json.dumps(settings, sort_keys=True))
        await self.store.ping("settings changed (kill switch %s; app %s%s, runner %s%s)"
                              % ("ON" if settings["killSwitch"] else "off",
                                 "on" if settings["app"]["enabled"] else "off",
                                 " dry run" if settings["app"]["dryRun"] else "",
                                 "on" if settings["runner"]["enabled"] else "off",
                                 " dry run" if settings["runner"]["dryRun"] else ""))
        return {"ok": True}

    async def command(self, data):
        await self.check_signed(policy.COMMAND_KIND, data,
                                ("ts", "nonce", "pool", "direction", "node", "reason"))
        pool, direction = data.get("pool"), data.get("direction")
        node = data.get("node") or None
        if pool not in policy.POOLS or direction not in ("up", "down"):
            raise ApiError(400, "bad_command")
        if node is not None and not AUTO_NAME_RE.match(str(node)):
            raise ApiError(400, "bad_node")
        command_id = os.urandom(12).hex()
        await self.store.execute(
            "INSERT INTO autoscale_commands (id, created_ms, pool, direction, node, "
            "reason) VALUES (?,?,?,?,?,?)", command_id, self.clock(), pool,
            direction, node, str(data.get("reason") or "")[:200])
        await self.store.audit(pool, "command_queued", "%s %s" % (direction, node or ""))
        return {"ok": True, "queued": command_id}

    async def status(self):
        settings_doc = await self.store.settings_doc()
        public = _autoscaler.operator_key(self.runtime.env)
        verified, error = False, "no settings saved yet (both pools off)"
        settings = policy.validate({})
        if settings_doc is not None:
            try:
                settings = policy.verify_settings(settings_doc, public)
                verified, error = True, ""
            except policy.PolicyError as problem:
                error = str(problem)
        nodes = await self.store.nodes()
        now = self.clock()
        pools = {}
        for pool in policy.POOLS:
            state = await self.store.pool_state(pool)
            mine = [n for n in nodes if n["pool"] == pool]
            policy.accrue(state, [{"createdMs": n["created_ms"],
                                   "hourlyCents": n["hourly_cents"]} for n in mine],
                          now)
            pools[pool] = {
                "nodes": [{"name": n["name"], "label": n["label"],
                           "ip": n.get("public_ipv4") or "", "state": n["state"],
                           "createdMs": n["created_ms"],
                           "instanceId": n.get("instance_id") or ""} for n in mine],
                "decision": state.get("decision") or {},
                "lastActionMs": state.get("lastActionMs", 0),
                "spentCents": state["spentCents"], "month": state["month"],
                "signals": state.get("signals") or {},
                "snapshot": {"id": settings[pool].get("snapshotId", "")}}
        lease = await self.store.lease()
        last = int(lease.get("expires_ms") or 0) - _autoscaler.LEASE_MS
        return {"ok": True, "settings": settings, "verified": verified,
                "settingsError": error,
                "service": {"active": now - last < 3 * 60_000, "unit": "cores",
                            "owner": lease.get("holder") or "", "lastTickMs": max(0, last)},
                "pools": pools, "recent": await self.store.recent_audit()}


def client_ip(scope, headers):
    peer = (scope.get("client") or (None,))[0]
    if peer in ("127.0.0.1", "::1"):
        return str(headers.get("cf-connecting-ip") or "").strip()
    return str(peer or "")


def response_json(status, body):
    return status, json.dumps(body, sort_keys=True).encode(), "application/json"


async def dispatch(api, method, path, headers, body, ip):
    try:
        status, payload = await api.handle(method, path, headers, body, ip)
    except ApiError as error:
        return response_json(error.status, {"ok": False, "error": error.code})
    except vultr_guard.GuardRefused as error:
        print("fleet api: guard refused: %s" % error, flush=True)
        return response_json(403, {"ok": False, "error": "guard_refused"})
    except Exception:
        error_log.report("fleet api: %s %s -> 500" % (method, path))
        return response_json(500, {"ok": False, "error": "internal"})
    if isinstance(payload, (bytes, bytearray)):
        return status, bytes(payload), "application/gzip"
    return response_json(status, payload)

