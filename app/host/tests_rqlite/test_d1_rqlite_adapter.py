"""forkmesh_host.d1_rqlite against a real rqlite, through the app's helpers."""

from __future__ import annotations

import asyncio
import time

import pytest

import app_d1
import rqlite_cluster
import rqlite_migrate
from conftest import requires_rqlite
from forkmesh_host import d1_rqlite
from forkmesh_host.d1_rqlite import make_d1, request_scope

pytestmark = requires_rqlite


def run(coro):
    return asyncio.run(coro)


@pytest.fixture(params=["http", "local"])
def env(request, migrated_node):
    db = make_d1({"url": migrated_node.http, "read_mode": request.param,
                  "db_path": str(migrated_node.db_path)})
    env = app_d1.Env(db)
    env.node = migrated_node
    env.mode = request.param
    yield env
    run(db.aclose())


def test_schema_fast_path_row_and_read_guard_are_stamped(env):
    fingerprint = rqlite_migrate.runtime_schema_fingerprint()

    async def go():
        row = await app_d1.d1_first(
            env,
            "SELECT v FROM schema_meta "
            "WHERE (k='fingerprint' AND v=?) OR (k=? AND v='applied') "
            "LIMIT 1",
            fingerprint, "schema_fingerprint:" + fingerprint)
        guard = await app_d1.d1_first(
            env, "SELECT mode FROM d1_read_guard WHERE id=1")
        applied = await app_d1.d1_first(
            env, "SELECT count(*) n FROM d1_migrations")
        trigger = await env.DB.prepare(
            "SELECT sql FROM sqlite_master WHERE name=?").bind(
                "trg_chat_channel_member_remove_rotate").first("sql")
        return row, guard, applied, trigger

    row, guard, applied, trigger = run(go())
    assert row == {"v": "applied"}
    assert guard == {"mode": "off"}
    assert applied["n"] == len(rqlite_migrate.migration_files(
        rqlite_migrate.MIGRATIONS))
    assert "now" not in trigger.lower()


def test_first_all_run_meta_and_returning(env):
    async def go():
        await app_d1.d1_run(env, "CREATE TABLE ad_t (id INTEGER PRIMARY KEY, "
                                 "v TEXT UNIQUE, n INTEGER, r REAL, b BLOB)")
        first = await app_d1.d1_run(
            env, "INSERT INTO ad_t (v, n, r, b) VALUES (?,?,?,?)",
            "a", 2 ** 63 - 1, 1.0, b"\x00\xff")
        second = await app_d1.d1_run(
            env, "INSERT INTO ad_t (v, n, r, b) VALUES (?,?,?,?)",
            "b", None, 2.5, None)
        updated = await app_d1.d1_run(env, "UPDATE ad_t SET n = 7")
        noop = await app_d1.d1_run(env, "DELETE FROM ad_t WHERE v='zzz'")
        rows = await app_d1.d1_all(env, "SELECT * FROM ad_t ORDER BY id")
        one = await app_d1.d1_first(env, "SELECT v FROM ad_t WHERE id=?", 2)
        none = await app_d1.d1_first(env, "SELECT v FROM ad_t WHERE id=?", 99)
        empty = await app_d1.d1_all(env, "SELECT v FROM ad_t WHERE 0")
        col = await env.DB.prepare("SELECT v FROM ad_t WHERE id=1").first("v")
        raw = await env.DB.prepare("SELECT id, v FROM ad_t ORDER BY id").raw()
        deleted = await app_d1.d1_first(
            env, "DELETE FROM ad_t WHERE v=? RETURNING id", "b")
        again = await app_d1.d1_first(
            env, "DELETE FROM ad_t WHERE v=? RETURNING id", "b")
        return (first, second, updated, noop, rows, one, none, empty, col,
                raw, deleted, again)

    (first, second, updated, noop, rows, one, none, empty, col, raw,
     deleted, again) = run(go())
    assert first.meta.last_row_id == 1 and app_d1.changes(first) == 1
    assert second.meta.last_row_id == 2
    assert first.success is True
    assert app_d1.changes(updated) == 2
    assert app_d1.changes(noop) == 0
    assert rows[0] == {"id": 1, "v": "a", "n": 7, "r": 1.0, "b": b"\x00\xff"}
    assert isinstance(rows[0]["r"], float)
    assert rows[1]["b"] is None
    assert one == {"v": "b"} and none is None and empty == []
    assert col == "a"
    assert raw == [[1, "a"], [2, "b"]]
    assert deleted == {"id": 2} and again is None


def test_jsnull_bytes_bool_and_int64_arguments(env):
    async def go():
        await app_d1.d1_run(env, "CREATE TABLE ad_args (a, b BLOB, c, d)")
        await app_d1.d1_run(env, "INSERT INTO ad_args VALUES (?,?,?,?)",
                            app_d1.jsnull, bytearray(b"hi"), True,
                            -2 ** 63)
        return await app_d1.d1_first(
            env, "SELECT a, typeof(a) ta, b, typeof(b) tb, c, d FROM ad_args")

    row = run(go())
    assert row == {"a": None, "ta": "null", "b": b"hi", "tb": "blob",
                   "c": 1, "d": -2 ** 63}


