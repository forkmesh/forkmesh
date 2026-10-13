"""Phase 0 step 4: what rqlite does with the SQL the app sends to D1.

Every test runs against real local rqlited processes (1 node and 3 nodes)
with data directories under a fresh temp directory. Results are summarised
in app/host/RQLITE_FINDINGS.md; a test that pins a *surprising* rqlite
behaviour says so in its name, so an rqlite upgrade that changes it fails
loudly here.
"""

from __future__ import annotations

import datetime as dt
import os
import sqlite3
import threading
import time
import uuid

import pytest

import rqlite_cluster
from conftest import requires_rqlite

pytestmark = requires_rqlite

INT64_MAX = 9223372036854775807
INT64_MIN = -9223372036854775808

TRIGGER_0069 = """CREATE TRIGGER IF NOT EXISTS trg_chat_channel_member_remove_rotate
AFTER DELETE ON chat_channel_members
BEGIN
  UPDATE chat_channels
     SET key_version = key_version + 1,
         updated_at = CAST(strftime('%s','now') AS INTEGER) * 1000
   WHERE channel_id = OLD.channel_id;
END"""

CHAT_TABLES = [
    ["""CREATE TABLE IF NOT EXISTS chat_channels (
        channel_id TEXT PRIMARY KEY, name_bi TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL, created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
        key_version INTEGER NOT NULL DEFAULT 1 CHECK (key_version >= 1))"""],
    ["""CREATE TABLE IF NOT EXISTS chat_channel_members (
        channel_id TEXT NOT NULL, member_bi TEXT NOT NULL,
        data TEXT NOT NULL, invited_by_bi TEXT NOT NULL,
        joined_at INTEGER NOT NULL, PRIMARY KEY (channel_id, member_bi))"""],
]


def uniq(prefix="t"):
    return "%s_%s" % (prefix, uuid.uuid4().hex[:10])


def results(node, statements, params="associative"):
    body = node.request(statements, params)
    assert "results" in body, body
    return body["results"]


def rows(node, sql, *args, params="associative&level=none"):
    out = results(node, [[sql, *args]], params)[0]
    assert "error" not in out, out
    return out.get("rows") or []


def wait_same_on_all(cluster, sql, timeout=10.0):
    leader = cluster.leader()
    want = rows(leader, sql, params="associative&level=strong")
    deadline = time.monotonic() + timeout
    while True:
        seen = {n.name: rows(n, sql) for n in cluster.nodes}
        if all(v == want for v in seen.values()) or time.monotonic() > deadline:
            return want, seen
        time.sleep(0.1)


# -- basic D1 surface ------------------------------------------------------


@pytest.mark.parametrize("which", ["cluster1", "cluster3"])
def test_returning_through_db_request_returns_rows(which, request):
    cluster = request.getfixturevalue(which)
    table = uniq()
    node = cluster.nodes[-1]  # a follower on the 3-node cluster
    out = results(node, [
        ["CREATE TABLE %s (id INTEGER PRIMARY KEY, v TEXT)" % table],
        ["INSERT INTO %s (v) VALUES (?), (?) RETURNING id, v" % table,
         "a", "b"],
        ["DELETE FROM %s WHERE v=? RETURNING id" % table, "a"],
    ])
    assert out[1]["rows"] == [{"id": 1, "v": "a"}, {"id": 2, "v": "b"}]
    assert out[1]["types"] == {"id": "integer", "v": "text"}
    assert out[2]["rows"] == [{"id": 1}]


@pytest.mark.parametrize("which", ["cluster1", "cluster3"])
def test_transaction_rolls_back_the_whole_batch_on_error(which, request):
    cluster = request.getfixturevalue(which)
    table = uniq()
    node = cluster.leader()
    results(node, [["CREATE TABLE %s (id INTEGER PRIMARY KEY, v TEXT UNIQUE)"
                    % table], ["INSERT INTO %s (v) VALUES ('seed')" % table]])
    out = results(node, [
        ["INSERT INTO %s (v) VALUES (?)" % table, "new"],
        ["UPDATE %s SET v='changed' WHERE v='seed'" % table],
        ["INSERT INTO %s (v) VALUES (?)" % table, "new"],  # UNIQUE fails
    ], "associative&transaction")
    assert out[-1]["error"] == "UNIQUE constraint failed: %s.v" % table
    assert rows(node, "SELECT v FROM %s ORDER BY id" % table,
                params="associative&level=strong") == [{"v": "seed"}]
    # Without ?transaction the statements before the failure stay applied.
    out = results(node, [
        ["INSERT INTO %s (v) VALUES (?)" % table, "kept"],
        ["INSERT INTO %s (v) VALUES (?)" % table, "kept"],
    ])
    assert "error" in out[1]
    assert rows(node, "SELECT count(*) n FROM %s WHERE v='kept'" % table,
                params="associative&level=strong") == [{"n": 1}]


