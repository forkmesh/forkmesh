"""IDS quarantine in the app host (docs/IDS.md): signed records, the view,
membership, peer unseal, the socket, the mesh and seal-restart. In
process, with fakes."""

from __future__ import annotations

import asyncio
import json
import os
import time
import types

import pytest

pytest.importorskip("cryptography")

from forkmesh_host import (cluster, ids_records, quarantine,  # noqa: E402
                           vault, vault_crypto)
from forkmesh_host.d1_sqlite import D1Sqlite  # noqa: E402
from forkmesh_host.env import Env  # noqa: E402

KEY = "quarantine-test-data-key-" + os.urandom(16).hex()
MESH = "quarantine-mesh-secret-" + os.urandom(8).hex()
PEERS = ("n1=http://10.77.0.1:8792,n2=http://10.77.0.2:8792,"
         "n3=http://10.77.0.3:8792")
NODES = ["n1", "n2", "n3"]
PRIVATE = {n: ids_records.generate_private() for n in NODES}
PUBLIC = {n: ids_records.public_key(k) for n, k in PRIVATE.items()}
OPERATOR = ids_records.generate_private()
OPERATOR_PUB = ids_records.public_key(OPERATOR)


def run(coroutine):
    return asyncio.run(coroutine)


def now_ms():
    return time.time_ns() // 1_000_000


def self_row(node, ts=None, signer_key=None):
    body = ids_records.self_body(node, ts or now_ms(), "inc", "lockdown")
    return ids_records.make_row("self", node, node, body, ids_records.sign(
        signer_key or PRIVATE[node], "self", body))


def vote_row(voter, target, ts=None):
    body = ids_records.vote_body(voter, target, ts or now_ms(), "silent")
    return ids_records.make_row("vote", target, voter, body,
                                ids_records.sign(PRIVATE[voter], "vote", body))


def release_row(node, clears=(), ts=None, key=None):
    body = ids_records.release_body(node, ts or now_ms(), clears, "n0nce")
    return ids_records.make_row("release", node, "operator", body,
                                ids_records.sign(key or OPERATOR, "release",
                                                 body))


def state(rows):
    return ids_records.quarantine_state(rows, PUBLIC, OPERATOR_PUB, NODES)


# -- signed records -----------------------------------------------------------

def test_self_row_quarantines_and_a_release_clears():
    rows = [self_row("n2", ts=1000)]
    assert state(rows)["n2"]["quarantined"]
    rows.append(release_row("n2", ts=2000))
    current = state(rows)
    assert not current["n2"]["quarantined"]
    assert ids_records.peer_unseal_blocked(current, "n2")    # not reunsealed
    rows[-1]["reunsealed_ms"] = 3000
    assert not ids_records.peer_unseal_blocked(state(rows), "n2")
    # A later lockdown quarantines again.
    rows.append(self_row("n2", ts=4000))
    assert state(rows)["n2"]["quarantined"]


def test_forged_records_are_ignored():
    rows = [self_row("n2", signer_key=PRIVATE["n1"]),      # n1 forging n2
            release_row("n3", key=PRIVATE["n3"])]           # node "releases"
    current = state(rows)
    assert not current["n2"]["quarantined"] and current["n2"]["invalid"] == 1
    assert current["n3"]["release"] is None
    tampered = self_row("n2")
    tampered["body"] = tampered["body"].replace("lockdown", "harmless")
    assert not state([tampered])["n2"]["quarantined"]
    stranger = ids_records.generate_private()
    assert not state([self_row("n9", signer_key=stranger)]).get(
        "n9", {}).get("quarantined")        # unknown node: no key


def test_votes_need_a_strict_majority_of_other_nodes():
    assert not state([vote_row("n1", "n3")])["n3"]["quarantined"]
    assert not state([vote_row("n1", "n3"), vote_row("n1", "n3")])["n3"][
        "quarantined"]
    assert state([vote_row("n1", "n3"), vote_row("n2", "n3")])["n3"][
        "quarantined"]


