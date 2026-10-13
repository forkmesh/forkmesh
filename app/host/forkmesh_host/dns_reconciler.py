"""Leaderless DNS health for app.next / world.next, inside the app process.

Replaces the operator desktop's ``fmnext-dns-health`` loop. Enabled with
``FORKMESH_DNS_RECONCILER=on`` (``dry`` = vote and decide, log what it
would change, never call Cloudflare; the fleet default) in cluster mode. The
Cloudflare token and zone would be the vault values ``NEXT_DNS_API_TOKEN`` /
``NEXT_DNS_ZONE_ID``, living only in this process's memory while unsealed.
They are deliberately NOT in the vault (audit 2026-09-24: the only token can
edit the whole forkmesh.com zone); without them the reconciler only votes and
logs once, and never calls Cloudflare. A sealed node neither votes nor acts.

Every 30 s each unsealed node:

1. probes every core node (``FORKMESH_DNS_TARGETS``: ``n1=IP,...``) with a
   TLS-verified ``GET https://app.next.forkmesh.com/readyz`` connected to
   that node's IP, plus ``https://nN.next.forkmesh.com/readyz`` for the
   record's detail;
2. turns the results into votes with hysteresis (dns_rules.VoterState:
   down after 3 failures, up after 2 minutes healthy) and upserts its own
   rows in ``dns_health_votes``;
3. if it is the node chosen for this 30 s interval -- round-robin over the
   live members, the same computation on every node, no leader -- it reads
   the fresh votes of all live voters and the live A records, and applies at
   most one change (dns_rules.decide) when a strict majority agrees: remove a
   down node's A records from app.next and world.next, or re-add a healthy
   one. Before calling Cloudflare it advances ``last_change_ms`` in
   ``dns_reconciler_state`` with a compare-and-set, so at most one change
   happens per 5 minutes cluster-wide even if two nodes act at once.

The hard guards (dns_rules): only app.next / world.next are ever written
(``nN.next`` never), a set is never emptied, TTL 60, DNS only.
"""

from __future__ import annotations

import asyncio
import re
import contextlib
import json
import os
import time

from . import cluster as _cluster
from . import dns_rules

CLOUDFLARE_API = "https://api.cloudflare.com/client/v4"
VOTE_FRESH_MS = 3 * int(dns_rules.PROBE_INTERVAL * 1000)
STATE_KEY = "last_change_ms"


REPLICA_RE = re.compile(r"^a[1-9]$")


def parse_targets(text):
    """``n1=203.0.113.1,n2=...`` -> {node: ip} (IPv4 only)."""
    targets = {}
    for item in str(text or "").split(","):
        name, sep, ip = item.partition("=")
        name, ip = name.strip(), ip.strip()
        if sep and name and ip:
            targets[name] = dns_rules.check_ipv4(ip)
    return targets


class CloudflareDns:
    """The three Cloudflare DNS calls the reconciler needs, guarded."""

    def __init__(self, token, zone_id, client=None):
        if not zone_id or not all(c in "0123456789abcdef" for c in zone_id):
            raise dns_rules.DnsGuardError("NEXT_DNS_ZONE_ID must be the "
                                          "32-hex zone id")
        import httpx
        self.zone = zone_id
        self._client = client or httpx.AsyncClient(
            base_url=CLOUDFLARE_API, timeout=15.0,
            headers={"authorization": "Bearer " + token})

    async def _call(self, method, path, body=None):
        response = await self._client.request(method, path, json=body)
        payload = response.json()
        if response.status_code >= 400 or not payload.get("success", True):
            raise RuntimeError("cloudflare %s %s: HTTP %d" % (
                method, path.split("?")[0], response.status_code))
        return payload

    async def records(self, name):
        name = dns_rules.check_reconciler_name(name)
        payload = await self._call(
            "GET", "/zones/%s/dns_records?type=A&name=%s&per_page=100"
            % (self.zone, name))
        return [r for r in payload.get("result") or []
                if str(r.get("name", "")).lower() == name]

    async def create(self, name, ip):
        name = dns_rules.check_reconciler_name(name)
        body = dict(dns_rules.record_body(name, ip),
                    comment="ForkMesh Next (node DNS reconciler)")
        return await self._call("POST", "/zones/%s/dns_records" % self.zone,
                                body)

    async def delete(self, record_id):
        live = (await self._call("GET", "/zones/%s/dns_records/%s" % (
            self.zone, record_id))).get("result") or {}
        dns_rules.check_reconciler_name(live.get("name"))
        return await self._call("DELETE", "/zones/%s/dns_records/%s" % (
            self.zone, record_id))

    async def aclose(self):
        await self._client.aclose()


