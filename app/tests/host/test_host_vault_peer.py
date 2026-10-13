"""Peer unseal (vault.py) and sealed cluster membership, in process.

Two Vault objects on fake runtimes share one sqlite D1 holding the vault
rows; the requester's HTTP goes through an httpx MockTransport into the
responder's mesh handler, with the real mesh HMAC verifier in between.
"""

from __future__ import annotations

import asyncio
import json
import os
import types

import httpx
import pytest

from forkmesh_host import cluster, vault, vault_crypto
from forkmesh_host.d1_sqlite import D1Sqlite
from forkmesh_host.env import Env

KEY = "peer-test-data-key-" + os.urandom(16).hex()
MESH_SECRET = "peer-test-mesh-secret-" + os.urandom(8).hex()
PEERS = "n1=http://10.77.0.1:8792,n2=http://10.77.0.2:8792,n3=http://10.77.0.3:8792"
ADMIN = "peer-admin-" + os.urandom(6).hex()


def make_db(tmp_path, key=KEY):
    db = D1Sqlite(tmp_path / "d1.sqlite")
    db._conn.execute(
        "CREATE TABLE IF NOT EXISTS host_secrets (name TEXT PRIMARY KEY, nonce TEXT "
        "NOT NULL, ciphertext TEXT NOT NULL, key_id TEXT NOT NULL, updated_at "
        "INTEGER NOT NULL)")
    vk = vault_crypto.VaultKey(key)
    for row in (vk.check_row(), vk.encrypt("ADMIN_PASS", ADMIN)):
        db._conn.execute("INSERT OR REPLACE INTO host_secrets VALUES(?,?,?,?,1)",
                         (row["name"], row["nonce"], row["ciphertext"], row["key_id"]))
    return db


def fake_runtime(node, db, members=None, peer=True):
    environ = {"FORKMESH_VAULT": "1", "FORKMESH_MESH_PEERS": PEERS,
               "FORKMESH_VAULT_PEER_UNSEAL": "1" if peer else "0",
               "FORKMESH_VAULT_SOCKET": "/nonexistent/unseal.sock"}
    membership = types.SimpleNamespace(members=members if members is not None else {})
    durable = types.SimpleNamespace(
        membership=membership, node_id=node, peer_unseal_handler=None,
        config=types.SimpleNamespace(secret=MESH_SECRET))
    runtime = types.SimpleNamespace(
        config=types.SimpleNamespace(environ=environ), env=Env({"DB": db}),
        durable=durable, entry=None, _db_scope=None, available=[])
    runtime.set_available = runtime.available.append
    return runtime


def sealed_row(node, status="sealed", age_ms=0):
    return {"node_id": node, "status": status,
            "heartbeat_ms": cluster.now_ms() - age_ms}


def run(coroutine):
    return asyncio.run(coroutine)


async def unsealed_responder(tmp_path, members):
    runtime = fake_runtime("n1", make_db(tmp_path), members)
    responder = vault.Vault(runtime)
    assert (await responder.unseal(KEY))["ok"] is True
    return responder


def mesh_transport(responders, source_ips=None):
    """Route http://<wg ip>:8792/_vault/peer-unseal to the responder's
    handler after the real HMAC check; the source address is the
    requester's wg IP unless overridden."""
    verifier = cluster.Verifier(MESH_SECRET)
    source_ips = source_ips or {}

    async def handler(request):
        body = request.content
        if not verifier.verify(request.headers.get(cluster.AUTH_HEADER), "POST",
                               request.url.path, body):
            return httpx.Response(401, text="mesh authentication failed")
        responder = responders.get(request.url.host)
        if responder is None:
            raise httpx.ConnectError("no route")
        node = json.loads(body)["node"]
        client_ip = source_ips.get(node, "10.77.0.%s" % node[1:])
        status, payload = await responder.serve_peer(client_ip, body)
        return httpx.Response(status, json=payload)
    return httpx.MockTransport(handler)


# --------------------------------------------------------------------------
# crypto


def test_peer_seal_roundtrip_and_binding():
    private, public = vault_crypto.peer_keypair()
    import base64
    pub = base64.b64encode(public).decode()
    reply = vault_crypto.peer_seal(KEY, "n2", "n1", pub, "nonce-123456")
    assert KEY not in json.dumps(reply)
    assert vault_crypto.peer_open(private, public, "n2", "n1", "nonce-123456",
                                  reply) == KEY
    # Bound to the transcript: another responder name, requester or nonce fails.
    with pytest.raises(vault_crypto.VaultError):
        vault_crypto.peer_open(private, public, "n2", "n3", "nonce-123456",
                               dict(reply, responder="n3"))
    with pytest.raises(vault_crypto.VaultError):
        vault_crypto.peer_open(private, public, "n3", "n1", "nonce-123456", reply)
    with pytest.raises(vault_crypto.VaultError):
        vault_crypto.peer_open(private, public, "n2", "n1", "nonce-other", reply)
    # A replayed reply is useless without the requester's ephemeral key.
    other_private, _ = vault_crypto.peer_keypair()
    with pytest.raises(vault_crypto.VaultError):
        vault_crypto.peer_open(other_private, public, "n2", "n1", "nonce-123456", reply)


