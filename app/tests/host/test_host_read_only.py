from __future__ import annotations

import asyncio
import time

import pytest

from forkmesh_host import cluster, d1_rqlite, d1_sqlite, dns_rules, read_only
from forkmesh_host.d1_rqlite import D1Config, D1Rqlite


def run(coro):
    return asyncio.run(coro)


def _membership():
    config = cluster.ClusterConfig.from_environ({
        "FORKMESH_NODE_ID": "n1",
        "FORKMESH_MESH_LISTEN": "127.0.0.1:8792",
        "FORKMESH_MESH_SECRET": "s" * 32,
        "FORKMESH_HOST_DB": "rqlite:http://127.0.0.1:4001",
    })
    return cluster.Membership(config)


def test_membership_goes_read_only_without_a_leader_and_back():
    membership = _membership()
    now = time.monotonic()
    membership._started = now - 3600
    membership._leader_seen = now
    membership._write_ok = now
    assert membership.quorum and not membership.read_only
    membership._leader_seen = now - cluster.QUORUM_LOSS_SECONDS - 1
    assert not membership.quorum and membership.read_only
    assert membership.snapshot()["readOnly"] is True
    membership._leader_seen = time.monotonic()
    assert not membership.read_only
    membership._write_ok = now - cluster.QUORUM_LOSS_SECONDS - 6
    assert membership.read_only


def test_membership_is_not_read_only_while_booting_sealed_or_leaving():
    membership = _membership()
    assert not membership.quorum
    assert not membership.read_only
    membership._started = time.monotonic() - 3600
    assert membership.read_only
    membership.paused = True
    assert not membership.read_only
    membership.paused = False
    membership.leaving = True
    assert not membership.read_only


@pytest.mark.parametrize("method,path,reads", [
    ("GET", "/api/status", True),
    ("HEAD", "/", True),
    ("OPTIONS", "/api/accounts/login", True),
    ("POST", "/api/accounts/login", False),
    ("PUT", "/api/notes/1", False),
    ("DELETE", "/api/notes/1", False),
    ("GET", "/api/accounts/verify-email", False),
    ("GET", "/api/marketing-email/unsubscribe/", False),
    ("GET", "/api/integrations/discord/callback", False),
    ("POST", "/alice/demo.git/git-upload-pack", True),
    ("POST", "/alice/demo.git/git-receive-pack", False),
])
def test_which_requests_skip_writes(method, path, reads):
    assert read_only.reads_only(method, path) is reads


def test_refuse_raises_outside_a_request_and_counts():
    before = dict(read_only.STATS)
    with pytest.raises(d1_sqlite.ReadOnlyError) as error:
        read_only.refuse(d1_sqlite.ReadOnlyError)
    assert "overloaded" in str(error.value)
    assert "D1_ERROR" in str(error.value)
    with read_only.request_scope("GET", "/") as state:
        assert read_only.refuse(d1_sqlite.ReadOnlyError) is True
    assert state["skipped"] == 1 and state["refused"] == 0
    assert read_only.STATS["refused"] == before["refused"] + 1
    assert read_only.STATS["skipped"] == before["skipped"] + 1


def test_answer_is_json_for_api_and_html_for_pages():
    status, headers, body = read_only.answer("application/json")
    assert status == 503
    assert headers["retry-after"] == read_only.RETRY_AFTER
    assert headers[read_only.HEADER] == "1"
    assert b'"code": "read_only"' in body
    assert read_only.MESSAGE.encode() in body
    status, headers, body = read_only.answer(
        "text/html,application/xhtml+xml")
    assert status == 503 and headers["content-type"].startswith("text/html")
    assert b"ForkMesh is read-only" in body
    assert read_only.answer("*/*")[1]["content-type"] == "application/json"


@pytest.fixture
def sqlite_db(tmp_path):
    db = d1_sqlite.D1Sqlite(str(tmp_path / "d1.sqlite"))
    run(db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, kind TEXT);"
                "INSERT INTO t(kind) VALUES ('delete'), ('keep');"))
    state = {"on": False}
    db.read_only_probe = lambda: state["on"]
    yield db, state
    db.close()


