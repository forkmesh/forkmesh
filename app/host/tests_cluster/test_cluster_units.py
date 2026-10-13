"""Unit checks for forkmesh_host.cluster (no processes)."""

from __future__ import annotations

import sys
from pathlib import Path

HOST = Path(__file__).resolve().parents[1]
if str(HOST) not in sys.path:
    sys.path.insert(0, str(HOST))

from forkmesh_host import cluster  # noqa: E402
from forkmesh_host import js as _js  # noqa: E402
from forkmesh_host.ffi import JsArray, JsObject  # noqa: E402

SECRET = "unit-secret-0123456789abcdef"


def test_sign_and_verify_window_and_replay():
    verifier = cluster.Verifier(SECRET)
    header = cluster.sign(SECRET, "POST", "/_do/A/b/x?y=1", b"body")
    assert not verifier.verify(header, "POST", "/_do/A/b/x?y=1", b"other")
    assert not verifier.verify(header, "GET", "/_do/A/b/x?y=1", b"body")
    assert not verifier.verify(header, "POST", "/_do/A/b/x", b"body")
    assert verifier.verify(header, "POST", "/_do/A/b/x?y=1", b"body")
    assert not verifier.verify(header, "POST", "/_do/A/b/x?y=1", b"body")
    old = cluster.sign(SECRET, "GET", "/p", ts=cluster.now_ms() - 31_000)
    assert not verifier.verify(old, "GET", "/p")
    assert not verifier.verify(None, "GET", "/p")
    assert not verifier.verify("v1:garbage", "GET", "/p")
    assert not cluster.Verifier("").verify(
        cluster.sign("", "GET", "/p"), "GET", "/p")


def test_rendezvous_is_stable_when_other_nodes_leave():
    nodes = ("n1", "n2", "n3")
    moved = 0
    for i in range(300):
        name = "room-%d" % i
        owner = cluster.rendezvous("ForkMeshRoom", name, nodes)
        survivors = tuple(n for n in nodes if n != "n3")
        after = cluster.rendezvous("ForkMeshRoom", name, survivors)
        if owner != "n3":
            assert after == owner
        else:
            moved += 1
    assert 60 < moved < 140
    assert cluster.rendezvous("X", "y", ()) is None


def test_placement_policy():
    assert cluster.placement("ForkMeshCronRunner", "app-ingress-v1:3") == \
        "local"
    assert cluster.placement("ForkMeshCronRunner",
                             "app-git-ingress-v1:f") == "local"
    assert cluster.placement("ForkMeshCronRunner",
                             "scheduled-runner-v1") == "rotate"
    assert cluster.placement("ForkMeshCronRunner",
                             "snapshot-republish-v1") == "hash"
    assert cluster.placement("ForkMeshCronWatchdog", "x") == "rotate"
    assert cluster.placement("ForkMeshDiscordGate", "bot-rate-v1") == \
        "hash"
    assert cluster.placement("ForkMeshWorld", "town-square-v1") == "hash"


def test_value_codec_round_trip():
    value = JsObject(a=1, b="x", c=[1, JsObject(d=None)],
                     u=_js.Uint8Array.new(b"\x00\xff"),
                     ab=_js.ArrayBuffer(b"ab"), when=_js.JsDate(1234))
    back = cluster.decode_value(cluster.encode_value(value))
    assert isinstance(back, JsObject) and isinstance(back["c"], JsArray)
    assert back["a"] == 1 and back["c"][1]["d"] is None
    assert bytes(_js._as_bytes(back["u"])) == b"\x00\xff"
    assert bytes(back["ab"]._data) == b"ab"
    assert back["when"].getTime() == 1234
    assert cluster.decode_value(cluster.encode_value("plain")) == "plain"
    assert cluster.decode_value(cluster.encode_value(
        {"k": [True, 2.5]})) == {"k": [True, 2.5]}


def test_mesh_path_round_trip():
    path = cluster.do_path("ForkMeshRoom", "repo:a/b:room:general",
                           "/api/repo/a/b/rooms/general/ws")
    kind, ns, name, rest = cluster.parse_mesh_path(path + "?x=1")
    assert (kind, ns, name) == ("_do", "ForkMeshRoom",
                                "repo:a/b:room:general")
    assert rest == "api/repo/a/b/rooms/general/ws"
    assert cluster.parse_mesh_path("/other/x/y") is None