def test_associative_shape_and_write_metadata(cluster1):
    node = cluster1.leader()
    table = uniq()
    out = results(node, [
        ["CREATE TABLE %s (id INTEGER PRIMARY KEY, v TEXT)" % table],
        ["INSERT INTO %s (v) VALUES (?)" % table, "x"],
        ["INSERT INTO %s (v) VALUES (?)" % table, "y"],
        ["UPDATE %s SET v = v || '!'" % table],
        ["DELETE FROM %s WHERE v = 'nope'" % table],
        ["SELECT id, v FROM %s ORDER BY id" % table],
        ["SELECT id FROM %s WHERE 0" % table],
    ])
    assert out[1]["last_insert_id"] == 1 and out[1]["rows_affected"] == 1
    assert out[2]["last_insert_id"] == 2
    assert out[3]["rows_affected"] == 2
    # Zero is omitted (Go omitempty): a no-op DELETE has no rows_affected.
    assert out[4].get("rows_affected", 0) == 0
    assert out[5]["rows"] == [{"id": 1, "v": "x!"}, {"id": 2, "v": "y!"}]
    # An empty result set: "rows" is absent/None/[] - never a list of lists.
    assert not out[6].get("rows")
    plain = node.request([["SELECT id, v FROM %s ORDER BY id" % table]], "")
    assert plain["results"][0]["columns"] == ["id", "v"]
    assert plain["results"][0]["values"] == [[1, "x!"], [2, "y!"]]


def test_json1_and_window_functions(cluster1):
    node = cluster1.leader()
    out = rows(node, (
        "SELECT json_extract(?, '$.a.b') j, "
        "(SELECT group_concat(value, ',') FROM json_each(?)) e, "
        "json_object('k', 1) o, json_array_length(?) n"),
        '{"a":{"b":[1,2]}}', "[3,4,5]", "[1,2]")
    assert out == [{"j": "[1,2]", "e": "3,4,5", "o": '{"k":1}', "n": 2}]
    out = rows(node, (
        "WITH d(g, x) AS (VALUES ('a',1),('a',2),('b',5)) "
        "SELECT g, x, row_number() OVER (PARTITION BY g ORDER BY x) rn, "
        "sum(x) OVER (PARTITION BY g) s, "
        "lag(x) OVER (ORDER BY x) l FROM d ORDER BY x"))
    assert out == [
        {"g": "a", "x": 1, "rn": 1, "s": 3, "l": None},
        {"g": "a", "x": 2, "rn": 2, "s": 3, "l": 1},
        {"g": "b", "x": 5, "rn": 1, "s": 5, "l": 2},
    ]


def test_int64_real_and_null_round_trip(cluster1):
    node = cluster1.leader()
    table = uniq()
    values = [INT64_MAX, INT64_MIN, 2 ** 53 + 1, -(2 ** 53) - 1, 0]
    results(node, [["CREATE TABLE %s (id INTEGER PRIMARY KEY, n INTEGER, "
                    "r REAL, z)" % table]])
    for value in values:
        results(node, [["INSERT INTO %s (n, r, z) VALUES (?, ?, ?)" % table,
                        value, 1.0, None]])
    got = rows(node, "SELECT n, r, z, typeof(n) t FROM %s ORDER BY id"
               % table, params="associative&level=strong")
    assert [r["n"] for r in got] == values
    assert {r["t"] for r in got} == {"integer"}
    # REAL 1.0 arrives as JSON 1: only "types" says it is a real.
    assert got[0]["r"] == 1 and got[0]["z"] is None
    body = node.request([["SELECT r FROM %s LIMIT 1" % table]],
                        "associative&level=none")
    assert body["results"][0]["types"] == {"r": "real"}


