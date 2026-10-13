"""Security checks for cluster addressing and the mesh listener.

Audit 2026-09-24 H3: member ids and mesh addresses come only from the static
``FORKMESH_MESH_PEERS`` map, never from ``cluster_members`` rows, and a
heartbeat dated in the future is never live. M4: the mesh listener rejects
bad auth and oversized bodies before reading the body.
"""

from __future__ import annotations

import asyncio
import json

from forkmesh_host import cluster
from forkmesh_host import durable_cluster

SECRET = "unit-secret-0123456789abcdef"
PEERS = "n1=http://10.77.0.1:8792,n2=http://10.77.0.2:8792,n3=http://10.77.0.3:8792"


def _config(peers=PEERS, platform="vps"):
    return cluster.ClusterConfig.from_environ({
        "FORKMESH_NODE_ID": "n1",
        "FORKMESH_MESH_LISTEN": "10.77.0.1:8792",
        "FORKMESH_MESH_URL": "http://10.77.0.1:8792",
        "FORKMESH_MESH_SECRET": SECRET,
        "FORKMESH_HOST_DB": "rqlite:http://127.0.0.1:4001",
        "FORKMESH_MESH_PEERS": peers,
        "FORKMESH_PLATFORM": platform,
    })


def _membership(config):
    membership = cluster.Membership(config)
    return membership


def _rows(now):
    return [
        {"node_id": "n1", "mesh_url": "http://10.77.0.1:8792",
         "status": "active", "heartbeat_ms": now, "epoch": 1},
        # A known node whose row was tampered with.
        {"node_id": "n2", "mesh_url": "http://evil.example:80",
         "status": "active", "heartbeat_ms": now, "epoch": 1},
        # An inserted node with a far-future heartbeat.
        {"node_id": "n9", "mesh_url": "http://evil", "status": "active",
         "heartbeat_ms": now + 10 ** 12, "epoch": 1},
        # A known node with a far-future heartbeat.
        {"node_id": "n3", "mesh_url": "http://10.77.0.3:8792",
         "status": "active", "heartbeat_ms": now + 10 ** 12, "epoch": 1},
    ]


def test_rows_never_supply_members_or_addresses():
    config = _config()
    assert config.strict and config.peers["n2"] == "http://10.77.0.2:8792"
    membership = _membership(config)
    now = cluster.now_ms()
    members = membership.accept_rows(_rows(now), now)
    membership.members = members
    assert set(members) == {"n1", "n2"}
    assert "n9" not in members and "n3" not in members
    assert membership.mesh_url("n2") == "http://10.77.0.2:8792"
    assert membership.mesh_url("n9") is None
    assert membership.mesh_url("n1") == "http://10.77.0.1:8792"
    live = cluster.live_members(members.values(), now)
    assert live == ("n1", "n2")
    for i in range(50):
        assert cluster.rendezvous("ForkMeshRoom", "r%d" % i, live) != "n9"
    assert membership.stats["rejected_rows"] >= 2
    asyncio.run(membership.rqlite.aclose())


def test_future_heartbeat_is_never_live():
    now = 1_000_000_000
    rows = [{"node_id": "n1", "heartbeat_ms": now + 61_000, "status": "active"},
            {"node_id": "n2", "heartbeat_ms": now + 5_000, "status": "active"},
            {"node_id": "n3", "heartbeat_ms": now - 1_000, "status": "sealed"}]
    assert cluster.live_members(rows, now) == ("n2",)


def test_vps_without_map_routes_only_to_self():
    config = _config(peers="")
    assert config.strict and set(config.peers) == {"n1"}
    membership = _membership(config)
    now = cluster.now_ms()
    members = membership.accept_rows(_rows(now), now)
    assert set(members) == {"n1"}
    assert membership.mesh_url("n2") is None
    asyncio.run(membership.rqlite.aclose())


def test_dev_platform_without_map_keeps_row_addresses():
    config = _config(peers="", platform="dev")
    assert not config.strict
    membership = _membership(config)
    now = cluster.now_ms()
    membership.members = membership.accept_rows(_rows(now), now)
    # Dev keeps trusting rows, but still drops future heartbeats.
    assert set(membership.members) == {"n1", "n2"}
    assert membership.mesh_url("n2") == "http://evil.example:80"
    asyncio.run(membership.rqlite.aclose())