# --------------------------------------------------------------------------
# responder checks


def test_responder_refuses_unless_every_check_passes(tmp_path, capsys):
    async def scenario():
        members = {"n2": sealed_row("n2"), "n3": sealed_row("n3", "active")}
        responder = await unsealed_responder(tmp_path, members)
        _, public = vault_crypto.peer_keypair()
        import base64
        pub = base64.b64encode(public).decode()

        def body(node):
            return json.dumps({"v": 1, "node": node, "pub": pub,
                               "nonce": os.urandom(8).hex()}).encode()
        results = {
            "ok": await responder.serve_peer("10.77.0.2", body("n2")),
            "self": await responder.serve_peer("10.77.0.1", body("n1")),
            "stranger": await responder.serve_peer("10.77.0.9", body("n9")),
            "spoofed": await responder.serve_peer("10.77.0.3", body("n2")),
            "active": await responder.serve_peer("10.77.0.3", body("n3")),
            "garbage": await responder.serve_peer("10.77.0.2", b"{not json"),
        }
        members["n2"] = sealed_row("n2", age_ms=cluster.STALE_MS + 1000)
        results["stale"] = await responder.serve_peer("10.77.0.2", body("n2"))
        members["n2"] = sealed_row("n2")
        await responder.seal()
        results["sealed"] = await responder.serve_peer("10.77.0.2", body("n2"))
        return results
    results = run(scenario())
    assert results["ok"][0] == 200 and results["ok"][1]["responder"] == "n1"
    assert results["self"] == (403, {"ok": False, "error": "unknown_peer"})
    assert results["stranger"] == (403, {"ok": False, "error": "unknown_peer"})
    assert results["spoofed"] == (403, {"ok": False, "error": "source_mismatch"})
    assert results["active"] == (403, {"ok": False, "error": "not_a_sealed_member"})
    assert results["garbage"][0] == 400
    assert results["stale"] == (403, {"ok": False, "error": "not_a_sealed_member"})
    assert results["sealed"] == (503, {"ok": False, "error": "sealed"})
    log = capsys.readouterr().out
    assert "vault: peer unseal served to n2 (10.77.0.2)" in log
    assert "refused for n2 from 10.77.0.3 (source_mismatch)" in log
    assert KEY not in log and ADMIN not in log


def test_responder_rate_limits_per_node(tmp_path):
    async def scenario():
        responder = await unsealed_responder(tmp_path, {"n2": sealed_row("n2")})
        import base64
        pub = base64.b64encode(vault_crypto.peer_keypair()[1]).decode()
        codes = []
        for _ in range(vault.PEER_MAX_PER_NODE + 1):
            body = json.dumps({"node": "n2", "pub": pub,
                               "nonce": os.urandom(8).hex()}).encode()
            codes.append((await responder.serve_peer("10.77.0.2", body))[0])
        return codes
    codes = run(scenario())
    assert codes == [200] * vault.PEER_MAX_PER_NODE + [429]


# --------------------------------------------------------------------------
# requester


def test_sealed_node_unseals_from_a_peer(tmp_path, capsys):
    async def scenario():
        members = {"n2": sealed_row("n2")}
        responder = await unsealed_responder(tmp_path / "a", members)
        requester_runtime = fake_runtime("n2", make_db(tmp_path / "a"), members)
        requester = vault.Vault(requester_runtime)
        assert requester_runtime.durable.peer_unseal_handler == requester.serve_peer
        requester.http_client_factory = lambda: httpx.AsyncClient(
            transport=mesh_transport({"10.77.0.1": responder}))
        assert requester.sealed
        ok = await requester.try_peers()
        return ok, requester, requester_runtime
    ok, requester, runtime = run(scenario())
    assert ok is True and requester.sealed is False
    assert runtime.env.DATA_KEY == KEY and runtime.env.ADMIN_PASS == ADMIN
    assert runtime.available == [True]
    assert requester.stats["peer_unseals"] == 1
    log = capsys.readouterr().out
    assert "from peer n1" in log
    assert KEY not in log and ADMIN not in log


