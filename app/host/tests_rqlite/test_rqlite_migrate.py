"""tools/rqlite_migrate.py on a fresh 3-node cluster."""

from __future__ import annotations

import hashlib
import sqlite3
import time

import pytest

import rqlite_cluster
import rqlite_migrate
from conftest import requires_rqlite

pytestmark = requires_rqlite


def _schema_digest(node):
    body = node.query([[
        "SELECT type, name, tbl_name, sql FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name"]])
    rows = body["results"][0].get("rows") or []
    return hashlib.sha256(repr(rows).encode()).hexdigest(), len(rows)


def test_splitter_keeps_trigger_bodies_whole():
    sql = (rqlite_migrate.MIGRATIONS /
           "0064_organization_succession.sql").read_text()
    statements = rqlite_migrate.split_statements(sql)
    triggers = [s for s in statements if "CREATE TRIGGER" in s.upper()]
    assert triggers
    for trigger in triggers:
        assert trigger.rstrip().upper().endswith("END;")
        assert sqlite3.complete_statement(trigger)
    # and the split is what sqlite would execute
    db = sqlite3.connect(":memory:")
    for statement in rqlite_migrate.split_statements(
            (rqlite_migrate.MIGRATIONS / "0069_private_chat_channels.sql")
            .read_text()):
        db.execute(statement)


def test_only_trigger_bodies_with_now_disable_the_rewrite():
    plans = {p.name: rqlite_migrate.plan_file(p)
             for p in rqlite_migrate.migration_files(
                 rqlite_migrate.MIGRATIONS)}
    no_rewrite = sorted(name for name, (_s, rewrite, _w) in plans.items()
                        if not rewrite)
    assert no_rewrite == ["0069_private_chat_channels.sql"]


def test_full_migration_on_a_fresh_three_node_cluster():
    with rqlite_cluster.Cluster(3) as cluster:
        leader = cluster.leader()
        follower = cluster.followers()[0]
        started = time.perf_counter()
        report = rqlite_migrate.migrate(follower.http, log=lambda *_: None)
        seconds = time.perf_counter() - started
        total = len(rqlite_migrate.migration_files(rqlite_migrate.MIGRATIONS))
        assert len(report["applied"]) == total
        slowest = sorted(report["applied"], key=lambda item: -item[2])[:3]
        print("\nMIGRATION-REPORT 3-node: %d files in %.1fs; slowest %s; "
              "warnings %r" % (total, seconds, slowest, report["warnings"]))

        again = rqlite_migrate.migrate(follower.http, force_follower=True,
                                       log=lambda *_: None)
        assert again["applied"] == [] and again["already"] == total

        # Every replica ends with the identical schema, trigger text included.
        deadline = time.monotonic() + 15
        while True:
            digests = {n.name: _schema_digest(n) for n in cluster.nodes}
            if len(set(digests.values())) == 1 or time.monotonic() > deadline:
                break
            time.sleep(0.2)
        assert len(set(digests.values())) == 1, digests
        stored = leader.query([[
            "SELECT sql FROM sqlite_master WHERE name="
            "'trg_chat_channel_member_remove_rotate'"]])
        assert "now" not in stored["results"][0]["rows"][0]["sql"].lower()
        stamp = leader.query([[
            "SELECT v FROM schema_meta WHERE k=?",
            "schema_fingerprint:"
            + rqlite_migrate.runtime_schema_fingerprint()]])
        assert stamp["results"][0]["rows"] == [{"v": "applied"}]


def test_list_mode_reports_pending_without_applying():
    with rqlite_cluster.Cluster(1) as cluster:
        node = cluster.leader()
        report = rqlite_migrate.migrate(node.http, list_only=True,
                                        log=lambda *_: None)
        assert len(report["pending"]) == report["total"]
        assert rqlite_migrate.main(["--url", node.http, "--list"]) == 0


def test_app_schema_replay_creates_tables_no_migration_creates():
    """The fingerprint stamp makes the app skip its own DDL, so the migrator
    must replay it: schema.py creates tables (system_status_minute, ...)
    that no migration does, and /api/status 503s without them."""
    groups = rqlite_migrate.app_schema_groups()
    assert [tolerant for _statements, tolerant in groups] == [
        True, False, True]
    with rqlite_cluster.Cluster(1) as cluster:
        leader = cluster.leader()
        report = rqlite_migrate.migrate(leader.http, log=lambda *_: None)
        assert report["app_schema"]["applied"] > 0
        body = leader.query([[
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name='system_status_minute'"]])
        assert body["results"][0].get("rows")
        # Nothing pending: the replay is skipped unless asked for.
        again = rqlite_migrate.migrate(leader.http, log=lambda *_: None)
        assert "app_schema" not in again