def test_a_release_can_clear_future_dated_records_by_id():
    future = self_row("n2", ts=now_ms() + 10 ** 9)
    assert state([future, release_row("n2")])["n2"]["quarantined"]
    assert not state([future, release_row("n2", clears=[future["id"]])])[
        "n2"]["quarantined"]


def test_sticky_quarantine_survives_row_deletion():
    rows = [self_row("n2", ts=1000)]
    current, sticky = ids_records.merge_sticky(state(rows), {})
    assert sticky == {"n2": {rows[0]["id"]: 1000}}
    # An attacker deletes the rows: still quarantined.
    current, sticky = ids_records.merge_sticky(state([]), sticky)
    assert current["n2"]["quarantined"] and "sticky" in \
        current["n2"]["reasons"][0]
    # Only a valid operator release lifts it.
    current, sticky = ids_records.merge_sticky(
        state([release_row("n2", ts=2000)]), sticky)
    assert not current["n2"]["quarantined"] and sticky == {}


# -- the view -----------------------------------------------------------------

class FakeRqlite:
    def __init__(self, rows=None):
        self.rows = list(rows or [])
        self.executed = []
        self.fail = False

    async def query(self, sql, *params, level="none"):
        if self.fail:
            raise RuntimeError("down")
        if "cluster_quarantine" in sql:
            return [dict(r) for r in self.rows]
        return []

    async def execute(self, statements, transaction=True):
        self.executed.extend(statements)
        for statement in statements:
            if statement[0].startswith("UPDATE cluster_quarantine"):
                for row in self.rows:
                    if row["id"] == statement[2] and \
                            row.get("reunsealed_ms") is None:
                        row["reunsealed_ms"] = statement[1]
        return [{"rows_affected": 1}]


def view(node, rows=None, data_dir=None, rq=None):
    return quarantine.QuarantineView(node, NODES, PUBLIC, OPERATOR_PUB,
                                     rq or FakeRqlite(rows), data_dir)


def test_view_refresh_sticky_file_and_reunseal(tmp_path):
    rq = FakeRqlite([self_row("n2")])
    v = view("n2", rq=rq, data_dir=tmp_path)
    run(v.refresh())
    assert v.self_quarantined and v.quarantined() == {"n2"}
    # A new process (restart) remembers it before reading anything.
    rq.rows = []
    again = view("n2", rq=FakeRqlite([]), data_dir=tmp_path)
    assert again.self_quarantined
    run(again.refresh())
    assert again.self_quarantined
    # The operator's release: no longer quarantined, but needs the desktop.
    release = release_row("n2", clears=[])
    again.rqlite.rows = [release]
    run(again.refresh())
    assert not again.self_quarantined and again.self_needs_reunseal
    assert again.peer_unseal_blocked("n2")
    run(again.mark_reunsealed())
    assert again.rqlite.rows[0]["reunsealed_ms"]
    run(again.refresh())
    assert not again.self_needs_reunseal and not again.peer_unseal_blocked(
        "n2")
    # A failed read keeps the last state.
    again.rqlite.fail = True
    run(again.refresh())
    assert again.stats["errors"] == 1


def test_view_from_environ_is_off_without_keys():
    assert quarantine.from_environ({}, "n1", NODES) is None
    keys = ",".join("%s=%s" % kv for kv in PUBLIC.items())
    v = quarantine.from_environ({"FORKMESH_IDS_PUBKEYS": keys,
                                 "FORKMESH_IDS_OPERATOR_PUBKEY": OPERATOR_PUB},
                                "n1", NODES)
    assert v.enabled and v.operator_key == OPERATOR_PUB


# -- membership ---------------------------------------------------------------

def test_live_members_exclude_quarantined():
    now = now_ms()
    rows = [{"node_id": n, "heartbeat_ms": now, "status": "active"}
            for n in NODES]
    assert cluster.live_members(rows, now, exclude={"n2"}) == ("n1", "n3")
    rows[2]["status"] = "quarantined"
    assert cluster.live_members(rows, now) == ("n1", "n2")