def test_sqlite_errors_keep_d1_text(env):
    async def go():
        await app_d1.d1_run(env, "CREATE TABLE ad_u (e TEXT UNIQUE)")
        await app_d1.d1_run(env, "INSERT INTO ad_u VALUES ('x')")
        with pytest.raises(Exception) as unique:
            await app_d1.d1_run(env, "INSERT INTO ad_u VALUES ('x')")
        with pytest.raises(Exception) as missing:
            await app_d1.d1_first(env, "SELECT * FROM no_such_table_here")
        return unique.value, missing.value

    unique, missing = run(go())
    assert str(unique).startswith("D1_ERROR: UNIQUE constraint failed: ad_u.e")
    # entry.py/admin_console.py match exactly this substring.
    assert "UNIQUE constraint failed: ad_u.e" in str(unique)
    assert "no such table" in str(missing).lower()
    # A query bug is not a platform fault: the app keeps it a 500.
    assert not app_d1.is_d1_platform_error(unique)
    assert not app_d1.is_d1_platform_error(missing)


def test_batch_is_atomic(env):
    async def go():
        await app_d1.d1_run(env, "CREATE TABLE ad_b (id INTEGER PRIMARY KEY, "
                                 "v TEXT UNIQUE)")
        ok = await app_d1.d1_run_batch(env, [
            ("INSERT INTO ad_b (v) VALUES (?)", ("a",)),
            ("INSERT INTO ad_b (v) VALUES (?) RETURNING id", ("b",)),
            ("SELECT count(*) n FROM ad_b", ()),
        ])
        with pytest.raises(Exception) as failed:
            await app_d1.d1_run_batch(env, [
                ("INSERT INTO ad_b (v) VALUES (?)", ("c",)),
                ("UPDATE ad_b SET v='z' WHERE v='a'", ()),
                ("INSERT INTO ad_b (v) VALUES (?)", ("b",)),
            ])
        rows = await app_d1.d1_all(env, "SELECT v FROM ad_b ORDER BY id")
        return ok, failed.value, rows

    ok, failed, rows = run(go())
    assert [r.meta.last_row_id for r in ok[:1]] == [1]
    assert ok[1].results == [{"id": 2}]
    assert ok[2].results == [{"n": 2}]
    assert "UNIQUE constraint failed" in str(failed)
    assert rows == [{"v": "a"}, {"v": "b"}]


def test_read_levels_none_then_weak_after_write_and_linearizable_session(
        env, monkeypatch):
    seen = []
    original = d1_rqlite.D1Rqlite._post

    async def spy(self, payload, transaction=False, level=None):
        seen.append(level)
        return await original(self, payload, transaction, level)

    monkeypatch.setattr(d1_rqlite.D1Rqlite, "_post", spy)

    async def request_one():
        async with request_scope():
            await app_d1.d1_first(env, "SELECT 1 x")
            await app_d1.d1_run(env, "CREATE TABLE IF NOT EXISTS ad_l (x)")
            await app_d1.d1_first(env, "SELECT count(*) n FROM ad_l")

    async def request_two():
        async with request_scope():
            await app_d1.d1_first(env, "SELECT 2 x")
            await app_d1.d1_first_primary(env, "SELECT 3 x")

    run(request_one())
    run(request_two())
    if env.mode == "http":
        assert seen == ["none", None, "weak", "none", "linearizable"]
    else:
        # plain reads before any write never reach HTTP in local mode
        assert seen == [None, "weak", "linearizable"]
        assert env.DB.stats["local"] == 2


def test_unreachable_rqlite_is_sustained_overload():
    db = make_d1({"url": "http://127.0.0.1:%d" % rqlite_cluster.free_port(),
                  "timeout": 2})
    env = app_d1.Env(db)

    async def go():
        with pytest.raises(Exception) as read_error:
            await app_d1.d1_first(env, "SELECT 1")
        with pytest.raises(Exception) as write_error:
            await app_d1.d1_run(env, "INSERT INTO x VALUES (1)")
        await db.aclose()
        return read_error.value, write_error.value

    for error in run(go()):
        text = str(error)
        assert text.startswith("D1_ERROR: D1 DB is overloaded")
        assert app_d1.is_d1_platform_error(error)
        # sustained, so _d1_read does not replay it
        assert not app_d1.is_transient_d1_error(error)


def test_follower_without_leader_is_overload():
    with rqlite_cluster.Cluster(3) as cluster:
        leader = cluster.leader()
        follower = cluster.followers()[0]
        leader.request([["CREATE TABLE q (x)"]])
        others = [n for n in cluster.nodes if n is not follower]
        for node in others:
            node.stop()
        db = make_d1({"url": follower.http, "timeout": 5})
        env = app_d1.Env(db)

        async def go():
            # stale local read still works at level=none ...
            rows = await app_d1.d1_all(env, "SELECT * FROM q")
            with pytest.raises(Exception) as write_error:
                await app_d1.d1_run(env, "INSERT INTO q VALUES (1)")
            with pytest.raises(Exception) as primary_error:
                await app_d1.d1_first_primary(env, "SELECT * FROM q")
            await db.aclose()
            return rows, write_error.value, primary_error.value

        rows, write_error, primary_error = run(go())
        assert rows == []
        for error in (write_error, primary_error):
            assert "overload" in str(error).lower(), str(error)
            assert app_d1.is_d1_platform_error(error)