def test_blob_parameters_and_results(cluster1):
    node = cluster1.leader()
    table = uniq()
    payload = bytes([0, 1, 2, 127, 128, 254, 255])
    results(node, [
        ["CREATE TABLE %s (id INTEGER PRIMARY KEY, b BLOB)" % table],
        # A JSON array of byte values binds as a BLOB.
        ["INSERT INTO %s (b) VALUES (?)" % table, list(payload)],
    ])
    got = rows(node, "SELECT b, typeof(b) t, hex(b) h FROM %s" % table,
               params="associative&level=strong")
    assert got[0]["t"] == "blob" and got[0]["h"] == payload.hex().upper()
    # Default result encoding is base64; ?blob_array gives byte arrays.
    import base64
    assert base64.b64decode(got[0]["b"]) == payload
    got = rows(node, "SELECT b FROM %s" % table,
               params="associative&level=none&blob_array")
    assert bytes(got[0]["b"]) == payload


def test_blob_in_a_non_blob_column_comes_back_as_lossy_text(cluster1):
    """rqlite formats values by *declared column type*, not storage class:
    a BLOB stored in an untyped/TEXT column (or produced by an expression
    such as randomblob()) is returned as a string, and bytes that are not
    valid UTF-8 are replaced. The app stores no BLOBs today (every sealed
    payload is TEXT); d1_rqlite's local read mode returns real bytes."""
    node = cluster1.leader()
    table = uniq()
    results(node, [["CREATE TABLE %s (a, t TEXT, b BLOB)" % table],
                   ["INSERT INTO %s VALUES (?, ?, ?)" % table,
                    [104, 105, 255], [104, 105, 255], [104, 105, 255]]])
    got = results(node, [["SELECT a, typeof(a) ta, t, b FROM %s" % table]],
                  "associative&level=strong&blob_array")[0]
    assert got["types"]["a"] == "text" and got["types"]["b"] == "blob"
    row = got["rows"][0]
    assert row["ta"] == "blob"
    assert row["a"] == "hi\ufffd" and row["t"] == "hi\ufffd"
    assert row["b"] == [104, 105, 255]


def test_unique_error_text_and_on_conflict_do_update(cluster1):
    node = cluster1.leader()
    table = uniq()
    results(node, [["CREATE TABLE %s (k TEXT PRIMARY KEY, v INTEGER, "
                    "e TEXT UNIQUE)" % table]])
    out = results(node, [
        ["INSERT INTO %s (k, v, e) VALUES ('a', 1, 'x')" % table],
        ["INSERT INTO %s (k, v, e) VALUES ('b', 1, 'x')" % table],
        ["INSERT INTO %s (k, v) VALUES ('a', 5) "
         "ON CONFLICT(k) DO UPDATE SET v = v + excluded.v" % table],
        ["INSERT INTO %s (k, v) VALUES ('a', 5) "
         "ON CONFLICT(k) DO UPDATE SET v = v + excluded.v RETURNING v" % table],
    ])
    # Exactly SQLite's text, which the app matches with `in`
    # (e.g. "UNIQUE constraint failed: users.email_bi").
    assert out[1]["error"] == "UNIQUE constraint failed: %s.e" % table
    assert out[3]["rows"] == [{"v": 11}]


# -- non-deterministic functions ------------------------------------------


def _utc_offset_seconds():
    return int(dt.datetime.now().astimezone().utcoffset().total_seconds())


def test_rewrite_turns_time_functions_into_literals_in_host_local_time(
        cluster1):
    node = cluster1.leader()
    sql = ("SELECT CAST(strftime('%s','now') AS INTEGER) s, "
           "unixepoch() u, datetime('now') d")
    now = int(time.time())
    rewritten = results(node, [[sql]], "associative")[0]["rows"][0]
    plain = results(node, [[sql]], "associative&norwtime&norwrandom")[0][
        "rows"][0]
    # unixepoch() with no argument is NOT rewritten: always true UTC.
    assert abs(rewritten["u"] - now) <= 2
    assert abs(plain["s"] - now) <= 2
    # The rewritten 'now' is the host's *local* wall time read as UTC: off by
    # the UTC offset on a non-UTC host (0 on a UTC server).
    assert abs((rewritten["s"] - now) - _utc_offset_seconds()) <= 2
    # /db/query never rewrites.
    query = node.query([[sql]])["results"][0]["rows"][0]
    assert abs(query["s"] - now) <= 2


