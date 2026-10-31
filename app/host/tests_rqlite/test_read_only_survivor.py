from __future__ import annotations

import asyncio
import time

import pytest

import app_d1
import rqlite_cluster
from conftest import requires_rqlite
from forkmesh_host import cluster as host_cluster
from forkmesh_host import d1_rqlite, read_only
from forkmesh_host.d1_rqlite import make_d1

pytestmark = requires_rqlite

MEMBERS = ("CREATE TABLE cluster_members (node_id TEXT PRIMARY KEY, "
           "mesh_url TEXT, status TEXT, heartbeat_ms INTEGER, epoch INTEGER)")


def run(coro):
    return asyncio.run(coro)


def _membership(url):
    config = host_cluster.ClusterConfig.from_environ({
        "FORKMESH_NODE_ID": "n3",
        "FORKMESH_MESH_LISTEN": "127.0.0.1:8792",
        "FORKMESH_MESH_SECRET": "s" * 32,
        "FORKMESH_HOST_DB": "rqlite:" + url,
    })
    membership = host_cluster.Membership(config)
    membership._started = time.monotonic() - 3600
    return membership


async def _settle(membership, want, timeout=20.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            await membership.beat()
        except Exception:
            pass
        await membership.refresh()
        if membership.read_only is want:
            return True
        await asyncio.sleep(0.2)
    return False


def test_survivor_serves_reads_refuses_writes_and_recovers(monkeypatch):
    monkeypatch.setattr(host_cluster, "QUORUM_LOSS_SECONDS", 1.0)
    with rqlite_cluster.Cluster(3) as group:
        leader = group.leader()
        leader.request([[MEMBERS], ["CREATE TABLE notes (id INTEGER PRIMARY "
                                    "KEY, body TEXT)"],
                        ["INSERT INTO notes(body) VALUES ('kept'), "
                         "('delete me')"]])
        survivor = group.followers()[0]
        stopped = [node for node in group.nodes if node is not survivor]
        time.sleep(0.5)
        for node in stopped:
            node.stop()
        membership = _membership(survivor.http)

        async def read_only_phase():
            assert await _settle(membership, True)
            out = {}
            for mode in ("http", "local"):
                db = make_d1({"url": survivor.http, "read_mode": mode,
                              "db_path": str(survivor.db_path),
                              "timeout": 10})
                db.read_only_probe = lambda: membership.read_only
                env = app_d1.Env(db)
                started = time.monotonic()
                rows = await app_d1.d1_all(
                    env, "SELECT body FROM notes ORDER BY id")
                literal = await app_d1.d1_first(
                    env, "SELECT count(*) n FROM notes WHERE body = "
                    "'delete me'")
                missing = await app_d1.d1_first(
                    env, "SELECT * FROM notes WHERE id = 99")
                primary = await app_d1.d1_first_primary(
                    env, "SELECT count(*) n FROM notes")
                with read_only.request_scope("POST", "/api/notes"):
                    with pytest.raises(d1_rqlite.ReadOnlyError) as error:
                        await app_d1.d1_run(
                            env, "INSERT INTO notes(body) VALUES ('new')")
                with read_only.request_scope("GET", "/api/notes"):
                    skipped = await db.prepare(
                        "UPDATE notes SET body = body").run()
                out[mode] = {
                    "rows": [r["body"] for r in rows],
                    "literal": literal["n"], "missing": missing,
                    "primary": primary["n"], "error": error.value,
                    "skipped": skipped.meta.changes,
                    "elapsed": time.monotonic() - started}
                await db.aclose()
            return out

        try:
            results = run(read_only_phase())
        finally:
            for node in stopped:
                node.start()
        for mode, found in results.items():
            assert found["rows"] == ["kept", "delete me"], mode
            assert found["literal"] == 1 and found["missing"] is None
            assert found["primary"] == 2 and found["skipped"] == 0
            assert app_d1.is_d1_platform_error(found["error"])
            assert found["elapsed"] < 3, (mode, found["elapsed"])
        for node in stopped:
            node.wait_ready()
        group.leader(timeout=30)

        async def recovered():
            membership.rqlite = host_cluster.Rqlite(survivor.http)
            assert await _settle(membership, False)
            db = make_d1({"url": survivor.http})
            db.read_only_probe = lambda: membership.read_only
            env = app_d1.Env(db)
            with read_only.request_scope("POST", "/api/notes"):
                await app_d1.d1_run(
                    env, "INSERT INTO notes(body) VALUES ('after')")
            count = await app_d1.d1_first_primary(
                env, "SELECT count(*) n FROM notes")
            await db.aclose()
            await membership.rqlite.aclose()
            return count["n"]

        assert run(recovered()) == 3