def make_membership(node="n2", rows=None):
    config = cluster.ClusterConfig(
        node, "10.77.0.%s:8792" % node[1:], "http://10.77.0.%s:8792" % node[1:],
        MESH, "http://127.0.0.1:1", peers=cluster.parse_peers(PEERS),
        ids_env={"FORKMESH_IDS_PUBKEYS": ",".join(
            "%s=%s" % kv for kv in PUBLIC.items()),
            "FORKMESH_IDS_OPERATOR_PUBKEY": OPERATOR_PUB})
    membership = cluster.Membership(config)
    membership.quarantine.rqlite = FakeRqlite(rows)
    return membership


def test_membership_readiness_heartbeat_and_callback():
    async def scenario():
        membership = make_membership("n2", [self_row("n2")])
        calls = []

        async def on_quarantine(flag):
            calls.append(flag)
        membership.on_quarantine = on_quarantine
        await membership._refresh_quarantine()
        assert calls == [True] and membership.self_quarantined

        async def status():
            return {"store": {"leader": {"node_id": "n1"}}}
        membership.rqlite.status = status
        ok, reasons = await membership.readiness()
        assert not ok and reasons[0] == "quarantined"
        sent = []

        async def execute(statements, transaction=True):
            sent.extend(statements)
        membership.rqlite.execute = execute
        await membership.heartbeat()
        assert sent[0][3] == "quarantined"
        # Its mesh address is refused by the peers.
        peer = make_membership("n1", [self_row("n2")])
        await peer._refresh_quarantine()
        assert peer.source_quarantined("10.77.0.2")
        assert not peer.source_quarantined("10.77.0.3")
        await membership.rqlite.aclose()
        await peer.rqlite.aclose()
    run(scenario())


# -- vault -------------------------------------------------------------------

def make_db(tmp_path):
    db = D1Sqlite(tmp_path / "d1.sqlite")
    db._conn.execute(
        "CREATE TABLE IF NOT EXISTS host_secrets (name TEXT PRIMARY KEY, nonce "
        "TEXT NOT NULL, ciphertext TEXT NOT NULL, key_id TEXT NOT NULL, "
        "updated_at INTEGER NOT NULL)")
    vk = vault_crypto.VaultKey(KEY)
    for row in (vk.check_row(), vk.encrypt("ADMIN_PASS", "admin-SECRET")):
        db._conn.execute("INSERT OR REPLACE INTO host_secrets VALUES(?,?,?,?,1)",
                         (row["name"], row["nonce"], row["ciphertext"],
                          row["key_id"]))
    return db


def fake_runtime(node, tmp_path, qview, members=None):
    environ = {"FORKMESH_VAULT": "1", "FORKMESH_MESH_PEERS": PEERS,
               "FORKMESH_VAULT_PEER_UNSEAL": "1",
               "FORKMESH_VAULT_SOCKET": str(tmp_path / "unseal.sock")}
    membership = types.SimpleNamespace(members=members or {},
                                       quarantine=qview)
    durable = types.SimpleNamespace(
        membership=membership, node_id=node, peer_unseal_handler=None,
        config=types.SimpleNamespace(secret=MESH))
    runtime = types.SimpleNamespace(
        config=types.SimpleNamespace(environ=environ),
        env=Env({"DB": make_db(tmp_path)}), durable=durable, entry=None,
        _db_scope=None, available=[])
    runtime.set_available = runtime.available.append
    return runtime


def sealed_member(node):
    return {node: {"node_id": node, "status": "sealed",
                   "heartbeat_ms": cluster.now_ms()}}


def peer_request(node="n2"):
    private, public = vault_crypto.peer_keypair()
    return json.dumps({"v": 1, "node": node, "nonce": os.urandom(16).hex(),
                       "pub": vault_crypto._b64(public)}).encode()


