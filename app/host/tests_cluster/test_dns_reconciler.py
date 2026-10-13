"""Leaderless DNS reconciler (forkmesh_host.dns_reconciler) on real rqlite.

Three reconcilers (n1..n3) share one rqlite node, with scripted probe
results and a fake Cloudflare API. Checks: votes go to dns_health_votes,
only the interval's actor acts, a change needs a strict majority of the live
voters, a set is never emptied, at most one change per 5 minutes
cluster-wide (compare-and-set on last_change_ms, even for two actors at
once), nN.next records are never touched, and a sealed node neither votes
nor acts.
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
for path in (HERE.parent / "tests_rqlite", HERE.parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import rqlite_cluster  # noqa: E402

from forkmesh_host import cluster as _cluster  # noqa: E402
from forkmesh_host import dns_reconciler, dns_rules  # noqa: E402

MIGRATION = (HERE.parents[1] / "migrations"
             / "0196_leaderless_cron_and_dns.sql")
APP, WORLD = "app.next.forkmesh.com", "world.next.forkmesh.com"
TARGETS = {"n1": "203.0.113.11", "n2": "203.0.113.12", "n3": "203.0.113.13"}
ZONE = "0123456789abcdef0123456789abcdef"
INTERVAL_MS = 30_000


@pytest.fixture(scope="module")
def rqlite_url():
    if rqlite_cluster.rqlited_binary() is None:
        pytest.skip("rqlited not found")
    import json
    import urllib.request
    with rqlite_cluster.Cluster(1) as cluster:
        url = cluster.leader().http
        statements = [s[s.index("CREATE"):] for s in
                      MIGRATION.read_text().split(";") if "CREATE" in s]
        request = urllib.request.Request(
            url + "/db/execute", data=json.dumps(statements).encode(),
            headers={"content-type": "application/json"})
        with urllib.request.urlopen(request, timeout=10) as response:
            results = json.loads(response.read())["results"]
        assert not any(r.get("error") for r in results), results
        yield url


class FakeCloudflare:
    """Records per name; the same guards as the real client."""

    def __init__(self, records):
        self.records_by_name = {n: list(ips) for n, ips in records.items()}
        self.calls = []

    def factory(self, token, zone):
        assert token == "cf-token" and zone == ZONE
        return self

    async def records(self, name):
        dns_rules.check_reconciler_name(name)
        return [{"id": "%s|%s" % (name, ip), "name": name, "content": ip,
                 "ttl": 60, "proxied": False}
                for ip in self.records_by_name.get(name, [])]

    async def create(self, name, ip):
        dns_rules.check_reconciler_name(name)
        self.calls.append(("create", name, ip))
        self.records_by_name.setdefault(name, []).append(ip)

    async def delete(self, record_id):
        name, ip = record_id.split("|")
        dns_rules.check_reconciler_name(name)
        self.calls.append(("delete", name, ip))
        self.records_by_name[name].remove(ip)


class Member:
    """The slice of cluster.Membership the reconciler reads."""

    def __init__(self, node_id, url, live):
        self.node_id = node_id
        self.rqlite = _cluster.Rqlite(url)
        self.active = tuple(live)
        self.hosting = True


class Clock:
    def __init__(self, ms):
        self.ms = ms

    def __call__(self):
        return self.ms


def make(url, node, probes, cloudflare, clock, live=("n1", "n2", "n3"),
         sealed=False, creds=True):
    async def prober(target, ip):
        ok = probes[target]
        return ok, {"app": ok}
    return dns_reconciler.DnsReconciler(
        node, Member(node, url, live), TARGETS, [APP, WORLD],
        credentials=lambda: ("cf-token", ZONE) if creds else None,
        sealed=lambda: sealed, prober=prober,
        cloudflare_factory=cloudflare.factory, clock=clock)


async def _reset(url):
    rq = _cluster.Rqlite(url)
    await rq.execute([["DELETE FROM dns_health_votes"],
                      ["DELETE FROM dns_reconciler_state"]])
    await rq.aclose()


def actor_for(clock, live=("n1", "n2", "n3")):
    return _cluster.rotation_owner("dns", "reconciler", live,
                                   clock() // INTERVAL_MS)


async def rounds(reconcilers, clock, count, step_ms=INTERVAL_MS):
    decisions = []
    for _ in range(count):
        for reconciler in reconcilers:
            decisions.append((reconciler.node_id, await reconciler.round()))
        clock.ms += step_ms
    return [d for d in decisions if d[1]]


def test_majority_removes_a_down_node_and_readds_it(rqlite_url):
    async def scenario():
        await _reset(rqlite_url)
        clock = Clock(1_000_000_000_000)
        cloudflare = FakeCloudflare({APP: TARGETS.values(),
                                     WORLD: TARGETS.values()})
        probes = {"n1": True, "n2": True, "n3": False}
        nodes = [make(rqlite_url, n, probes, cloudflare, clock)
                 for n in ("n1", "n2", "n3")]
        # Two failures are not enough (hysteresis: 3 in a row).
        assert await rounds(nodes, clock, 2) == []
        assert cloudflare.calls == []
        decided = await rounds(nodes, clock, 2)
        assert [d[1] for d in decided] == [("remove", "n3")]
        assert decided[0][0] == actor_for(Clock(clock.ms - 2 * INTERVAL_MS)) \
            or decided[0][0] == actor_for(Clock(clock.ms - INTERVAL_MS))
        assert sorted(cloudflare.calls) == [("delete", APP, TARGETS["n3"]),
                                            ("delete", WORLD, TARGETS["n3"])]
        # Healthy again: re-added only after 2 minutes healthy AND 5 minutes
        # after the last change.
        probes["n3"] = True
        cloudflare.calls.clear()
        await rounds(nodes, clock, 5)
        assert cloudflare.calls == []           # < 5 min since the removal
        decided = await rounds(nodes, clock, 6)
        assert [d[1] for d in decided] == [("add", "n3")]
        assert sorted(cloudflare.calls) == [("create", APP, TARGETS["n3"]),
                                            ("create", WORLD, TARGETS["n3"])]
        for n in nodes:
            await n.membership.rqlite.aclose()
    asyncio.run(scenario())


def test_a_minority_view_changes_nothing(rqlite_url):
    async def scenario():
        await _reset(rqlite_url)
        clock = Clock(1_100_000_000_000)
        cloudflare = FakeCloudflare({APP: TARGETS.values(),
                                     WORLD: TARGETS.values()})
        # Only n1 cannot reach n2 (a partition on n1's side): 1 of 3 votes.
        views = {"n1": {"n1": True, "n2": False, "n3": True},
                 "n2": {"n1": True, "n2": True, "n3": True},
                 "n3": {"n1": True, "n2": True, "n3": True}}
        nodes = [make(rqlite_url, n, views[n], cloudflare, clock)
                 for n in ("n1", "n2", "n3")]
        assert await rounds(nodes, clock, 8) == []
        assert cloudflare.calls == []
        for n in nodes:
            await n.membership.rqlite.aclose()
    asyncio.run(scenario())


def test_never_empties_a_set_and_never_touches_node_names(rqlite_url):
    async def scenario():
        await _reset(rqlite_url)
        clock = Clock(1_200_000_000_000)
        # Only n1 is in DNS and everyone sees n1 down: keep it anyway.
        cloudflare = FakeCloudflare({APP: [TARGETS["n1"]],
                                     WORLD: [TARGETS["n1"]]})
        probes = {"n1": False, "n2": True, "n3": True}
        nodes = [make(rqlite_url, n, probes, cloudflare, clock)
                 for n in ("n1", "n2", "n3")]
        decided = await rounds(nodes, clock, 6)
        # The healthy n2/n3 are added back (one per 5 minutes) but n1, the
        # last record, is never removed while it is alone.
        assert all(d[1][0] == "add" for d in decided)
        assert ("delete", APP, TARGETS["n1"]) not in cloudflare.calls
        for n in nodes:
            await n.membership.rqlite.aclose()
        with pytest.raises(dns_rules.DnsGuardError):
            dns_rules.plan_set(APP, [], [])
        with pytest.raises(dns_rules.DnsGuardError):
            dns_rules.check_reconciler_name("n1.next.forkmesh.com")
        with pytest.raises(dns_rules.DnsGuardError):
            dns_rules.check_reconciler_name("app.forkmesh.com")
        with pytest.raises(dns_rules.DnsGuardError):
            dns_reconciler.DnsReconciler(
                "n1", Member("n1", rqlite_url, ["n1"]), TARGETS,
                ["n1.next.forkmesh.com"], credentials=lambda: None)
        assert dns_rules.record_body(APP, "203.0.113.11") == {
            "type": "A", "name": APP, "content": "203.0.113.11", "ttl": 60,
            "proxied": False}
    asyncio.run(scenario())


def test_two_actors_at_once_make_one_change(rqlite_url):
    async def scenario():
        await _reset(rqlite_url)
        clock = Clock(1_300_000_000_000)
        cloudflare = FakeCloudflare({APP: TARGETS.values(),
                                     WORLD: TARGETS.values()})
        probes = {"n1": True, "n2": False, "n3": True}
        nodes = {n: make(rqlite_url, n, probes, cloudflare, clock)
                 for n in ("n1", "n2", "n3")}
        for _ in range(3):   # enough failed probes for everyone's vote
            for n in nodes.values():
                await n.probe_all(clock.ms)
            clock.ms += INTERVAL_MS
        for n in nodes.values():
            await n.publish({t: (n.voter.targets[t]["vote"], {})
                             for t in TARGETS}, clock.ms)
        # Both n1 and n3 believe they are the actor (views disagree).
        results = await asyncio.gather(nodes["n1"].act(clock.ms),
                                       nodes["n3"].act(clock.ms))
        assert sorted(r for r in results if r) == [("remove", "n2")]
        assert sorted(cloudflare.calls) == [("delete", APP, TARGETS["n2"]),
                                            ("delete", WORLD, TARGETS["n2"])]
        assert nodes["n1"].stats["cas_lost"] + nodes["n3"].stats["cas_lost"] == 1
        for n in nodes.values():
            await n.membership.rqlite.aclose()
    asyncio.run(scenario())


def test_sealed_or_keyless_node_neither_votes_nor_acts(rqlite_url):
    async def scenario():
        await _reset(rqlite_url)
        clock = Clock(1_400_000_000_000)
        cloudflare = FakeCloudflare({APP: TARGETS.values(),
                                     WORLD: TARGETS.values()})
        probes = {"n1": False, "n2": True, "n3": True}
        sealed = make(rqlite_url, "n2", probes, cloudflare, clock, sealed=True)
        for _ in range(6):
            assert await sealed.round() is None
            clock.ms += INTERVAL_MS
        rq = _cluster.Rqlite(rqlite_url)
        assert await rq.query("SELECT * FROM dns_health_votes", level="strong") == []
        # Unsealed but without the vault's DNS token: votes, never acts.
        keyless = [make(rqlite_url, n, probes, cloudflare, clock, creds=False)
                   for n in ("n1", "n2", "n3")]
        await rounds(keyless, clock, 6)
        assert cloudflare.calls == []
        assert len(await rq.query("SELECT * FROM dns_health_votes",
                                  level="strong")) == 9
        await rq.aclose()
        for n in [sealed] + keyless:
            await n.membership.rqlite.aclose()
    asyncio.run(scenario())
