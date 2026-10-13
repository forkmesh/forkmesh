"""IDS quarantine and incident delivery on a real rqlite (docs/IDS.md).

One rqlite node with migrations 0192 (cluster_members), 0197 (the IDS
tables) and 0208 (the mail budget). The root IDS side runs deploy/core/ids.py's own rqlite client and
PeerWatch against it; the app side runs forkmesh_host.quarantine, the vault
and the incident dispatcher. Checks:

* a node's signed lockdown row makes its peers refuse it peer unseal, and
  deleting the rows does not help (sticky);
* after the operator's release peers still refuse it until the released node
  was unsealed from the desktop (which stamps the release row);
* two IDS voters against a silent peer quarantine it; one alone cannot;
* two dispatchers racing on one incident deliver it exactly once.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
import types
import urllib.request
from pathlib import Path

import pytest

pytest.importorskip("cryptography")

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[2]
for path in (HERE.parent / "tests_rqlite", HERE.parent, SRC / "deploy" / "core",
             SRC / "tools"):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import rqlite_cluster  # noqa: E402

import ids  # noqa: E402  (deploy/core/ids.py)
from forkmesh_host import cluster as _cluster  # noqa: E402
from forkmesh_host import ids_alerts, ids_records, quarantine, vault  # noqa: E402
from forkmesh_host import vault_crypto  # noqa: E402
from forkmesh_host.d1_sqlite import D1Sqlite  # noqa: E402
from forkmesh_host.env import Env  # noqa: E402

MIGRATIONS = [SRC / "app" / "migrations" / name for name in (
    "0192_cluster_members_and_do_state.sql", "0197_security_ids.sql",
    "0208_mail_budget.sql")]
NODES = ["n1", "n2", "n3"]
PRIVATE = {n: ids_records.generate_private() for n in NODES}
PUBLIC = {n: ids_records.public_key(k) for n, k in PRIVATE.items()}
OPERATOR = ids_records.generate_private()
OPERATOR_PUB = ids_records.public_key(OPERATOR)
PEERS = ",".join("%s=http://10.77.0.%s:8792" % (n, n[1:]) for n in NODES)
KEY = "ids-cluster-data-key-" + os.urandom(16).hex()
MESH = "ids-cluster-mesh-" + os.urandom(8).hex()


def statements(path):
    import rqlite_migrate   # tools/: the splitter the deploy uses
    return rqlite_migrate.split_statements(path.read_text())


@pytest.fixture(scope="module")
def rqlite_url():
    if rqlite_cluster.rqlited_binary() is None:
        pytest.skip("rqlited not found")
    with rqlite_cluster.Cluster(1) as cluster:
        url = cluster.leader().http
        body = [s for path in MIGRATIONS for s in statements(path)]
        request = urllib.request.Request(
            url + "/db/execute", data=json.dumps(body).encode(),
            headers={"content-type": "application/json"})
        with urllib.request.urlopen(request, timeout=10) as response:
            results = json.loads(response.read())["results"]
        assert not any(r.get("error") for r in results), results
        yield url


def run(coroutine):
    return asyncio.run(coroutine)


async def reset(url):
    rq = _cluster.Rqlite(url)
    await rq.execute([["DELETE FROM cluster_quarantine"],
                      ["DELETE FROM security_incidents"],
                      ["DELETE FROM ids_heartbeats"],
                      ["DELETE FROM cluster_members"]])
    await rq.aclose()


def make_db(tmp_path):
    db = D1Sqlite(tmp_path / "d1.sqlite")
    db._conn.execute(
        "CREATE TABLE IF NOT EXISTS host_secrets (name TEXT PRIMARY KEY, nonce "
        "TEXT NOT NULL, ciphertext TEXT NOT NULL, key_id TEXT NOT NULL, "
        "updated_at INTEGER NOT NULL)")
    vk = vault_crypto.VaultKey(KEY)
    row = vk.check_row()
    db._conn.execute("INSERT OR REPLACE INTO host_secrets VALUES(?,?,?,?,1)",
                     (row["name"], row["nonce"], row["ciphertext"],
                      row["key_id"]))
    return db


def responder_vault(tmp_path, qview):
    environ = {"FORKMESH_VAULT": "1", "FORKMESH_MESH_PEERS": PEERS,
               "FORKMESH_VAULT_PEER_UNSEAL": "1",
               "FORKMESH_VAULT_SOCKET": str(tmp_path / "unseal.sock")}
    members = {"n2": {"node_id": "n2", "status": "sealed",
                      "heartbeat_ms": _cluster.now_ms()}}
    durable = types.SimpleNamespace(
        membership=types.SimpleNamespace(members=members, quarantine=qview),
        node_id="n1", peer_unseal_handler=None,
        config=types.SimpleNamespace(secret=MESH))
    runtime = types.SimpleNamespace(
        config=types.SimpleNamespace(environ=environ),
        env=Env({"DB": make_db(tmp_path)}), durable=durable, entry=None,
        _db_scope=None, available=[])
    runtime.set_available = runtime.available.append
    return vault.Vault(runtime)


def peer_request():
    _private, public = vault_crypto.peer_keypair()
    return json.dumps({"v": 1, "node": "n2", "nonce": os.urandom(16).hex(),
                       "pub": vault_crypto._b64(public)}).encode()


def ids_config(tmp_path, node):
    return {"node": node, "role": "core", "state_dir": str(tmp_path / node),
            "nodes": NODES, "node_public_keys": PUBLIC,
            "operator_public_key": OPERATOR_PUB,
            "heartbeat": {"stale_ms": 180_000},
            "lockdown": {"core_wg_ips_by_node": {
                n: "10.77.0.%s" % n[1:] for n in NODES}}}


def test_lockdown_row_blocks_peer_unseal_until_release_and_desktop_unseal(
        rqlite_url, tmp_path):
    async def scenario():
        await reset(rqlite_url)
        # n2's IDS locks down: its own signed row, through ids.py's client.
        ids_rq = ids.Rqlite([rqlite_url], "", "")
        body = ids_records.self_body("n2", ids.now_ms(), "inc1", "ssh key")
        row = ids_records.make_row("self", "n2", "n2", body,
                                   ids_records.sign(PRIVATE["n2"], "self",
                                                    body))
        ids_rq.execute([ids_records.insert_statement(row)])
        # A forged row for n3 (signed by n2) is stored but never trusted.
        forged = ids_records.self_body("n3", ids.now_ms(), "x", "forged")
        ids_rq.execute([ids_records.insert_statement(ids_records.make_row(
            "self", "n3", "n3", forged, ids_records.sign(PRIVATE["n2"],
                                                         "self", forged)))])
        rq1 = _cluster.Rqlite(rqlite_url)
        view1 = quarantine.QuarantineView("n1", NODES, PUBLIC, OPERATOR_PUB,
                                          rq1, tmp_path / "v1")
        (tmp_path / "v1").mkdir()
        await view1.refresh()
        assert view1.quarantined() == {"n2"}
        responder = responder_vault(tmp_path, view1)
        assert (await responder.unseal(KEY))["ok"]
        status, reply = await responder.serve_peer("10.77.0.2",
                                                   peer_request())
        assert (status, reply["error"]) == (403, "quarantined")
        # Deleting the evidence does not lift the quarantine.
        await rq1.execute([["DELETE FROM cluster_quarantine"]])
        await view1.refresh()
        assert view1.is_quarantined("n2")
        # The operator's release: stored, then the peers still refuse until
        # the node was unsealed from the desktop.
        release_body = ids_records.release_body("n2", ids.now_ms(),
                                                [row["id"]], "n0nce1")
        release = ids_records.make_row(
            "release", "n2", "operator", release_body,
            ids_records.sign(OPERATOR, "release", release_body))
        ids_rq.execute([ids_records.insert_statement(release)])
        await view1.refresh()
        assert not view1.is_quarantined("n2")
        status, reply = await responder.serve_peer("10.77.0.2",
                                                   peer_request())
        assert (status, reply["error"]) == (403, "quarantined")
        rq2 = _cluster.Rqlite(rqlite_url)
        view2 = quarantine.QuarantineView("n2", NODES, PUBLIC, OPERATOR_PUB,
                                          rq2)
        await view2.refresh()
        assert view2.self_needs_reunseal
        assert await view2.mark_reunsealed()
        await view1.refresh()
        status, reply = await responder.serve_peer("10.77.0.2",
                                                   peer_request())
        assert status == 200 and reply["ok"]
        await rq1.aclose()
        await rq2.aclose()
    run(scenario())


def test_silent_ids_needs_two_voters(rqlite_url, tmp_path):
    async def reset_now():
        await reset(rqlite_url)
    run(reset_now())
    rq = ids.Rqlite([rqlite_url], "", "")
    now = ids.now_ms()
    for node in NODES:
        rq.execute([[
            "INSERT INTO cluster_members(node_id, mesh_url, status, "
            "heartbeat_ms, epoch) VALUES(?, ?, 'active', ?, ?)", node,
            "http://x", now, now - 3_600_000]])
    rq.execute([["INSERT INTO ids_heartbeats(node_id, beat_ms, state) "
                 "VALUES('n3', ?, 'normal')", now - 600_000]])
    watches = {}
    for node in ("n1", "n2"):
        config = ids_config(tmp_path, node)
        os.makedirs(config["state_dir"], exist_ok=True)
        store = ids.StateStore(os.path.join(config["state_dir"], "s.json"))
        responder = ids.Responder(config, None, None, store, "",
                                  lambda n=node: PRIVATE[n], rq)
        watches[node] = ids.PeerWatch(config, rq, responder,
                                      lambda n=node: PRIVATE[n])
    events, state = watches["n1"].round()
    assert [e["detector"] for e in events] == ["peer_ids_silent"]
    assert not state.get("n3", {}).get("quarantined")
    events, state = watches["n2"].round()
    assert state["n3"]["quarantined"]
    beats = rq.query("SELECT node_id, state FROM ids_heartbeats "
                     "ORDER BY node_id")
    assert [b["node_id"] for b in beats] == ["n1", "n2", "n3"]
    # The app side agrees: n3 is no longer live anywhere.
    async def app_view():
        client = _cluster.Rqlite(rqlite_url)
        view = quarantine.QuarantineView("n1", NODES, PUBLIC, OPERATOR_PUB,
                                         client)
        await view.refresh()
        await client.aclose()
        return view.quarantined()
    assert run(app_view()) == {"n3"}


def _mail_budget():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "mail_budget_cluster", SRC / "app" / "src" / "mail_budget.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Entry:
    mail_budget = _mail_budget()

    def __init__(self):
        self.pings = []

    async def d1_all(self, env, sql, *args):
        return [{"data": "admin"}]

    async def decrypt_row(self, env, data):
        return {"name": "admin"}

    @staticmethod
    def valid_node_name(name):
        return True

    async def enqueue_notification(self, env, admin, kind, title, **kw):
        await asyncio.sleep(0.05)
        self.pings.append((admin, kind, title))
        return True

    async def _repository_monitor_admin_emails(self, env):
        return []

    async def _send_email(self, *args, **kw):
        return True


def test_two_dispatchers_deliver_an_incident_once(rqlite_url):
    async def scenario():
        await reset(rqlite_url)
        alerter = ids.Alerter({"role": "core"}, ids.Rqlite([rqlite_url], "",
                                                          ""))
        ok, _ = alerter.record({"id": "incident0001", "node": "n2",
                                "ts": ids.now_ms(), "severity": "lockdown",
                                "detector": "ssh_unknown_key",
                                "summary": "unknown key", "detail": {}})
        assert ok
        entry = Entry()
        dispatchers = []
        clients = []
        for node in ("n1", "n3"):
            client = _cluster.Rqlite(rqlite_url)
            clients.append(client)
            runtime = types.SimpleNamespace(sealed=False, entry=entry,
                                            env=object(), _db_scope=None)
            membership = types.SimpleNamespace(rqlite=client, hosting=True,
                                               node_id=node)
            dispatchers.append(ids_alerts.IncidentDispatcher(
                runtime, membership, node))
        delivered = await asyncio.gather(*(d.round() for d in dispatchers))
        assert sorted(delivered) == [0, 1]
        assert len(entry.pings) == 1
        rows = await clients[0].query(
            "SELECT delivered_ms, delivery FROM security_incidents",
            level="strong")
        assert rows[0]["delivered_ms"] and json.loads(rows[0]["delivery"])[
            "pings"] == 1
        for client in clients:
            await client.aclose()
    run(scenario())