def test_rewrite_mangles_current_timestamp_into_a_string(cluster1):
    node = cluster1.leader()
    sql = "SELECT CURRENT_TIMESTAMP c, datetime('now') d"
    rewritten = results(node, [[sql]], "associative")[0]["rows"][0]
    assert rewritten["c"] == "CURRENT_TIMESTAMP"
    # The mangling happens whenever *any* rewrite re-serializes the
    # statement: with norwtime, a random() elsewhere still triggers it.
    with_random = "SELECT CURRENT_TIMESTAMP c, random() r"
    only_time = results(node, [[with_random]], "associative&norwtime")[0][
        "rows"][0]
    assert only_time["c"] == "CURRENT_TIMESTAMP"
    no_random = results(node, [[sql]], "associative&norwtime")[0]["rows"][0]
    assert no_random["c"][:2] == "20"
    both = results(node, [[with_random]], "associative&norwtime&norwrandom")[
        0]["rows"][0]
    assert both["c"][:2] == "20"
    # A statement with no non-deterministic call is not re-serialized.
    alone = results(node, [["SELECT CURRENT_TIMESTAMP c"]], "associative")[0]
    assert alone["rows"][0]["c"][:2] == "20"


def test_random_is_rewritten_so_replicas_agree(cluster3):
    leader = cluster3.leader()
    table = uniq()
    results(leader, [["CREATE TABLE %s (id INTEGER PRIMARY KEY, r INTEGER, "
                      "b BLOB, ts TEXT)" % table]])
    for _ in range(5):
        results(leader, [["INSERT INTO %s (r, b, ts) VALUES (random(), "
                          "randomblob(4), strftime('%%Y-%%m-%%d %%H:%%M:%%f',"
                          "'now'))" % table]])
    want, seen = wait_same_on_all(
        cluster3, "SELECT r, hex(b) b, ts FROM %s ORDER BY id" % table)
    assert len(want) == 5
    assert all(v == want for v in seen.values())


def test_norwrandom_lets_replicas_diverge(cluster3):
    leader = cluster3.leader()
    table = uniq()
    results(leader, [["CREATE TABLE %s (id INTEGER PRIMARY KEY, r INTEGER)"
                      % table]])
    for _ in range(3):
        results(leader, [["INSERT INTO %s (r) VALUES (random())" % table]],
                "associative&norwrandom&norwtime")
    time.sleep(1.0)
    per_node = {n.name: rows(n, "SELECT r FROM %s ORDER BY id" % table)
                for n in cluster3.nodes}
    assert all(len(v) == 3 for v in per_node.values())
    assert len({tuple(r["r"] for r in v) for v in per_node.values()}) == 3


def _chat_setup(node, params="associative"):
    out = results(node, CHAT_TABLES, params)
    assert all("error" not in r for r in out), out


def _seed_channel(node, channel):
    out = results(node, [
        ["INSERT INTO chat_channels VALUES (?, ?, 'd', 'a', 1, 1, 1)",
         channel, channel + "-name"],
        ["INSERT INTO chat_channel_members VALUES (?, 'm1', 'd', 'a', 1)",
         channel],
        ["INSERT INTO chat_channel_members VALUES (?, 'm2', 'd', 'a', 1)",
         channel],
    ])
    assert all("error" not in r for r in out), out