def test_cluster_config_requires_secret_and_rqlite():
    import pytest
    assert cluster.ClusterConfig.from_environ({}) is None
    base = {"FORKMESH_NODE_ID": "n1", "FORKMESH_MESH_LISTEN": "127.0.0.1:1"}
    with pytest.raises(RuntimeError):
        cluster.ClusterConfig.from_environ(dict(base))
    with pytest.raises(RuntimeError):
        cluster.ClusterConfig.from_environ(dict(
            base, FORKMESH_HOST_DB="rqlite:http://127.0.0.1:4001"))
    config = cluster.ClusterConfig.from_environ(dict(
        base, FORKMESH_HOST_DB="rqlite:http://127.0.0.1:4001",
        FORKMESH_MESH_SECRET=SECRET, FORKMESH_RQLITE_NODE_MAP="r1=n1, r2=n2"))
    assert config.mesh_url == "http://127.0.0.1:1"
    assert config.rqlite_url == "http://127.0.0.1:4001"
    assert config.rqlite_node_map == {"r1": "n1", "r2": "n2"}


def test_leaving_members_are_not_live_and_show_as_leaving():
    now = cluster.now_ms()
    rows = [{"node_id": "a", "status": "active", "heartbeat_ms": now},
            {"node_id": "b", "status": "leaving", "heartbeat_ms": now},
            {"node_id": "c", "status": "sealed", "heartbeat_ms": now}]
    assert cluster.live_members(rows, now) == ("a",)
    assert [cluster.member_state(r, now) for r in rows] == [
        "live", "leaving", "sealed"]
    stale = dict(rows[1], heartbeat_ms=now - cluster.STALE_MS - 1)
    assert cluster.member_state(stale, now) == "stale"


class _Rqlite:
    def __init__(self):
        self.statements = []

    async def execute(self, statements):
        self.statements.extend(statements)


def _membership(**flags):
    membership = cluster.Membership.__new__(cluster.Membership)
    membership.replica = False
    membership.paused = False
    membership.leaving = False
    membership.quarantine = None
    membership.node_id = "n1"
    membership.epoch = 1
    membership.config = type("C", (), {"mesh_url": "http://n1:1"})()
    membership.rqlite = _Rqlite()
    membership.__dict__.update(flags)
    return membership


def test_heartbeat_reports_leaving_once_the_node_drains():
    import asyncio
    for flags, status in (({}, "active"), ({"paused": True}, "sealed"),
                          ({"leaving": True}, "leaving"),
                          ({"paused": True, "leaving": True}, "leaving")):
        membership = _membership(**flags)
        asyncio.run(membership.heartbeat())
        assert membership.rqlite.statements[0][3] == status, flags


def test_only_the_mesh_own_refusals_are_retried():
    from forkmesh_host.durable_cluster import MESH_STATE_HEADER, _mesh_retryable
    assert _mesh_retryable(421, {})
    assert _mesh_retryable(503, {MESH_STATE_HEADER: "no-quorum"})
    assert not _mesh_retryable(503, {})
    assert not _mesh_retryable(500, {MESH_STATE_HEADER: "no-quorum"})


def test_drain_leaves_then_waits_only_for_a_live_hosting_node(monkeypatch):
    import asyncio

    from forkmesh_host import durable_cluster
    monkeypatch.setattr(cluster, "DRAIN_SECONDS", 0.01)
    calls = []

    class Membership:
        replica = False
        hosting = True
        active = ("n1", "n2")

        async def leave(self):
            calls.append("leave")
            self.active = ("n2",)

    def host(**flags):
        durable = durable_cluster.ClusterDurableHost.__new__(
            durable_cluster.ClusterDurableHost)
        durable.node_id = "n1"
        durable.stats = {"drains": 0}
        durable.membership = Membership()
        durable.membership.__dict__.update(flags)
        return durable

    live = host()
    assert asyncio.run(live.drain()) is True
    assert calls == ["leave"] and live.stats["drains"] == 1
    assert live.membership.active == ("n2",)
    for flags in ({"hosting": False}, {"replica": True},
                  {"active": ("n2",)}):
        calls.clear()
        assert asyncio.run(host(**flags).drain()) is False
        assert calls == []


def test_requests_wait_for_hosting_only_while_the_node_joins():
    import asyncio
    import time

    async def flip(membership):
        await asyncio.sleep(0.1)
        membership.hosting = True

    async def joining():
        membership = _membership(hosting=False, _started=time.monotonic())
        asyncio.get_running_loop().create_task(flip(membership))
        return await membership.wait_hosting(2.0)

    async def settled():
        membership = _membership(
            hosting=False,
            _started=time.monotonic() - cluster.JOIN_WINDOW_SECONDS - 1)
        started = time.monotonic()
        result = await membership.wait_hosting(2.0)
        return result, time.monotonic() - started

    assert asyncio.run(joining()) is True
    result, waited = asyncio.run(settled())
    assert result is False and waited < 0.05
    sealed = _membership(hosting=False, paused=True,
                         _started=time.monotonic())
    assert asyncio.run(sealed.wait_hosting(2.0)) is False
