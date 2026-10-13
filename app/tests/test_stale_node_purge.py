#!/usr/bin/env python3
"""Stale-node housekeeping must never turn downtime into catalog deletion."""

import ast
import asyncio
import sqlite3
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
NOW = 1_000_000
ACTIVE_MS = 100_000


def _load_purge(extra_globals):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "purge_stale_registered_nodes"
    )
    namespace = dict(extra_globals)
    exec(
        compile(
            ast.fix_missing_locations(
                ast.Module(body=[node], type_ignores=[])
            ),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    return namespace["purge_stale_registered_nodes"]


def test_stale_node_purge_only_removes_orphaned_registration():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        """
        CREATE TABLE nodes (
            node_bi TEXT PRIMARY KEY,
            name TEXT,
            last_seen INTEGER NOT NULL
        );
        CREATE TABLE repositories (
            key_bi TEXT PRIMARY KEY,
            owner_bi TEXT NOT NULL
        );
        CREATE TABLE mirror_https_endpoints (
            node_bi TEXT PRIMARY KEY,
            node_name TEXT NOT NULL
        );
        CREATE TABLE account_presence (ts INTEGER NOT NULL);
        CREATE TABLE host_presence (
            repo_bi TEXT NOT NULL,
            ts INTEGER NOT NULL
        );

        INSERT INTO nodes VALUES ('repo-node', 'repo-node', 0);
        INSERT INTO nodes VALUES ('endpoint-node', 'endpoint-node', 0);
        INSERT INTO nodes VALUES ('orphan-node', 'orphan-node', 0);
        INSERT INTO repositories VALUES ('repo:kept', 'repo-node');
        INSERT INTO mirror_https_endpoints
            VALUES ('endpoint-node', 'endpoint-node');
        INSERT INTO account_presence VALUES (0);
        INSERT INTO host_presence VALUES ('repo:stale', 0);
        """
    )
    writes = []
    namespace_deletes = []
    cache_purges = []
    republishes = []

    async def ensure_schema(_env):
        return None

    async def d1_all(_env, sql, *args):
        return [
            dict(row)
            for row in db.execute(sql, args).fetchall()
        ]

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))
        db.execute(sql, args)
        db.commit()

    async def active_registered_node_bis(_env, _now):
        return set()

    async def notify_stale_hosts_offline(_env, _cutoff):
        return ["repo:stale"]

    async def delete_repo_namespace(*args):
        namespace_deletes.append(args)
        raise AssertionError("stale-node purge attempted catalog deletion")

    async def purge_catalog_related_caches():
        cache_purges.append(True)

    async def schedule_public_catalog_republish(env, *args, **kwargs):
        republishes.append(True)

    purge = _load_purge({
        "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
        "REGISTERED_NODE_ACTIVE_MS": ACTIVE_MS,
        "STALE_NODE_PURGE_INTERVAL_MS": 1_000,
        "STALE_NODE_PURGE_BATCH": 100,
        "_stale_node_purge": {"ts": 0},
        "ensure_schema": ensure_schema,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "active_registered_node_bis": active_registered_node_bis,
        "notify_stale_hosts_offline": notify_stale_hosts_offline,
        "_delete_repo_namespace": delete_repo_namespace,
        "purge_catalog_related_caches": purge_catalog_related_caches,
        "schedule_public_catalog_republish": schedule_public_catalog_republish,
    })

    removed = asyncio.run(purge(object(), force=True))

    assert removed == 1
    assert namespace_deletes == []
    assert [
        row["node_bi"]
        for row in db.execute(
            "SELECT node_bi FROM nodes ORDER BY node_bi"
        ).fetchall()
    ] == ["endpoint-node", "repo-node"]
    assert db.execute(
        "SELECT COUNT(*) FROM repositories"
    ).fetchone()[0] == 1
    assert db.execute(
        "SELECT COUNT(*) FROM mirror_https_endpoints"
    ).fetchone()[0] == 1
    assert db.execute(
        "SELECT COUNT(*) FROM account_presence"
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM host_presence"
    ).fetchone()[0] == 0
    assert cache_purges == [True]
    # The removed node changes the anonymous catalog: one republish doorbell.
    assert republishes == [True]
    assert not any(
        "DELETE FROM repositories" in sql
        for sql, _args in writes
    )