def test_trigger_0069_body_is_frozen_by_the_rewrite(cluster1):
    """The production trigger, created through /db/request as-is."""
    node = cluster1.leader()
    results(node, [["DROP TRIGGER IF EXISTS "
                    "trg_chat_channel_member_remove_rotate"]])
    _chat_setup(node)
    out = results(node, [[TRIGGER_0069]])
    assert "error" not in out[0], out
    stored = rows(node, "SELECT sql FROM sqlite_master WHERE name="
                  "'trg_chat_channel_member_remove_rotate'")[0]["sql"]
    # 'now' was replaced by a julian-day literal and the text re-serialized.
    assert "'now'" not in stored
    assert "strftime('%s', 24" in stored
    channel = uniq("c")
    _seed_channel(node, channel)
    results(node, [["DELETE FROM chat_channel_members WHERE channel_id=? AND "
                    "member_bi='m1'", channel]])
    first = rows(node, "SELECT updated_at, key_version FROM chat_channels "
                 "WHERE channel_id=?", channel)[0]
    time.sleep(1.2)
    results(node, [["DELETE FROM chat_channel_members WHERE channel_id=? AND "
                    "member_bi='m2'", channel]])
    second = rows(node, "SELECT updated_at, key_version FROM chat_channels "
                  "WHERE channel_id=?", channel)[0]
    assert second["key_version"] == first["key_version"] + 1 == 3
    # Frozen: the second removal, 1.2 s later, stamps the same instant.
    assert second["updated_at"] == first["updated_at"]
    # ...and it is local wall time read as UTC (see the test above).
    assert abs(first["updated_at"] // 1000 - int(time.time())
               - _utc_offset_seconds()) <= 5


def test_trigger_0069_without_rewrite_evaluates_now_per_replica(cluster3):
    leader = cluster3.leader()
    results(leader, [["DROP TRIGGER IF EXISTS "
                      "trg_chat_channel_member_remove_rotate"]])
    _chat_setup(leader)
    # Millisecond precision makes per-replica evaluation visible.
    body = TRIGGER_0069.replace(
        "CAST(strftime('%s','now') AS INTEGER) * 1000",
        "CAST(strftime('%s','now') AS INTEGER) * 1000 + "
        "CAST(substr(strftime('%f','now'), 4) AS INTEGER)")
    out = results(leader, [[body]], "associative&norwtime&norwrandom")
    assert "error" not in out[0], out
    stored = rows(leader, "SELECT sql FROM sqlite_master WHERE name="
                  "'trg_chat_channel_member_remove_rotate'")[0]["sql"]
    assert "'now'" in stored  # text kept verbatim
    samples = []
    for _ in range(6):
        channel = uniq("c")
        _seed_channel(leader, channel)
        results(leader, [["DELETE FROM chat_channel_members WHERE "
                          "channel_id=? AND member_bi='m1'", channel]])
        samples.append(channel)
    time.sleep(1.0)
    diverged = 0
    for channel in samples:
        values = {n.name: rows(n, "SELECT updated_at FROM chat_channels "
                               "WHERE channel_id=?", channel)[0]["updated_at"]
                  for n in cluster3.nodes}
        if len(set(values.values())) > 1:
            diverged += 1
    # Every replica runs the trigger itself at apply time, on its own clock.
    assert diverged >= 1, "expected at least one ms-level divergence"
    results(leader, [["DROP TRIGGER trg_chat_channel_member_remove_rotate"]])


def test_trigger_0191_is_deterministic_on_every_replica(cluster3):
    sql = (rqlite_cluster.SRC_ROOT / "app" / "migrations" /
           "0191_chat_member_rotate_trigger_deterministic.sql").read_text()
    import rqlite_migrate
    statements = [s for s in rqlite_migrate.split_statements(sql)
                  if "schema_meta" not in s]
    leader = cluster3.leader()
    _chat_setup(leader)
    out = results(leader, [[s] for s in statements],
                  "associative&transaction")
    assert all("error" not in r for r in out), out
    stored = rows(leader, "SELECT sql FROM sqlite_master WHERE name="
                  "'trg_chat_channel_member_remove_rotate'")[0]["sql"]
    assert "now" not in stored.lower()
    channel = uniq("c")
    _seed_channel(leader, channel)
    results(leader, [["DELETE FROM chat_channel_members WHERE channel_id=?",
                      channel]])
    want, seen = wait_same_on_all(
        cluster3, "SELECT key_version, updated_at FROM chat_channels WHERE "
        "channel_id='%s'" % channel)
    assert want == [{"key_version": 3, "updated_at": 1}]
    assert all(v == want for v in seen.values())


# -- in-process read-only sqlite3 on rqlite's file -------------------------