class DnsReconciler:
    def __init__(self, node_id, membership, targets, shared_names, *,
                 credentials, sealed=lambda: False, mode="on",
                 prober=None, cloudflare_factory=None, clock=None,
                 interval=dns_rules.PROBE_INTERVAL, node_domain=None):
        self.node_id = node_id
        self.membership = membership
        self.targets = dict(targets)
        self.shared_names = [dns_rules.check_reconciler_name(n)
                             for n in shared_names]
        self.credentials = credentials       # () -> (token, zone) | None
        self.sealed = sealed
        self.mode = mode
        self.prober = prober or self._probe
        self.cloudflare_factory = cloudflare_factory or CloudflareDns
        self.clock = clock or (lambda: time.time_ns() // 1_000_000)
        self.interval_ms = int(interval * 1000)
        self.node_domain = node_domain or dns_rules.ZONE_APEX
        self.voter = dns_rules.VoterState()
        self._task = None
        self._warned_no_token = False
        self.last = {}
        self.stats = {"rounds": 0, "acted": 0, "changes": 0,
                      "cas_lost": 0, "errors": 0}

    # -- probing ----------------------------------------------------------
    async def _probe(self, target, ip):
        app_host = self.shared_names[0]
        if REPLICA_RE.match(target):
            ok = await asyncio.to_thread(dns_rules.probe, ip, app_host)
            return ok, {"app": ok, "node": ok}
        node_host = "%s.%s" % (target, self.node_domain)
        app_ok, node_ok = await asyncio.gather(
            asyncio.to_thread(dns_rules.probe, ip, app_host),
            asyncio.to_thread(dns_rules.probe, ip, node_host))
        return app_ok, {"app": app_ok, "node": node_ok}

    async def refresh_replicas(self):
        try:
            rows = await self.membership.rqlite.query(
                "SELECT name, public_ipv4 FROM autoscale_nodes WHERE pool='app' "
                "AND state='serving' AND created_by='autoscaler'", level="weak")
        except Exception:
            return
        serving = {}
        for row in rows or []:
            name = str(row.get("name") or "")
            if not REPLICA_RE.match(name):
                continue
            try:
                serving[name] = dns_rules.check_ipv4(str(row.get("public_ipv4") or ""))
            except Exception:
                continue
        for name in [n for n in self.targets if REPLICA_RE.match(n)]:
            if name not in serving:
                del self.targets[name]
        self.targets.update(serving)

    async def probe_all(self, now):
        await self.refresh_replicas()
        names = sorted(self.targets)
        results = await asyncio.gather(*[
            self.prober(name, self.targets[name]) for name in names])
        votes = {}
        for name, (ok, detail) in zip(names, results):
            votes[name] = (self.voter.observe(name, ok, now / 1000.0), detail)
        return votes

    # -- rqlite -------------------------------------------------------------
    @property
    def rqlite(self):
        return self.membership.rqlite

    async def publish(self, votes, now):
        statements = [[
            "INSERT INTO dns_health_votes(voter, target, healthy, "
            "observed_ms, detail) VALUES(?, ?, ?, ?, ?) "
            "ON CONFLICT(voter, target) DO UPDATE SET "
            "healthy=excluded.healthy, observed_ms=excluded.observed_ms, "
            "detail=excluded.detail",
            self.node_id, target, 1 if vote else 0, now,
            json.dumps(detail, separators=(",", ":"))]
            for target, (vote, detail) in votes.items() if vote is not None]
        if statements:
            await self.rqlite.execute(statements, transaction=False)

    async def read_votes(self, now):
        rows = await self.rqlite.query(
            "SELECT voter, target, healthy, observed_ms FROM "
            "dns_health_votes WHERE observed_ms >= ?", now - VOTE_FRESH_MS,
            level="weak")
        votes = {}
        for row in rows:
            votes.setdefault(row["voter"], {})[row["target"]] = \
                bool(int(row["healthy"]))
        return votes

    async def last_change(self):
        rows = await self.rqlite.query(
            "SELECT value FROM dns_reconciler_state WHERE name=?", STATE_KEY,
            level="strong")
        if not rows:
            return None
        value = int(rows[0]["value"] or 0)
        return value or None

    async def claim_change(self, previous, now):
        """Compare-and-set last_change_ms: True for exactly one claimant."""
        await self.rqlite.execute([[
            "INSERT INTO dns_reconciler_state(name, value, updated_ms) "
            "VALUES(?, '0', ?) ON CONFLICT(name) DO NOTHING", STATE_KEY,
            now]], transaction=False)
        results = await self.rqlite.execute([[
            "UPDATE dns_reconciler_state SET value=?, updated_ms=? "
            "WHERE name=? AND value=?", str(int(now)), now, STATE_KEY,
            str(int(previous or 0))]], transaction=False)
        try:
            return int(results[0].get("rows_affected") or 0) == 1
        except (IndexError, AttributeError, TypeError, ValueError):
            return False

    # -- acting ---------------------------------------------------------------
    def actor(self, now):
        live = self.membership.active
        index = now // self.interval_ms
        return _cluster.rotation_owner("dns", "reconciler", live, index)

    async def act(self, now):
        creds = self.credentials()
        if not creds:
            # No token in the vault (the default since the 2026-09-24
            # audit): keep voting, never call Cloudflare. Say so once.
            if not self._warned_no_token:
                self._warned_no_token = True
                print("dns: NEXT_DNS_API_TOKEN/NEXT_DNS_ZONE_ID not in the "
                      "vault; voting only, DNS changes stay manual "
                      "(tools/next_dns.py)", flush=True)
            return None
        live = tuple(self.membership.active)
        votes = await self.read_votes(now)
        cloudflare = self.cloudflare_factory(*creds)
        try:
            records = {name: await cloudflare.records(name)
                       for name in self.shared_names}
            in_dns = {name: {r.get("content") for r in recs}
                      for name, recs in records.items()}
            previous = await self.last_change()
            decision = dns_rules.decide(
                votes, live, self.targets, in_dns, now / 1000.0,
                None if previous is None else previous / 1000.0)
            self.last["decision"] = decision
            if decision is None:
                return None
            action, target = decision
            ip = self.targets[target]
            plans = {}
            for name, recs in records.items():
                wanted = set(in_dns[name])
                if action == "remove":
                    wanted.discard(ip)
                else:
                    wanted.add(ip)
                # Raises on an empty set: never empty a record set.
                plans[name] = dns_rules.plan_set(name, recs, wanted)
            print("dns: %s %s (%s) by %s, votes %s" % (
                action, target, ip, self.node_id, json.dumps(
                    {v: votes.get(v, {}).get(target) for v in live})),
                flush=True)
            if self.mode != "on":
                print("dns: [dry] not changed", flush=True)
                return decision
            if not await self.claim_change(previous, now):
                self.stats["cas_lost"] += 1
                return None
            for name, changes in plans.items():
                for kind, change_ip, record in changes:
                    if kind == "delete":
                        await cloudflare.delete(record["id"])
                    elif kind == "create":
                        await cloudflare.create(name, change_ip)
                    # "update" (TTL/proxied drift) is left to next_dns.py
                    # ensure: the reconciler only adds and removes.
                    self.stats["changes"] += 1
            return decision
        finally:
            close = getattr(cloudflare, "aclose", None)
            if close is not None:
                with contextlib.suppress(Exception):
                    await close()

    async def round(self):
        """One probe/vote/act round. Returns the decision taken, if any."""
        if self.sealed() or not self.membership.hosting:
            return None
        now = self.clock()
        self.stats["rounds"] += 1
        votes = await self.probe_all(now)
        self.last = {"at": now, "votes": {t: v for t, (v, _) in
                                          votes.items()}}
        await self.publish(votes, now)
        if self.actor(now) != self.node_id:
            return None
        self.stats["acted"] += 1
        return await self.act(now)

    async def loop(self):
        await asyncio.sleep(5.0)
        while True:
            started = time.monotonic()
            try:
                await self.round()
            except Exception as error:
                self.stats["errors"] += 1
                print("dns: round failed: %s" % (type(error).__name__
                                                  if not isinstance(
                                                      error,
                                                      dns_rules.DnsGuardError)
                                                  else error), flush=True)
            elapsed = time.monotonic() - started
            await asyncio.sleep(max(1.0, self.interval_ms / 1000.0 - elapsed))

    def start(self):
        self._task = asyncio.get_running_loop().create_task(self.loop())

    async def stop(self):
        if self._task is not None:
            self._task.cancel()
            with contextlib.suppress(BaseException):
                await self._task


def from_runtime(runtime):
    """The reconciler for a cluster host, or None when it is off."""
    environ = runtime.config.environ if hasattr(runtime, "config") else {}
    mode = str((environ or {}).get("FORKMESH_DNS_RECONCILER")
               or os.environ.get("FORKMESH_DNS_RECONCILER") or "off").lower()
    if mode not in ("on", "dry", "1"):
        return None
    membership = getattr(runtime.durable, "membership", None)
    if membership is None:
        print("dns: reconciler needs cluster mode; off", flush=True)
        return None

    def get(name):
        return (environ or {}).get(name) or os.environ.get(name) or ""
    targets = parse_targets(get("FORKMESH_DNS_TARGETS"))
    names = [n for n in get("FORKMESH_DNS_NAMES").split(",") if n.strip()] \
        or sorted(dns_rules.RECONCILER_NAMES)
    if not targets:
        print("dns: FORKMESH_DNS_TARGETS is empty; reconciler off",
              flush=True)
        return None

    def credentials():
        env = runtime.env
        token = getattr(env, "NEXT_DNS_API_TOKEN", None)
        zone = getattr(env, "NEXT_DNS_ZONE_ID", None)
        if not isinstance(token, str) or not isinstance(zone, str) or \
                not token or not zone:
            return None
        return token, zone

    names = sorted(names, key=lambda n: 0 if n.startswith("app.") else 1)
    return DnsReconciler(
        membership.node_id, membership, targets, names,
        credentials=credentials, sealed=lambda: runtime.sealed,
        mode="on" if mode in ("on", "1") else "dry")