# -- mesh listener -----------------------------------------------------------

class _Host:
    def __init__(self):
        self.verifier = cluster.Verifier(SECRET)
        self.stats = {"mesh_rejected": 0, "mesh_requests": 0}
        self.peer_unseal_handler = None
        self.served = []

    async def serve_mesh(self, *args):
        self.served.append(args)
        return durable_cluster._text_response(200, "ok")


def _call(path, headers, body_chunks):
    host = _Host()
    app = durable_cluster.MeshASGI(host)
    reads = []
    sent = []
    chunks = list(body_chunks) or [b""]

    async def receive():
        chunk = chunks.pop(0)
        reads.append(len(chunk))
        return {"type": "http.request", "body": chunk,
                "more_body": bool(chunks)}

    async def send(message):
        sent.append(message)

    scope = {"type": "http", "method": "POST", "path": path,
             "raw_path": path.encode(), "query_string": b"",
             "headers": [(k.encode(), v.encode()) for k, v in headers]}
    asyncio.run(app(scope, receive, send))
    status = next(m["status"] for m in sent
                  if m["type"] == "http.response.start")
    return status, reads, host


def test_mesh_rejects_missing_or_stale_auth_before_reading_body():
    path = "/_rpc/ForkMeshRoom/r/get"
    status, reads, host = _call(path, [], [b"x" * 100])
    assert status == 401 and reads == []
    stale = cluster.sign(SECRET, "POST", path, b"x",
                         ts=cluster.now_ms() - 120_000)
    status, reads, _ = _call(path, [(cluster.AUTH_HEADER, stale)], [b"x"])
    assert status == 401 and reads == []
    status, reads, _ = _call(path, [(cluster.AUTH_HEADER, "v1:1:n:zz")], [b"x"])
    assert status == 401 and reads == []


def test_mesh_rejects_oversized_bodies():
    path = "/_rpc/ForkMeshRoom/r/get"
    big = durable_cluster.MESH_MAX_BODY + 1
    header = cluster.sign(SECRET, "POST", path, b"")
    status, reads, _ = _call(
        path, [(cluster.AUTH_HEADER, header), ("content-length", str(big))],
        [b"x"])
    assert status == 413 and reads == []
    # Streamed without a length: stops once the cap is crossed.
    chunk = b"x" * (1024 * 1024)
    status, reads, _ = _call(path, [(cluster.AUTH_HEADER, header)],
                             [chunk] * 8)
    assert status == 413 and len(reads) == 5


def test_mesh_accepts_a_valid_signed_request():
    path = "/_rpc/ForkMeshRoom/r/get"
    body = json.dumps({"args": []}).encode()
    header = cluster.sign(SECRET, "POST", path, body)
    status, _, host = _call(path, [(cluster.AUTH_HEADER, header)], [body])
    assert status == 200 and len(host.served) == 1


class _ObjectId:
    def toString(self):
        return "ab" * 32


def test_client_headers_cannot_override_mesh_routing_headers():
    from forkmesh_host import js as _js
    from forkmesh_host.ffi import JsObject
    headers = _js.Headers()
    headers.append("x-fm-do-url", "https://app.example/api/internal/revoke")
    headers.append("x-fm-do-id", "cd" * 32)
    headers.append("x-fm-do-cf", '{"country":"ZZ"}')
    headers.append("accept", "application/json")
    request = _js.Request("https://app.example/api/chat/ws", "GET", headers,
                          None, None, JsObject(country="DE"))
    forwarded = durable_cluster.ClusterDurableHost._mesh_headers(
        None, _ObjectId(), request)
    names = [name.lower() for name, _ in forwarded]
    for internal in ("x-fm-do-url", "x-fm-do-id", "x-fm-do-cf"):
        assert names.count(internal) == 1
    values = dict((name.lower(), value) for name, value in forwarded)
    assert values["x-fm-do-url"] == "https://app.example/api/chat/ws"
    assert values["x-fm-do-id"] == "ab" * 32
    scope = {"type": "http", "method": "GET", "path": "/_do/x",
             "headers": [(k.encode(), v.encode()) for k, v in forwarded]}
    rebuilt = durable_cluster.MeshASGI(_Host())._request(scope, "GET", None)
    assert rebuilt.url == "https://app.example/api/chat/ws"
    assert rebuilt.headers.get("accept") == "application/json"
