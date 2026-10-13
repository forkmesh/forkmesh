from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import pytest

HOST = Path(__file__).resolve().parents[1]
if str(HOST) not in sys.path:
    sys.path.insert(0, str(HOST))

from forkmesh_host import cluster  # noqa: E402
from forkmesh_host.host_metrics import HostMetrics, percentile  # noqa: E402

SECRET = "unit-secret-0123456789abcdef"
PEERS = "n1=http://10.77.0.1:8792,n2=http://10.77.0.2:8792,n3=http://10.77.0.3:8792"


def replica_config(**extra):
    env = {"FORKMESH_NODE_ID": "a1", "FORKMESH_MESH_LISTEN": "10.77.0.21:8792",
           "FORKMESH_MESH_SECRET": SECRET,
           "FORKMESH_HOST_DB": "rqlite:http://127.0.0.1:4001",
           "FORKMESH_MESH_PEERS": PEERS, "FORKMESH_CLUSTER_ROLE": "replica"}
    env.update(extra)
    return cluster.ClusterConfig.from_environ(env)


class FakeRqlite:
    def __init__(self, rows):
        self.rows = rows
        self.executed = []

    async def status(self):
        return {"store": {"leader": {"node_id": "n2", "addr": "10.77.0.2:4002"}}}

    async def leader(self):
        return "n2"

    async def execute(self, statements, transaction=True):
        self.executed.append(statements)
        return [{}]

    async def query(self, sql, *params, level="none"):
        return list(self.rows)

    async def aclose(self):
        return None


def rows(now):
    return [{"node_id": n, "mesh_url": "x", "status": "active",
             "heartbeat_ms": now, "epoch": 1} for n in ("n1", "n2", "n3")]


def test_replica_config_needs_the_core_map():
    config = replica_config()
    assert config.replica is True
    assert "a1" in config.peers
    with pytest.raises(RuntimeError):
        replica_config(FORKMESH_MESH_PEERS="")
    core = cluster.ClusterConfig.from_environ({
        "FORKMESH_NODE_ID": "n1", "FORKMESH_MESH_LISTEN": "10.77.0.1:8792",
        "FORKMESH_MESH_SECRET": SECRET,
        "FORKMESH_HOST_DB": "rqlite:http://127.0.0.1:4001",
        "FORKMESH_MESH_PEERS": PEERS})
    assert core.replica is False


def run_view_once(membership):
    async def go():
        task = asyncio.ensure_future(membership._view_loop())
        await asyncio.sleep(0.05)
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        membership._write_ok = 0.0
    asyncio.run(go())


def test_replica_never_hosts_never_heartbeats_and_routes_to_cores():
    membership = cluster.Membership(replica_config())
    fake = FakeRqlite(rows(cluster.now_ms()))
    membership.rqlite = fake
    asyncio.run(membership.heartbeat())
    assert fake.executed == []
    run_view_once(membership)
    assert membership.active == ("n1", "n2", "n3")
    assert membership.routing is True
    assert membership.hosting is False
    assert membership.quorum is True
    owner = membership.owner("ForkMeshRoom", "lobby")
    assert owner in ("n1", "n2", "n3")
    for index in range(20):
        assert cluster.rotation_owner(
            *cluster.CRON_COORDINATOR, membership.active, index) != "a1"
    ok, reasons = asyncio.run(membership.readiness())
    assert ok, reasons


def test_replica_row_is_ignored_even_if_present():
    membership = cluster.Membership(replica_config())
    now = cluster.now_ms()
    membership.rqlite = FakeRqlite(rows(now) + [
        {"node_id": "a1", "mesh_url": "x", "status": "active",
         "heartbeat_ms": now, "epoch": 1}])
    run_view_once(membership)
    assert "a1" not in membership.active


def test_replica_without_live_cores_is_not_ready():
    membership = cluster.Membership(replica_config())
    membership.rqlite = FakeRqlite([])
    run_view_once(membership)
    ok, reasons = asyncio.run(membership.readiness())
    assert not ok and "no_live_core" in reasons


def test_sealed_replica_does_not_route():
    membership = cluster.Membership(replica_config())
    membership.rqlite = FakeRqlite(rows(cluster.now_ms()))
    membership.paused = True
    run_view_once(membership)
    assert membership.routing is False and membership.hosting is False


def test_core_membership_unchanged():
    config = cluster.ClusterConfig.from_environ({
        "FORKMESH_NODE_ID": "n1", "FORKMESH_MESH_LISTEN": "10.77.0.1:8792",
        "FORKMESH_MESH_SECRET": SECRET,
        "FORKMESH_HOST_DB": "rqlite:http://127.0.0.1:4001",
        "FORKMESH_MESH_PEERS": PEERS})
    membership = cluster.Membership(config)
    fake = FakeRqlite(rows(cluster.now_ms()))
    membership.rqlite = fake

    async def go():
        await membership.beat()
        task = asyncio.ensure_future(membership._view_loop())
        await asyncio.sleep(0.05)
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
    asyncio.run(go())
    assert len(fake.executed) == 1
    assert membership.hosting is True and membership.routing is True
    assert "n1" in membership.active


def test_host_metrics_window_percentiles_and_load():
    clock = [1000.0]
    metrics = HostMetrics(window_s=300, clock=lambda: clock[0],
                          loadavg=lambda: (1.6, 1.0, 1.0),
                          cpu_count=lambda: 2)
    for ms in range(1, 101):
        metrics.record("/api/x", ms)
    metrics.record("/readyz", 9999)
    for lag in (1, 2, 300):
        metrics.record_lag(lag)
    clock[0] += 100
    snap = metrics.snapshot()
    assert snap["requests"] == 100
    assert snap["p95Ms"] == 95.0
    assert snap["p50Ms"] in (50.0, 51.0)
    assert snap["rps"] == 1.0
    assert snap["loadPerCpu"] == 0.8
    assert snap["loopLagMaxMs"] == 300.0
    clock[0] += 400
    snap = metrics.snapshot()
    assert snap["requests"] == 0 and snap["p95Ms"] is None
    assert percentile([], 0.9) is None


def test_readyz_metrics_need_a_mesh_signature():
    from forkmesh_host.asgi import HostASGI

    class Durable:
        verifier = cluster.Verifier(SECRET)

    header = cluster.sign(SECRET, "GET", "/readyz?metrics=1").encode()
    scope = {"query_string": b"metrics=1",
             "headers": [(b"x-forkmesh-mesh-auth", header)]}
    assert HostASGI._metrics_allowed(scope, Durable())
    assert not HostASGI._metrics_allowed(scope, Durable())
    wrong = cluster.sign("other-secret-0123456789", "GET",
                         "/readyz?metrics=1").encode()
    assert not HostASGI._metrics_allowed(
        {"query_string": b"metrics=1",
         "headers": [(b"x-forkmesh-mesh-auth", wrong)]}, Durable())
    assert not HostASGI._metrics_allowed(
        {"query_string": b"", "headers": []}, Durable())
    assert not HostASGI._metrics_allowed(scope, object())


def test_autoscaler_incidents_are_labelled_as_fleet_events():
    from forkmesh_host import ids_alerts
    title, _ = ids_alerts.message({"severity": "alert", "node_id": "autoscaler",
                                   "detector": "autoscaler",
                                   "summary": "app pool: a1 created"})
    assert title.startswith("Fleet autoscaler on autoscaler")
    title, _ = ids_alerts.message({"severity": "alert", "node_id": "n1",
                                   "detector": "ssh", "summary": "x"})
    assert title.startswith("Security alert")