def test_local_mode_survives_restart_by_reopening():
    with rqlite_cluster.Cluster(1) as cluster:
        node = cluster.leader()
        node.request([["CREATE TABLE r (x INTEGER)"],
                      ["INSERT INTO r VALUES (1)"]])
        db = make_d1({"url": node.http, "read_mode": "local",
                      "db_path": str(node.db_path)})
        env = app_d1.Env(db)
        count = "SELECT count(*) n FROM r"

        async def read():
            return (await app_d1.d1_first(env, count))["n"]

        async def go():
            assert await read() == 1
            node.stop()
            node.start()
            node.wait_ready()
            node.request([["INSERT INTO r VALUES (2)"]])
            time.sleep(0.2)
            value = await read()
            await db.aclose()
            return value

        assert run(go()) == 2
        assert db.stats["local"] >= 2


def test_notes_suite_runs_on_rqlite(migrated_node):
    """app/tests/test_notes.py's scenarios with every query on rqlite."""
    import json
    import sys
    import urllib.request

    sys.path.insert(0, str(rqlite_cluster.SRC_ROOT / "app" / "tests"))
    import test_notes

    db = make_d1({"url": migrated_node.http})
    env = app_d1.Env(db)

    class RqliteNotesRuntime(test_notes.FakeRuntime):
        def __init__(self):
            super().__init__()
            self.db = _SyncShim(migrated_node)

        async def d1_all(self, sql, *args):
            return await app_d1.d1_all(env, sql, *args)

        async def d1_first(self, sql, *args):
            return await app_d1.d1_first(env, sql, *args)

        async def d1_run(self, sql, *args):
            return await app_d1.d1_run(env, sql, *args)

        @staticmethod
        def changes(result):
            return app_d1.changes(result)

    class _SyncShim:
        """runtime.db.execute(...) used directly by one test."""

        def __init__(self, node):
            self.node = node

        def execute(self, sql, args=()):
            if sql.startswith("INSERT INTO org_members"):
                sql = ("INSERT INTO org_members(org_bi,member_bi,name,"
                       "created_at) VALUES(?,?,?,0)")
            body = json.dumps([[sql, *args]]).encode()
            request = urllib.request.Request(
                self.node.http + "/db/request?associative", data=body,
                headers={"content-type": "application/json"})
            with urllib.request.urlopen(request) as response:
                out = json.loads(response.read())["results"][0]
            assert "error" not in out, out
            return out

        def commit(self):
            pass

    original = test_notes.FakeRuntime
    test_notes.FakeRuntime = RqliteNotesRuntime
    try:
        for name in ("test_note_lifecycle_acl_versions_and_repository_links",
                     "test_listing_carries_sidebar_status_shares_and_view_counts",
                     "test_private_note_is_hidden_and_organization_share_grants_read"):
            # each scenario on clean note tables
            for table in ("note_views", "note_links", "note_versions",
                          "note_shares", "notes", "org_members"):
                _SyncShim(migrated_node).execute("DELETE FROM %s" % table)
            getattr(test_notes, name)()
    finally:
        test_notes.FakeRuntime = original
    assert db.stats["http"] > 20


def test_chat_member_removal_rotates_and_app_stamps_updated_at(migrated_node):
    """The 0191 trigger plus chat_channels_api._remove_member on rqlite."""
    db = make_d1({"url": migrated_node.http})
    env = app_d1.Env(db)

    async def go():
        await app_d1.d1_run(
            env, "INSERT INTO chat_channels (channel_id,name_bi,data,"
            "created_by_bi,created_at,updated_at,key_version) "
            "VALUES ('c1','n','d','a',1,1,1)")
        await app_d1.d1_run(
            env, "INSERT INTO chat_channel_members VALUES "
            "('c1','m1','d','a',1)")
        gone = await app_d1.d1_first(
            env, "DELETE FROM chat_channel_members WHERE channel_id=? AND "
            "member_bi=? RETURNING member_bi", "c1", "m1")
        again = await app_d1.d1_first(
            env, "DELETE FROM chat_channel_members WHERE channel_id=? AND "
            "member_bi=? RETURNING member_bi", "c1", "m1")
        row = await app_d1.d1_first(
            env, "SELECT key_version, updated_at FROM chat_channels")
        await db.aclose()
        return gone, again, row

    gone, again, row = run(go())
    assert gone == {"member_bi": "m1"} and again is None
    assert row == {"key_version": 2, "updated_at": 1}


def test_migration_report(migrated_image):
    report = migrated_image["report"]
    print("\nMIGRATION-REPORT 1-node: %d files, %.1fs, warnings=%r" % (
        len(report["applied"]), report["wall_seconds"], report["warnings"]))
    assert len(report["applied"]) == report["total"]