def test_sqlite_binding_reads_and_refuses_writes_when_read_only(sqlite_db):
    db, state = sqlite_db
    state["on"] = True

    async def go():
        rows = (await db.prepare("SELECT kind FROM t ORDER BY id").all())
        literal = await db.prepare(
            "SELECT count(*) n FROM t WHERE kind = 'delete'").first("n")
        with read_only.request_scope("POST", "/api/x"):
            with pytest.raises(d1_sqlite.ReadOnlyError):
                await db.prepare("INSERT INTO t(kind) VALUES (?)").bind(
                    "new").run()
            with pytest.raises(d1_sqlite.ReadOnlyError):
                await db.batch([db.prepare("SELECT 1"), db.prepare(
                    "DELETE FROM t")])
            with pytest.raises(d1_sqlite.ReadOnlyError):
                await db.exec("DELETE FROM t")
        with read_only.request_scope("GET", "/api/x"):
            skipped = await db.prepare(
                "UPDATE t SET kind='x' RETURNING id").first()
            meta = (await db.prepare("DELETE FROM t").run()).meta
            reads = await db.batch([db.prepare("SELECT 1 AS one")])
        return rows, literal, skipped, meta, reads

    rows, literal, skipped, meta, reads = run(go())
    assert [r["kind"] for r in rows.results] == ["delete", "keep"]
    assert literal == 1
    assert skipped is None and meta.changes == 0
    assert reads[0].results == [{"one": 1}]
    state["on"] = False
    count = run(db.prepare("SELECT count(*) n FROM t").first("n"))
    assert count == 2
    run(db.prepare("INSERT INTO t(kind) VALUES ('back')").run())
    assert run(db.prepare("SELECT count(*) n FROM t").first("n")) == 3


class _SpyRqlite(D1Rqlite):
    def __init__(self, config, answer):
        super().__init__(config)
        self.calls = []
        self.answer = answer

    async def _post(self, payload, transaction=False, level=None,
                    freshness=True):
        self.calls.append({"payload": payload, "level": level,
                           "freshness": freshness,
                           "transaction": transaction})
        return self.answer(payload, level)


def _empty(payload, level):
    del payload, level
    return {"results": [{"columns": [], "types": {}, "rows": []}]}


def test_rqlite_binding_serves_reads_locally_and_never_posts_writes():
    db = _SpyRqlite(D1Config(url="http://127.0.0.1:1", freshness="5s"),
                    _empty)
    db.read_only_probe = lambda: True

    async def go():
        missing = await db.prepare("SELECT * FROM t WHERE id=?").bind(
            1).first()
        primary = await db.withSession("first-primary").prepare(
            "SELECT 1").first()
        with read_only.request_scope("POST", "/api/notes"):
            started = time.monotonic()
            with pytest.raises(d1_rqlite.ReadOnlyError) as error:
                await db.prepare("INSERT INTO t VALUES (1)").run()
            elapsed = time.monotonic() - started
            with pytest.raises(d1_rqlite.ReadOnlyError):
                await db.batch([db.prepare("UPDATE t SET x=1")])
            with pytest.raises(d1_rqlite.ReadOnlyError):
                await db.exec("DELETE FROM t")
        with read_only.request_scope("GET", "/alice"):
            touched = await db.prepare(
                "UPDATE sessions SET seen=? WHERE id=?").bind(1, 2).run()
            returned = await db.prepare(
                "INSERT INTO t VALUES (1) RETURNING id").first()
        return missing, primary, error.value, elapsed, touched, returned

    missing, primary, error, elapsed, touched, returned = run(go())
    assert missing is None and primary is None
    assert elapsed < 0.5
    assert "overloaded" in str(error)
    assert touched.meta.changes == 0 and touched.meta.served_by == "read-only"
    assert returned is None
    assert [call["level"] for call in db.calls] == ["none", "none"]
    assert all(call["freshness"] is False for call in db.calls)
    assert not any("INSERT" in call["payload"][0][0] or "UPDATE" in
                   call["payload"][0][0] for call in db.calls)


def test_rqlite_binding_refuses_an_ambiguous_write_rqlite_cannot_lead():
    def no_leader(payload, level):
        del payload, level
        raise d1_rqlite._overload("leader not found")

    db = _SpyRqlite(D1Config(url="http://127.0.0.1:1"), no_leader)
    db.read_only_probe = lambda: True

    async def go():
        with read_only.request_scope("GET", "/"):
            skipped = await db.prepare(
                "WITH x AS (SELECT 1) INSERT INTO t SELECT * FROM x").run()
        with pytest.raises(d1_rqlite.D1Error) as error:
            await db.prepare("SELECT * FROM t").all()
        return skipped, error.value

    skipped, error = run(go())
    assert skipped.meta.served_by == "read-only"
    assert not isinstance(error, d1_rqlite.ReadOnlyError)