def _ro_connect(path):
    conn = sqlite3.connect("file:%s?mode=ro" % path, uri=True, timeout=0.2,
                           isolation_level=None, check_same_thread=False)
    conn.execute("PRAGMA query_only=1")
    return conn


class _Reader(threading.Thread):
    """Reads (count(a), count(b), max(a.id)) as fast as it can.

    Every writer transaction inserts one row into a and one into b, so any
    snapshot with count(a) != count(b) would be a torn read.
    """

    def __init__(self, path, persistent):
        super().__init__(daemon=True)
        self.path = path
        self.persistent = persistent
        self.stop_flag = threading.Event()
        self.errors = {}
        self.torn = 0
        self.backwards = 0
        self.reads = 0
        self.last = 0
        self.conn = None

    def read_once(self):
        conn = self.conn
        if conn is None or not self.persistent:
            if conn is not None:
                conn.close()
            conn = self.conn = _ro_connect(self.path)
        a, b = conn.execute(
            "SELECT (SELECT count(*) FROM a), (SELECT count(*) FROM b)"
        ).fetchone()
        return a, b

    def run(self):
        while not self.stop_flag.is_set():
            try:
                a, b = self.read_once()
            except sqlite3.Error as error:
                key = type(error).__name__ + ": " + str(error)
                self.errors[key] = self.errors.get(key, 0) + 1
                if self.conn is not None:
                    self.conn.close()
                    self.conn = None
                time.sleep(0.001)
                continue
            self.reads += 1
            if a != b:
                self.torn += 1
            if a < self.last:
                self.backwards += 1
            self.last = max(self.last, a)


def _write_pairs(node, count, start=0):
    for i in range(start, start + count):
        out = results(node, [["INSERT INTO a (id) VALUES (?)", i],
                             ["INSERT INTO b (id) VALUES (?)", i]],
                      "associative&transaction")
        assert all("error" not in r for r in out), out