def test_peer_unseal_is_refused_to_a_quarantined_node(tmp_path):
    async def scenario():
        rq = FakeRqlite([self_row("n2")])
        qview = view("n1", rq=rq)
        await qview.refresh()
        responder = vault.Vault(fake_runtime("n1", tmp_path, qview,
                                             sealed_member("n2")))
        assert (await responder.unseal(KEY))["ok"]
        status, body = await responder.serve_peer("10.77.0.2", peer_request())
        assert (status, body["error"]) == (403, "quarantined")
        # Released but not yet unsealed from the desktop: still refused.
        rq.rows.append(release_row("n2"))
        await qview.refresh()
        status, body = await responder.serve_peer("10.77.0.2", peer_request())
        assert (status, body["error"]) == (403, "quarantined")
        # After the desktop unseal stamped the release row: served.
        rq.rows[-1]["reunsealed_ms"] = now_ms()
        await qview.refresh()
        status, body = await responder.serve_peer("10.77.0.2", peer_request())
        assert status == 200 and body["ok"]
    run(scenario())


def test_a_quarantined_node_refuses_every_unseal(tmp_path):
    async def scenario():
        rq = FakeRqlite([self_row("n2")])
        qview = view("n2", rq=rq)
        await qview.refresh()
        node = vault.Vault(fake_runtime("n2", tmp_path, qview))
        assert (await node.unseal(KEY))["error"] == "quarantined"
        assert (await node.unseal(KEY, source="peer n1"))["error"] == \
            "quarantined"
        assert await node.try_peers() is False
        assert node.sealed
        # Released: a peer's key is still refused; the desktop's works and
        # stamps the release row, after which peers may unseal it again.
        release = release_row("n2")
        rq.rows.append(release)
        await qview.refresh()
        assert (await node.unseal(KEY, source="peer n1"))["error"] == \
            "desktop_unseal_required"
        reply = await node.unseal(KEY)
        assert reply["ok"] and not reply["sealed"]
        assert rq.rows[-1]["reunsealed_ms"]
        await qview.refresh()
        assert not qview.self_needs_reunseal
    run(scenario())


def test_seal_restart_drops_scrubs_and_exits(tmp_path):
    async def scenario():
        node = vault.Vault(fake_runtime("n1", tmp_path, None))
        assert (await node.unseal(KEY))["ok"]
        secret = node.runtime.env.ADMIN_PASS
        exits = []
        node.exit = exits.append

        class Reader:
            async def readline(self):
                return b"seal-restart\n"
        reply = await node._dispatch(Reader())
        assert reply["sealed"] and reply["restarting"]
        assert not hasattr(node.runtime.env, "ADMIN_PASS") or \
            node.runtime.env.ADMIN_PASS != secret
        assert getattr(node.runtime.env, "DATA_KEY", None) is None
        await node._restart()
        assert exits == [0]
        # The dropped values were overwritten in place.
        assert secret != "admin-SECRET" and set(secret) == {"*"}
    run(scenario())


def test_quarantine_of_an_unsealed_node_seals_and_exits_once(tmp_path):
    async def scenario():
        node = vault.Vault(fake_runtime("n1", tmp_path, None))
        exits = []
        node.exit = exits.append
        # Already sealed: nothing to release, no restart loop.
        await node.seal_and_exit()
        await asyncio.sleep(0.3)
        assert exits == []
        assert (await node.unseal(KEY))["ok"]
        await node.seal_and_exit()
        assert node.sealed
        await asyncio.sleep(0.3)
        assert exits == [0]
    run(scenario())


def test_scrub_only_touches_what_it_can():
    value = bytearray(b"0123456789")
    assert vault.scrub(value) and value == bytearray(10)
    assert not vault.scrub("short")
    assert not vault.scrub(12345)


# -- mesh ---------------------------------------------------------------------

def test_mesh_refuses_a_quarantined_source_before_reading():
    from forkmesh_host import durable_cluster

    class Host:
        stats = {"mesh_rejected": 0}
        membership = types.SimpleNamespace(
            source_quarantined=lambda ip: ip == "10.77.0.2")
    sent = []

    async def send(message):
        sent.append(message)

    async def receive():
        raise AssertionError("the body must not be read")
    mesh = durable_cluster.MeshASGI(Host())
    scope = {"type": "http", "method": "POST", "path": "/_rpc/x/y/z",
             "raw_path": b"/_rpc/x/y/z", "headers": [],
             "client": ("10.77.0.2", 5555)}
    run(mesh(scope, receive, send))
    assert sent[0]["status"] == 403
    assert Host.stats["mesh_quarantined"] == 1
