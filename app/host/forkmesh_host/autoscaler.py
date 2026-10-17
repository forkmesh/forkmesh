from __future__ import annotations

import asyncio
import base64
import contextlib
import hashlib
import hmac
import json
import os
import re
import time

from . import autoscale_policy as policy
from . import cluster as _cluster
from . import vultr_guard

TICK_SECONDS = 60.0
METRICS_SECONDS = 30.0
LEASE_MS = 150_000
TOKEN_TTL_MS = 30 * 60_000
BOOT_TIMEOUT_MS = 25 * 60_000
JOIN_TIMEOUT_MS = 35 * 60_000
DRAIN_MS = 180_000
METRICS_FRESH_MS = 120_000
AUDIT_RETAIN_MS = 30 * 86_400_000
OWNER_KEY = ("ForkMeshAutoscaler", "tick-v1")
JOIN_KEY_LABEL = b"fmnext-autoscale-join-v1"
APP_WG_OFFSET = 20
NODE_RE = re.compile(r"^[ab][1-9]$")
TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{16,400}\.[0-9a-f]{64}$")
STATES = ("creating", "booting", "joining", "serving", "draining", "removing")
NODE_COLUMNS = ("name", "pool", "label", "instance_id", "created_by", "state",
                "state_since_ms", "public_ipv4", "wg_ip", "wg_pubkey",
                "ids_pubkey", "runner_pubkey", "token_hash", "token_expires_ms",
                "token_used_ms", "rev", "hourly_cents", "created_ms",
                "updated_ms", "detail")


def now_ms():
    return time.time_ns() // 1_000_000