def test_requester_rejects_a_key_that_fails_the_vault_check(tmp_path):
    async def scenario():
        members = {"n2": sealed_row("n2")}
        # The responder was unsealed against a vault under another key.
        other = "other-peer-key-" + os.urandom(16).hex()
        liar_runtime = fake_runtime("n1", make_db(tmp_path / "liar", other), members)
        liar = vault.Vault(liar_runtime)
        assert (await liar.unseal(other))["ok"]
        requester = vault.Vault(fake_runtime("n2", make_db(tmp_path / "real"), members))
        requester.http_client_factory = lambda: httpx.AsyncClient(
            transport=mesh_transport({"10.77.0.1": liar}))
        ok = await requester.try_peers()
        # A bad peer key does not burn the operator's socket attempts.
        return ok, requester
    ok, requester = run(scenario())
    assert ok is False and requester.sealed
    assert requester.stats["failures"] == 0


def test_requester_falls_through_unreachable_and_refusing_peers(tmp_path):
    async def scenario():
        members = {"n2": sealed_row("n2")}
        good = await unsealed_responder(tmp_path / "g", members)
        sealed_peer = vault.Vault(fake_runtime("n3", make_db(tmp_path / "g"), members))
        requester = vault.Vault(fake_runtime("n2", make_db(tmp_path / "g"), members))
        requester.http_client_factory = lambda: httpx.AsyncClient(
            transport=mesh_transport({"10.77.0.1": good, "10.77.0.3": sealed_peer}))
        return await requester.try_peers(), requester
    ok, requester = run(scenario())
    assert ok and not requester.sealed


def test_wrong_mesh_secret_is_refused(tmp_path):
    async def scenario():
        members = {"n2": sealed_row("n2")}
        responder = await unsealed_responder(tmp_path, members)
        requester = vault.Vault(fake_runtime("n2", make_db(tmp_path), members))
        requester._mesh_secret = "not-the-mesh-secret-000000"
        requester.http_client_factory = lambda: httpx.AsyncClient(
            transport=mesh_transport({"10.77.0.1": responder}))
        return await requester.try_peers(), responder
    ok, responder = run(scenario())
    assert ok is False and responder.stats["peer_served"] == 0


def test_peer_unseal_off_registers_nothing(tmp_path):
    runtime = fake_runtime("n2", make_db(tmp_path), peer=False)
    requester = vault.Vault(runtime)
    assert requester.peer_enabled is False
    assert runtime.durable.peer_unseal_handler is None


def test_peer_order_is_rendezvous_and_excludes_nobody():
    order = vault.peer_order("n2", ["n1", "n3"])
    assert sorted(order) == ["n1", "n3"]
    assert vault.peer_order("n2", ["n3", "n1"]) == order
    assert vault.parse_peers(PEERS)["n3"] == "http://10.77.0.3:8792"
    assert vault.parse_peers("n1=ftp://x,bogus") == {}


# --------------------------------------------------------------------------
# sealed membership


class FakeRqlite:
    def __init__(self):
        self.executed = []

    async def execute(self, statements, transaction=True):
        self.executed.extend(statements)
        return [{}]


def membership():
    config = cluster.ClusterConfig("n2", "10.77.0.2:8792", "http://10.77.0.2:8792",
                                   MESH_SECRET, "http://127.0.0.1:4001")
    member = cluster.Membership(config)
    run(member.rqlite.aclose())
    member.rqlite = FakeRqlite()
    return member


def test_sealed_node_heartbeats_as_sealed():
    member = membership()
    member.paused = True
    run(member.heartbeat())
    sql, *params = member.rqlite.executed[-1]
    assert params[0] == "n2" and params[2] == "sealed"
    member.paused = False
    run(member.heartbeat())
    sql, *params = member.rqlite.executed[-1]
    assert params[2] == "active"
    # Leaderless: a node only ever writes its own row.
    assert all(s[1] == "n2" for s in member.rqlite.executed)
    assert "UPDATE cluster_members" not in sql


def test_liveness_is_derived_and_never_includes_a_sealed_node():
    rows = [sealed_row("n1", "active"), sealed_row("n2", "sealed"),
            sealed_row("n3", "active", age_ms=cluster.STALE_MS + 5000),
            sealed_row("n4", "joining"), sealed_row("n5", "suspended")]
    # Every node computes the same set from the same rows: fresh, not sealed.
    assert cluster.live_members(rows) == ("n1", "n4", "n5")
    states = {r["node_id"]: cluster.member_state(r) for r in rows}
    assert states == {"n1": "live", "n2": "sealed", "n3": "stale",
                      "n4": "live", "n5": "live"}
    assert not hasattr(cluster.Membership, "_supervise")


def test_sealed_membership_reports_not_ready():
    member = membership()
    member.paused = True

    async def status():
        return {"store": {"leader": {"node_id": "n1"}}}
    member.rqlite.status = status
    ok, reasons = run(member.readiness())
    assert not ok and "sealed" in reasons and "not_hosting" in reasons