def _wait_reader_sees(reader_fn, want, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if reader_fn() >= want:
                return True
        except sqlite3.Error:
            pass
        time.sleep(0.02)
    return False


@pytest.mark.parametrize("persistent", [True, False],
                         ids=["persistent-conn", "conn-per-read"])
def test_local_readonly_sqlite_under_writes_and_snapshots(persistent):
    extra = ["-raft-snap", "64", "-raft-snap-int", "100ms"]
    with rqlite_cluster.Cluster(1, extra) as cluster:
        node = cluster.leader()
        results(node, [["CREATE TABLE a (id INTEGER PRIMARY KEY)"],
                       ["CREATE TABLE b (id INTEGER PRIMARY KEY)"]])
        snaps_before = node.status()["store"]["raft"]["last_snapshot_index"]
        readers = [_Reader(node.db_path, persistent) for _ in range(3)]
        for r in readers:
            r.start()
        _write_pairs(node, 600)
        time.sleep(0.5)
        for r in readers:
            r.stop_flag.set()
            r.join(5)
        snaps_after = node.status()["store"]["raft"]["last_snapshot_index"]
        assert snaps_after > snaps_before + 500, "snapshots did not run"
        for r in readers:
            assert r.reads > 50
            assert r.torn == 0
            assert r.backwards == 0
            assert r.last == 600, "reader stuck at %d" % r.last
            assert not r.errors, r.errors
        # Short autocommit reads never made a snapshot's checkpoint fail.
        log = node.log_path.read_text(errors="replace")
        assert "failed to take snapshot" not in log


def test_open_read_transaction_blocks_snapshots_until_released():
    """A reader holding a read transaction pins the WAL: rqlite's snapshot
    checkpoint gives up after 250 ms and the snapshot fails (and is retried)
    until the reader lets go. Writes keep working meanwhile."""
    extra = ["-raft-snap", "32", "-raft-snap-int", "100ms"]
    with rqlite_cluster.Cluster(1, extra) as cluster:
        node = cluster.leader()
        results(node, [["CREATE TABLE a (id INTEGER PRIMARY KEY)"],
                       ["CREATE TABLE b (id INTEGER PRIMARY KEY)"]])
        conn = _ro_connect(node.db_path)
        conn.execute("BEGIN")
        pinned = conn.execute("SELECT count(*) FROM a").fetchone()[0]
        _write_pairs(node, 200)
        assert conn.execute("SELECT count(*) FROM a").fetchone()[0] == pinned
        time.sleep(1.0)
        blocked_index = node.status()["store"]["raft"]["last_snapshot_index"]
        log = node.log_path.read_text(errors="replace")
        assert "checkpoint did not complete" in log
        conn.execute("COMMIT")
        assert conn.execute("SELECT count(*) FROM a").fetchone()[0] == 200
        conn.close()
        _write_pairs(node, 100, start=200)
        time.sleep(1.0)
        recovered = node.status()["store"]["raft"]["last_snapshot_index"]
        assert recovered > blocked_index
        assert recovered > 250


def _file_ids(node):
    ids = {}
    for suffix in ("", "-wal", "-shm"):
        try:
            ids[suffix or "db"] = os.stat(str(node.db_path) + suffix).st_ino
        except FileNotFoundError:
            ids[suffix or "db"] = None
    return ids


def _count(conn):
    return conn.execute("SELECT count(*) FROM a").fetchone()[0]


def test_restart_replaces_wal_and_shm_so_open_readers_go_stale_forever():
    """rqlite keeps db.sqlite's inode across a restart but creates new
    -wal/-shm files. A connection opened before the restart keeps the old
    shm mapping and never sees another commit; worse, SQLite shares that
    mapping per *inode per process*, so even a brand-new connection in the
    same process is stale until every old connection is closed."""
    with rqlite_cluster.Cluster(1) as cluster:
        node = cluster.leader()
        results(node, [["CREATE TABLE a (id INTEGER PRIMARY KEY)"],
                       ["CREATE TABLE b (id INTEGER PRIMARY KEY)"]])
        _write_pairs(node, 10)
        old = _ro_connect(node.db_path)
        assert _count(old) == 10
        before = _file_ids(node)
        node.stop()
        node.start()
        node.wait_ready()
        after = _file_ids(node)
        _write_pairs(node, 10, start=10)
        time.sleep(0.3)
        assert after["db"] == before["db"]
        assert after["-wal"] != before["-wal"]
        assert after["-shm"] != before["-shm"]
        assert _count(old) == 10                   # stale ...
        sibling = _ro_connect(node.db_path)
        assert _count(sibling) == 10               # ... process-wide
        time.sleep(1.0)
        assert _count(old) == 10                   # ... and forever
        sibling.close()
        old.close()
        fresh = _ro_connect(node.db_path)
        assert _count(fresh) == 20                 # all closed: fixed
        fresh.close()


def test_snapshot_install_on_a_lagging_follower():
    extra = ["-raft-snap", "64", "-raft-snap-int", "100ms"]
    with rqlite_cluster.Cluster(3, extra) as cluster:
        leader = cluster.leader()
        results(leader, [["CREATE TABLE a (id INTEGER PRIMARY KEY)"],
                         ["CREATE TABLE b (id INTEGER PRIMARY KEY)"]])
        _write_pairs(leader, 50)
        follower = cluster.followers()[0]
        assert _wait_reader_sees(
            lambda: _count(_closing(_ro_connect(follower.db_path))), 50)
        follower.stop()
        before = _file_ids(follower)
        _write_pairs(leader, 400, start=50)
        time.sleep(1.0)
        follower.start(join=leader.raft_addr)
        follower.wait_ready()
        after = _file_ids(follower)
        log = follower.log_path.read_text(errors="replace").lower()
        # The leader had truncated its log, so the follower restored from a
        # snapshot; rqlite rewrote the database in place (same inode) and
        # replaced the WAL and shm files.
        assert "restor" in log or "install" in log
        assert after["db"] == before["db"]
        assert after["-shm"] != before["-shm"]
        # A reader that reopens (no older connection alive) catches up.
        assert _wait_reader_sees(
            lambda: _count(_closing(_ro_connect(follower.db_path))), 450,
            timeout=15)


class _closing:
    """Use a connection for one call and close it."""

    def __init__(self, conn):
        self.conn = conn

    def execute(self, *args):
        try:
            return _Materialized(self.conn.execute(*args).fetchall())
        finally:
            self.conn.close()


class _Materialized:
    def __init__(self, rows):
        self.rows = rows

    def fetchone(self):
        return self.rows[0] if self.rows else None