def test_rqlite_binding_is_unchanged_with_a_leader():
    def rows(payload, level):
        del payload, level
        return {"results": [{"columns": ["id"], "types": {"id": "integer"},
                             "rows": [{"id": 1}], "rows_affected": 1}]}

    db = _SpyRqlite(D1Config(url="http://127.0.0.1:1"), rows)
    db.read_only_probe = lambda: False

    async def go():
        with read_only.request_scope("GET", "/"):
            await db.prepare("UPDATE t SET x=1").run()
            await db.withSession("first-primary").prepare(
                "SELECT id FROM t").first()

    run(go())
    assert [call["level"] for call in db.calls] == [None, "linearizable"]


def test_rqlite_local_read_tells_a_write_apart(tmp_path):
    import sqlite3
    path = tmp_path / "db.sqlite"
    writer = sqlite3.connect(str(path), isolation_level=None)
    writer.execute("PRAGMA journal_mode=WAL")
    writer.execute("CREATE TABLE t (kind TEXT)")
    writer.execute("INSERT INTO t VALUES ('create')")
    db = _SpyRqlite(D1Config(url="http://127.0.0.1:1", read_mode="local",
                             db_path=str(path)), _empty)
    db.read_only_probe = lambda: True

    async def go():
        literal = await db.prepare(
            "SELECT count(*) n FROM t WHERE kind = 'create'").first("n")
        with read_only.request_scope("POST", "/api/x"):
            with pytest.raises(d1_rqlite.ReadOnlyError):
                await db.prepare(
                    "WITH x AS (SELECT 1) INSERT INTO t SELECT 'a' FROM x"
                ).run()
        await db.aclose()
        return literal

    try:
        assert run(go()) == 1
    finally:
        writer.close()
    assert db.calls == []


@pytest.mark.parametrize("states,expected", [
    ({"n3": "ready", "n4": "read_only", "n5": "down"},
     {"n3": True, "n4": False, "n5": False}),
    ({"n3": "read_only", "n4": "down", "n5": "down"},
     {"n3": True, "n4": False, "n5": False}),
    ({"n3": True, "n4": False, "n5": "read_only"},
     {"n3": True, "n4": False, "n5": False}),
    ({"n3": "down", "n4": "down"}, {"n3": False, "n4": False}),
])
def test_dns_counts_read_only_cores_only_when_none_is_ready(states, expected):
    assert dns_rules.serving(states) == expected


def test_dns_probe_reads_the_read_only_flag():
    assert dns_rules.read_only_body(b'{"ok": false, "readOnly": true}')
    assert not dns_rules.read_only_body(b'{"ok": false}')
    assert not dns_rules.read_only_body(b"Service Unavailable")


def _runtime_stub(read_only_now, sealed=False):
    from types import SimpleNamespace
    calls = []
    return SimpleNamespace(read_only=read_only_now, sealed=sealed,
                           _read_only_on=False,
                           _set_queues=calls.append), calls


def test_runtime_pauses_queues_while_read_only_and_resumes():
    from forkmesh_host.runtime import HostRuntime
    stub, calls = _runtime_stub(True)
    assert HostRuntime.sync_read_only(stub) is True
    assert stub._read_only_on is True and calls == [False]
    assert HostRuntime.sync_read_only(stub) is False
    stub.read_only = False
    assert HostRuntime.sync_read_only(stub) is True
    assert stub._read_only_on is False and calls == [False, True]
    sealed, sealed_calls = _runtime_stub(True, sealed=True)
    assert HostRuntime.sync_read_only(sealed) is False
    assert sealed_calls == []


@pytest.mark.parametrize("read_only_now,sealed,reasons,serving", [
    (True, False, ["rqlite_no_leader", "not_hosting"], True),
    (True, False, ["rqlite_unreachable:ConnectError"], False),
    (True, False, ["quarantined", "rqlite_no_leader"], False),
    (True, True, ["sealed"], False),
    (False, False, ["rqlite_no_leader"], False),
])
def test_readyz_flags_only_a_node_that_can_serve_reads(read_only_now, sealed,
                                                       reasons, serving):
    from types import SimpleNamespace
    from forkmesh_host.asgi import HostASGI
    stub = SimpleNamespace(runtime=SimpleNamespace(read_only=read_only_now,
                                                   sealed=sealed))
    assert HostASGI._serving_read_only(stub, reasons) is serving