def _b64(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _unb64(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def join_key(data_key):
    if not data_key:
        raise RuntimeError("sealed: no DATA_KEY")
    return hmac.new(str(data_key).encode(), JOIN_KEY_LABEL, hashlib.sha256).digest()


def mint_token(data_key, name, label, expires_ms, nonce=None):
    body = _b64(json.dumps({"n": name, "l": label, "e": int(expires_ms),
                            "r": nonce or os.urandom(12).hex()},
                           separators=(",", ":"), sort_keys=True).encode())
    mac = hmac.new(join_key(data_key), body.encode(), hashlib.sha256).hexdigest()
    return body + "." + mac


def check_token(data_key, token, now=None):
    token = str(token or "")
    if not TOKEN_RE.match(token):
        return None
    body, mac = token.split(".", 1)
    expected = hmac.new(join_key(data_key), body.encode(), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, mac):
        return None
    try:
        claims = json.loads(_unb64(body))
    except ValueError:
        return None
    if not isinstance(claims, dict) or not NODE_RE.match(str(claims.get("n") or "")):
        return None
    if int(claims.get("e") or 0) < (now_ms() if now is None else now):
        return None
    return claims


def token_hash(token):
    return hashlib.sha256(str(token).encode()).hexdigest()


def wg_ip_for(name):
    return "10.77.0.%d" % (APP_WG_OFFSET + int(name[1:]))


def user_data(token, name, app_host):
    return ("#!/bin/sh\n"
            "exec /usr/bin/python3 -I /opt/forkmesh-next/bin/fmnext-join "
            "--name %s --app-host %s --token %s >>/var/log/fmnext-join.log 2>&1\n"
            % (name, app_host, token))


class Store:
    def __init__(self, rqlite, clock=None):
        self.rqlite = rqlite
        self.clock = clock or now_ms

    async def query(self, sql, *params, level="weak"):
        return await self.rqlite.query(sql, *params, level=level)

    async def execute(self, sql, *params):
        results = await self.rqlite.execute([[sql, *params]], transaction=False)
        return int((results or [{}])[0].get("rows_affected") or 0)

    async def nodes(self):
        rows = await self.query("SELECT %s FROM autoscale_nodes ORDER BY name"
                                % ",".join(NODE_COLUMNS))
        return [dict(r) for r in rows or []]

    async def node(self, name):
        rows = await self.query("SELECT %s FROM autoscale_nodes WHERE name=?"
                                % ",".join(NODE_COLUMNS), name, level="strong")
        return dict(rows[0]) if rows else None

    async def insert_node(self, record):
        columns = [c for c in NODE_COLUMNS if c in record]
        return await self.execute(
            "INSERT INTO autoscale_nodes (%s) VALUES (%s)" % (
                ",".join(columns), ",".join("?" * len(columns))),
            *[record[c] for c in columns])

    async def update_node(self, name, expect_state=None, **fields):
        fields["updated_ms"] = self.clock()
        if "state" in fields:
            fields["state_since_ms"] = fields["updated_ms"]
        sets = ",".join("%s=?" % k for k in fields)
        sql = "UPDATE autoscale_nodes SET %s WHERE name=?" % sets
        params = list(fields.values()) + [name]
        if expect_state is not None:
            sql += " AND state=?"
            params.append(expect_state)
        return await self.execute(sql, *params)

    async def delete_node(self, name):
        return await self.execute("DELETE FROM autoscale_nodes WHERE name=?", name)

    async def settings_doc(self):
        rows = await self.query("SELECT doc, signed_at FROM autoscale_settings "
                                "WHERE id=1", level="strong")
        if not rows:
            return None
        try:
            return json.loads(rows[0]["doc"])
        except (ValueError, TypeError):
            return {}

    async def put_settings_doc(self, document):
        signed_at = int(document["signedAt"])
        return await self.execute(
            "INSERT INTO autoscale_settings (id, doc, signed_at, updated_ms) "
            "VALUES (1, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET doc=excluded.doc, "
            "signed_at=excluded.signed_at, updated_ms=excluded.updated_ms "
            "WHERE excluded.signed_at > autoscale_settings.signed_at",
            json.dumps(document, sort_keys=True), signed_at, now_ms())

    async def pool_state(self, pool):
        rows = await self.query("SELECT data FROM autoscale_state WHERE pool=?",
                                pool, level="strong")
        state = policy.empty_pool_state()
        if rows:
            with contextlib.suppress(ValueError, TypeError):
                state.update(json.loads(rows[0]["data"]))
        return state

    async def put_pool_state(self, pool, state):
        return await self.execute(
            "INSERT INTO autoscale_state (pool, data, updated_ms) VALUES (?,?,?) "
            "ON CONFLICT(pool) DO UPDATE SET data=excluded.data, "
            "updated_ms=excluded.updated_ms",
            pool, json.dumps(state, sort_keys=True), now_ms())

    async def acquire_lease(self, holder, now=None):
        now = now or now_ms()
        await self.execute("INSERT INTO autoscale_lease (id, holder, expires_ms) "
                           "VALUES (1, '', 0) ON CONFLICT(id) DO NOTHING")
        changed = await self.execute(
            "UPDATE autoscale_lease SET holder=?, expires_ms=? WHERE id=1 AND "
            "(holder=? OR expires_ms<?)", holder, now + LEASE_MS, holder, now)
        return changed == 1

    async def release_lease(self, holder):
        return await self.execute("UPDATE autoscale_lease SET expires_ms=0 WHERE "
                                  "id=1 AND holder=?", holder)

    async def lease(self):
        rows = await self.query("SELECT holder, expires_ms FROM autoscale_lease "
                                "WHERE id=1")
        return dict(rows[0]) if rows else {}

    async def audit(self, pool, event, detail, ok=True, node=None):
        await self.execute(
            "INSERT INTO autoscale_audit (id, ts, pool, event, node, ok, detail) "
            "VALUES (?,?,?,?,?,?,?)", os.urandom(12).hex(), now_ms(), pool,
            event, node, 1 if ok else 0, str(detail)[:1000])
        await self.execute("DELETE FROM autoscale_audit WHERE ts < ?",
                           now_ms() - AUDIT_RETAIN_MS)

    async def recent_audit(self, limit=30):
        rows = await self.query("SELECT ts, pool, event, node, ok, detail FROM "
                                "autoscale_audit ORDER BY ts DESC LIMIT %d"
                                % int(limit))
        return [{"ts": r["ts"], "pool": r["pool"], "event": r["event"],
                 "node": r.get("node"), "ok": bool(r["ok"]),
                 "detail": r.get("detail") or ""} for r in rows or []]

    async def ping(self, summary, detail=None):
        await self.execute(
            "INSERT INTO security_incidents (id,node_id,created_ms,severity,"
            "detector,summary,detail) VALUES (?,?,?,?,?,?,?)",
            os.urandom(12).hex(), "autoscaler", now_ms(), "alert", "autoscaler",
            summary[:300], json.dumps(detail or {}, sort_keys=True)[:4000])

    async def commands(self):
        rows = await self.query("SELECT id, pool, direction, node, reason FROM "
                                "autoscale_commands WHERE done_ms IS NULL "
                                "ORDER BY created_ms LIMIT 5", level="strong")
        return [dict(r) for r in rows or []]

    async def finish_command(self, command_id, result):
        return await self.execute("UPDATE autoscale_commands SET done_ms=?, "
                                  "result=? WHERE id=? AND done_ms IS NULL",
                                  now_ms(), str(result)[:500], command_id)

    async def metrics(self, fresh_after):
        rows = await self.query("SELECT node_id, ms, data FROM host_metrics "
                                "WHERE ms>=?", fresh_after)
        out = {}
        for row in rows or []:
            with contextlib.suppress(ValueError, TypeError):
                out[row["node_id"]] = json.loads(row["data"])
        return out

    async def put_metrics(self, node, snapshot):
        return await self.execute(
            "INSERT INTO host_metrics (node_id, ms, data) VALUES (?,?,?) "
            "ON CONFLICT(node_id) DO UPDATE SET ms=excluded.ms, data=excluded.data",
            node, now_ms(), json.dumps(snapshot, sort_keys=True))

    async def runner_queue(self, targets, now):
        queue = await self.query(
            "SELECT COUNT(*) AS n, MIN(created_at) AS oldest FROM org_agent_jobs "
            "WHERE status='queued' AND target_node IN (%s)" % ",".join(
                "?" * len(targets)), *targets)
        rows = await self.query(
            "SELECT r.name AS name, r.last_seen_at AS seen, (SELECT COUNT(*) "
            "FROM runner_job_leases l WHERE l.runner=r.name AND l.lease_id!='' "
            "AND l.expires_at>=?) AS leases FROM runner_nodes r WHERE "
            "r.scope='pool' AND r.status='active'", now)
        busy, free = {}, False
        for row in rows or []:
            name = str(row.get("name") or "")
            leases = int(row.get("leases") or 0)
            if re.fullmatch(r"fmnext-b[1-9]", name):
                busy[name[len("fmnext-"):]] = leases > 0
            elif leases == 0 and now - int(row.get("seen") or 0) < 300_000:
                free = True
        first = (queue or [{}])[0]
        oldest = int(first.get("oldest") or 0)
        return {"queued": int(first.get("n") or 0),
                "oldestWaitS": max(0, (now - oldest) // 1000) if oldest else 0,
                "busy": busy, "alwaysOnFree": free}


def operator_key(env):
    return str(getattr(env, "FORKMESH_IDS_OPERATOR_PUBKEY", "") or
               os.environ.get("FORKMESH_IDS_OPERATOR_PUBKEY", "")).strip()



def default_runner_targets(environ=None):
    environ = os.environ if environ is None else environ
    raw = (environ.get("FORKMESH_VAR_RUNNER_POOL_TARGET_NODES")
           or environ.get("RUNNER_POOL_TARGET_NODES") or "")
    targets = [t.strip() for t in raw.split(",") if t.strip()]
    if targets:
        return targets
    peers = _cluster.parse_peers(environ.get("FORKMESH_MESH_PEERS", ""))
    return ["fmnext-%s" % name for name in sorted(peers)]

class Autoscaler:
    def __init__(self, runtime, membership, clock=now_ms, guard_factory=None,
                 app_host=None, runner_targets=None):
        self.runtime = runtime
        self.membership = membership
        self.node_id = membership.node_id
        self.store = Store(membership.rqlite, clock)
        self.clock = clock
        self.guard_factory = guard_factory or (
            lambda: vultr_guard.VultrGuard(self._env("VULTR_API_KEY")))
        self.app_host = app_host or os.environ.get(
            "FORKMESH_AUTOSCALE_APP_HOST", "app.forkmesh.com")
        self.runner_targets = tuple(runner_targets or default_runner_targets())
        self._task = None
        self._metrics_task = None
        self.stats = {"ticks": 0, "owned": 0, "actions": 0, "errors": 0}
        self.last_tick_ms = 0
        self.last_dry_run = {}

    def _env(self, name):
        value = getattr(self.runtime.env, name, "") or ""
        return str(value) if value else ""

    def ready(self):
        return (not self.runtime.sealed
                and getattr(self.membership, "hosting", False)
                and not getattr(self.membership, "replica", False))

    def owner(self, now=None):
        return _cluster.rotation_owner(*OWNER_KEY, self.membership.active,
                                       _cluster.rotation_index(now))

    async def settings(self):
        document = await self.store.settings_doc()
        if document is None:
            return policy.validate({}), False, "no settings saved yet (both pools off)"
        try:
            return (policy.verify_settings(document, operator_key(self.runtime.env)),
                    True, "")
        except policy.PolicyError as error:
            return policy.validate({}), False, str(error)

    async def event(self, pool, event, summary, ok=True, node=None, ping=True):
        await self.store.audit(pool, event, summary, ok, node)
        print("autoscaler: %s %s: %s" % (pool, event, summary), flush=True)
        if ping:
            with contextlib.suppress(Exception):
                await self.store.ping("%s pool: %s" % (pool, summary),
                                      {"event": event, "node": node, "by": self.node_id})

    async def tick(self, now=None):
        now = now or self.clock()
        self.stats["ticks"] += 1
        if not await self.store.acquire_lease(self.node_id, now):
            return {"skipped": "lease held elsewhere"}
        self.stats["owned"] += 1
        self.last_tick_ms = now
        try:
            settings, verified, error = await self.settings()
            report = {"verified": verified, "error": error}
            if settings["killSwitch"]:
                report["killSwitch"] = True
                for pool in policy.POOLS:
                    state = await self.store.pool_state(pool)
                    state["decision"] = {"action": "none", "atMs": now,
                                         "reason": "kill switch on"}
                    await self.store.put_pool_state(pool, state)
                return report
            nodes = await self.store.nodes()
            for record in nodes:
                await self.advance(record, settings, now)
            await self.run_commands(settings, verified, now)
            for pool in policy.POOLS:
                report[pool] = await self.decide(pool, settings, verified, now)
            return report
        finally:
            await self.store.release_lease(self.node_id)

    async def decide(self, pool, settings, verified, now):
        conf = settings[pool]
        state = await self.store.pool_state(pool)
        nodes = [n for n in await self.store.nodes() if n["pool"] == pool]
        active = [n for n in nodes if n["state"] not in ("removing",)]
        policy.accrue(state, [{"createdMs": n["created_ms"],
                               "hourlyCents": n["hourly_cents"]} for n in nodes], now)
        hourly = max([float(n["hourly_cents"] or 0) for n in nodes]
                     + [round(500 / policy.HOURS_PER_MONTH, 4)])
        if pool == "app":
            signals = {}
            if conf["enabled"]:
                serving = {n["name"] for n in active if n["state"] == "serving"}
                metrics = await self.store.metrics(now - METRICS_FRESH_MS)
                samples = [m for node, m in metrics.items()
                           if node in serving or not NODE_RE.match(node)]
                signals = policy.aggregate_app_signals(samples)
            decision = policy.decide_app(conf, state, signals, len(active), hourly, now)
        else:
            observation = {}
            if conf["enabled"] or active:
                observation = await self.store.runner_queue(self.runner_targets, now)
            view = [{"name": n["name"], "state": n["state"]} for n in active]
            decision = policy.decide_runner(conf, state, observation, view, hourly, now)
        busy = [n for n in active if n["state"] not in ("serving",)]
        if not verified or not conf["enabled"]:
            decision = dict(decision, action="none", reason="pool disabled" if verified
                            else "settings not verified")
        elif decision["action"] != "none" and busy:
            decision = dict(decision, action="none", reason=decision["reason"]
                            + "; waiting for %s to finish (%s)" % (
                                busy[0]["name"], busy[0]["state"]))
        state["decision"] = decision
        if decision["action"] == "none":
            self.last_dry_run.pop(pool, None)
        else:
            if conf["dryRun"]:
                key = (decision["action"], decision.get("node") or "")
                if self.last_dry_run.get(pool) != key:
                    await self.store.audit(pool, "decision_dry_run", "%s: %s" % (
                        decision["action"], decision["reason"]))
                    self.last_dry_run[pool] = key
            else:
                ok = await self.act(pool, decision["action"], decision["reason"],
                                    settings, now, node=decision.get("node"))
                if ok:
                    state["lastActionMs"] = now
                    state.setdefault("actions", []).append(now)
        await self.store.put_pool_state(pool, state)
        return decision

    async def run_commands(self, settings, verified, now):
        for command in await self.store.commands():
            pool, direction = command["pool"], command["direction"]
            if pool not in policy.POOLS or direction not in ("up", "down"):
                await self.store.finish_command(command["id"], "refused: bad command")
                continue
            if not verified:
                await self.store.finish_command(command["id"],
                                                "refused: settings not verified")
                continue
            conf = settings[pool]
            state = await self.store.pool_state(pool)
            nodes = [n for n in await self.store.nodes() if n["pool"] == pool
                     and n["state"] != "removing"]
            policy.accrue(state, [{"createdMs": n["created_ms"],
                                   "hourlyCents": n["hourly_cents"]} for n in nodes], now)
            reason = "manual: %s" % (command.get("reason") or "operator")
            if direction == "up":
                if len(nodes) >= min(conf["max"], policy.MAX_NODES) and \
                        len(nodes) >= policy.MAX_NODES:
                    result = "refused: at the hard limit of %d" % policy.MAX_NODES
                elif state["spentCents"] + 1 >= conf["monthlyCapCents"]:
                    result = "refused: at the monthly spend cap"
                elif any(n["state"] not in ("serving",) for n in nodes):
                    result = "refused: another node is still changing state"
                else:
                    ok = await self.act(pool, "up", reason, settings, now)
                    result = "started" if ok else "failed"
            else:
                target = command.get("node") or None
                ok = await self.act(pool, "down", reason, settings, now, node=target)
                result = "started" if ok else "failed"
            if result == "started":
                state["lastActionMs"] = now
                state.setdefault("actions", []).append(now)
                await self.store.put_pool_state(pool, state)
            await self.store.finish_command(command["id"], result)
            await self.store.audit(pool, "command", "%s %s: %s" % (
                direction, command.get("node") or "", result))

    async def act(self, pool, action, reason, settings, now, node=None):
        self.stats["actions"] += 1
        try:
            if action == "up":
                return await self.start_create(pool, reason, settings, now)
            return await self.start_remove(pool, reason, node)
        except Exception as error:
            self.stats["errors"] += 1
            await self.event(pool, "action_failed", "%s failed: %s" % (
                action, str(error)[:300]), ok=False)
            return False

    async def start_create(self, pool, reason, settings, now):
        conf = settings[pool]
        if not conf["snapshotId"]:
            raise RuntimeError("no snapshot published for the %s pool" % pool)
        prefix = "a" if pool == "app" else "b"
        used = {n["name"] for n in await self.store.nodes()}
        name = next((prefix + str(i) for i in range(1, 10)
                     if prefix + str(i) not in used), None)
        if name is None:
            raise RuntimeError("no free %s name" % pool)
        label = vultr_guard.label_for_name(name)
        guard = self.guard_factory()
        hourly = await asyncio.to_thread(guard.plan_hourly_cents, conf["plan"])
        token = mint_token(self._env("DATA_KEY"), name, label, now + TOKEN_TTL_MS)
        record = {"name": name, "pool": pool, "label": label, "instance_id": "",
                  "created_by": vultr_guard.CREATOR, "state": "creating",
                  "state_since_ms": now, "token_hash": token_hash(token),
                  "token_expires_ms": now + TOKEN_TTL_MS, "hourly_cents": hourly,
                  "created_ms": now, "updated_ms": now,
                  "wg_ip": wg_ip_for(name) if pool == "app" else "",
                  "detail": reason[:300]}
        if await self.store.insert_node(record) != 1:
            raise RuntimeError("could not record %s" % name)
        try:
            instance = await asyncio.to_thread(
                guard.create, record, conf["plan"], conf["region"],
                conf["snapshotId"], user_data(token, name, self.app_host))
        except Exception as error:
            await self.store.delete_node(name)
            raise RuntimeError("Vultr create of %s: %s" % (label, error))
        await self.store.update_node(name, instance_id=instance["id"],
                                     state="booting")
        await self.event(pool, "scale_up", "creating %s (%s, %s, %s, %.2f cents/h): %s"
                         % (name, label, conf["plan"], conf["region"], hourly, reason),
                         node=name)
        return True

    async def start_remove(self, pool, reason, node=None):
        nodes = [n for n in await self.store.nodes() if n["pool"] == pool
                 and n["state"] not in ("draining", "removing")]
        if node:
            nodes = [n for n in nodes if n["name"] == node]
        if not nodes:
            raise RuntimeError("no %s node to remove%s" % (
                pool, " named %s" % node if node else ""))
        victim = sorted(nodes, key=lambda n: (-int(n["created_ms"]), n["name"]))[0]
        target = "draining" if victim["state"] == "serving" else "removing"
        await self.store.update_node(victim["name"], state=target,
                                     detail=reason[:300])
        await self.event(pool, "scale_down", "%s %s: %s" % (
            "draining" if target == "draining" else "removing",
            victim["name"], reason), node=victim["name"])
        return True

    async def advance(self, record, settings, now):
        name, state = record["name"], record["state"]
        age = now - int(record["state_since_ms"] or record["created_ms"] or now)
        pool = record["pool"]
        if state == "creating" and age > 5 * 60_000:
            await self.event(pool, "create_unknown", "%s: the Vultr create never "
                             "confirmed an id; check Vultr for a stray %s by hand"
                             % (name, record["label"]), ok=False, node=name)
            await self.store.delete_node(name)
        elif state == "booting" and age > BOOT_TIMEOUT_MS:
            await self.store.update_node(name, state="removing",
                                         detail="never called join")
            await self.event(pool, "join_timeout", "%s never joined; removing it"
                             % name, ok=False, node=name)
        elif state == "joining":
            if pool == "app":
                metrics = await self.store.metrics(now - METRICS_FRESH_MS)
                ready = name in metrics
            else:
                ready = bool(record.get("runner_pubkey"))
            if ready:
                await self.store.update_node(name, state="serving")
                await self.event(pool, "serving", "%s (%s) is serving" % (
                    name, record.get("public_ipv4")), node=name)
            elif age > JOIN_TIMEOUT_MS:
                await self.store.update_node(name, state="removing",
                                             detail="join did not finish")
                await self.event(pool, "join_timeout", "%s did not finish joining; "
                                 "removing it" % name, ok=False, node=name)
        elif state == "draining" and age >= DRAIN_MS:
            await self.store.update_node(name, state="removing")
        elif state == "removing":
            await self.remove(record)

    async def remove(self, record):
        name, pool = record["name"], record["pool"]
        if pool == "app":
            with contextlib.suppress(Exception):
                await self.rqlite_remove(name)
        else:
            await self.store.execute("UPDATE runner_nodes SET status='revoked', "
                                     "updated_at=? WHERE name=?", now_ms(),
                                     "fmnext-" + name)
        if record.get("instance_id"):
            guard = self.guard_factory()
            await asyncio.to_thread(guard.destroy, record)
        await self.store.delete_node(name)
        await self.store.execute("DELETE FROM host_metrics WHERE node_id=?", name)
        await self.event(pool, "destroyed", "%s (%s) destroyed" % (
            name, record["label"]), node=name)

    async def rqlite_remove(self, name):
        rqlite = self.membership.rqlite
        nodes = await rqlite._client.get(rqlite.url + "/nodes?nonvoters&ver=2")
        nodes.raise_for_status()
        payload = nodes.json()
        items = payload.get("nodes", payload) if isinstance(payload, dict) else payload
        if isinstance(items, dict):
            items = [dict(v, id=k) for k, v in items.items()]
        entry = next((n for n in items or [] if n.get("id") == name), None)
        if entry is None:
            return False
        if entry.get("voter", True):
            raise RuntimeError("%s is an rqlite voter; refusing to remove it" % name)
        response = await rqlite._client.request(
            "DELETE", rqlite.url + "/remove", json={"id": name})
        response.raise_for_status()
        return True

    async def loop(self):
        await asyncio.sleep(10)
        while True:
            try:
                if self.ready() and self.owner() == self.node_id:
                    report = await self.tick()
                    if report.get("skipped") is None:
                        print("autoscaler: tick %s" % json.dumps(
                            {k: (v.get("action") if isinstance(v, dict) else v)
                             for k, v in report.items()}, sort_keys=True), flush=True)
            except Exception as error:
                self.stats["errors"] += 1
                print("autoscaler: tick failed: %r" % (error,), flush=True)
            now = time.time()
            await asyncio.sleep(TICK_SECONDS - (now % TICK_SECONDS) + 20)

    def start(self):
        self._task = asyncio.get_running_loop().create_task(self.loop())

    async def stop(self):
        for task in (self._task, self._metrics_task):
            if task is not None:
                task.cancel()
                with contextlib.suppress(BaseException):
                    await task


class MetricsPublisher:
    def __init__(self, runtime, membership, metrics):
        self.runtime = runtime
        self.membership = membership
        self.metrics = metrics
        self.store = Store(membership.rqlite)
        self._task = None

    async def loop(self):
        while True:
            await asyncio.sleep(METRICS_SECONDS)
            if self.runtime.sealed or self.metrics is None:
                continue
            if not (getattr(self.membership, "hosting", False)
                    or getattr(self.membership, "routing", False)):
                continue
            with contextlib.suppress(Exception):
                await self.store.put_metrics(self.membership.node_id,
                                             self.metrics.snapshot())

    def start(self):
        self._task = asyncio.get_running_loop().create_task(self.loop())

    async def stop(self):
        if self._task is not None:
            self._task.cancel()
            with contextlib.suppress(BaseException):
                await self._task


def from_runtime(runtime):
    membership = getattr(getattr(runtime, "durable", None), "membership", None)
    if membership is None or getattr(membership, "rqlite", None) is None:
        return None, None
    publisher = MetricsPublisher(runtime, membership,
                                 getattr(runtime, "host_metrics", None))
    if getattr(membership, "replica", False):
        return None, publisher
    if os.environ.get("FORKMESH_AUTOSCALER", "1") == "0":
        return None, publisher
    return Autoscaler(runtime, membership), publisher
