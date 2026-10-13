#!/usr/bin/env python3
"""Transient HTTPS verifier failures preserve the last signed health lease."""

import ast
import asyncio
import json
import sqlite3
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import quote, urlparse

import pytest


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
NOW = 1_700_000_000_000


def _function_source(name):
    source = ENTRY.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(source, node)


def test_transient_marker_preserves_last_verified_lease():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        """
        CREATE TABLE mirror_https_endpoints (
            node_bi TEXT PRIMARY KEY,
            checked_at INTEGER NOT NULL,
            healthy INTEGER NOT NULL,
            integrity TEXT NOT NULL,
            forkmesh_verified_at INTEGER NOT NULL,
            forkmesh_active INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );
        INSERT INTO mirror_https_endpoints VALUES (
            'mirror-bi', 700, 1, 'ok', 700, 1, 700
        );
        """
    )

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    namespace = {"d1_run": d1_run}
    exec(_function_source("_https_mirror_mark_transient"), namespace)
    asyncio.run(namespace["_https_mirror_mark_transient"](
        object(), {"node_bi": "mirror-bi"}, 900
    ))

    row = dict(db.execute(
        "SELECT * FROM mirror_https_endpoints WHERE node_bi='mirror-bi'"
    ).fetchone())
    assert row == {
        "node_bi": "mirror-bi",
        "checked_at": 700,
        "healthy": 1,
        "integrity": "ok",
        "forkmesh_verified_at": 700,
        "forkmesh_active": 1,
        "updated_at": 900,
    }


def _dns_result(*, documents, fetch_result=(200, '{"Status":0}'),
                proxied=False, base_url="https://mirror.example"):
    calls = []

    async def edge_documents():
        return documents

    async def fetch_text(*args):
        calls.append(args)
        return fetch_result

    routing = SimpleNamespace(
        normalize_base_url=lambda value: value,
        cloudflare_proxied_dns_answers=(
            lambda payloads, cidrs: bool(proxied)
        ),
    )
    namespace = {
        "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
        "https_routing": routing,
        "urlparse": urlparse,
        "quote": quote,
        "json": json,
        "_HTTPS_MIRROR_EDGE_DNS_MEMO": {},
        "HTTPS_MIRROR_EDGE_DNS_TTL_MS": 120_000,
        "HTTPS_MIRROR_MANIFEST_MAX_BYTES": 16_384,
        "HTTPS_MIRROR_CONTROL_FETCH_TIMEOUT_SECONDS": 5,
        "_cloudflare_edge_range_documents": edge_documents,
        "_https_mirror_fetch_text": fetch_text,
    }
    exec(_function_source("_https_mirror_cloudflare_dns_ok"), namespace)
    result = asyncio.run(
        namespace["_https_mirror_cloudflare_dns_ok"](base_url)
    )
    return result, calls


def test_cloudflare_dns_check_distinguishes_unavailable_from_rejected():
    unavailable, calls = _dns_result(documents=())
    assert unavailable is None
    assert calls == []

    unavailable, calls = _dns_result(
        documents=("173.245.48.0/20", "2400:cb00::/32"),
        fetch_result=(0, ""),
    )
    assert unavailable is None
    assert len(calls) == 1

    rejected, calls = _dns_result(
        documents=("173.245.48.0/20", "2400:cb00::/32"),
        proxied=False,
    )
    assert rejected is False
    assert len(calls) == 2

    invalid, calls = _dns_result(
        documents=("173.245.48.0/20", "2400:cb00::/32"),
        base_url="",
    )
    assert invalid is False
    assert calls == []


@pytest.mark.parametrize(
    ("dns_result", "fetch_result", "expected_marker"),
    [
        (None, (200, "{}"), "transient"),
        (True, (0, ""), "transient"),
        (True, (503, ""), "transient"),
        (True, (530, "error code: 1033"), "transient"),
        (True, (401, ""), "failed"),
        (True, (403, ""), "failed"),
        (True, (404, ""), "failed"),
    ],
)
def test_health_probe_distinguishes_transient_from_definitive_failures(
        dns_result, fetch_result, expected_marker):
    events = []

    async def owner_signing_pubkeys(_env, _node):
        return ("node-public-key",)

    async def dns_ok(_base_url):
        events.append("dns")
        return dns_result

    async def fetch_text(*_args):
        events.append("fetch")
        return fetch_result

    async def mark_transient(_env, _row, _now):
        events.append("transient")

    async def mark_failed(_env, _row, _now):
        events.append("failed")

    namespace = {
        "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: value == "mirror",
        "valid_node_pubkey": lambda value: value == "node-public-key",
        "https_routing": SimpleNamespace(
            normalize_base_url=lambda value: str(value or "")
        ),
        "_owner_signing_pubkeys": owner_signing_pubkeys,
        "_claimed_node_signing_pubkeys": owner_signing_pubkeys,
        "_https_mirror_cloudflare_dns_ok": dns_ok,
        "_https_mirror_fetch_text": fetch_text,
        "_https_mirror_mark_transient": mark_transient,
        "_https_mirror_mark_failed": mark_failed,
        "_b64url_encode": lambda _value: "nonce",
        "_random_bytes": lambda _size: b"nonce",
        "quote": quote,
        "json": json,
        "HTTPS_MIRROR_MANIFEST_MAX_BYTES": 16_384,
        "HTTPS_MIRROR_CONTROL_FETCH_TIMEOUT_SECONDS": 5,
        "HTTPS_MIRROR_HEALTH_TRANSIENT_STATUSES": {
            408, 425, 429, 500, 502, 503, 504, 530,
        },
    }
    exec(_function_source("_https_mirror_health_one"), namespace)

    active = asyncio.run(namespace["_https_mirror_health_one"](
        object(),
        {
            "node_bi": "mirror-bi",
            "node_name": "mirror",
            "base_url": "https://mirror.example",
            "public_key": "node-public-key",
        },
        frozenset({"a" * 64}),
    ))

    assert active is False
    assert events[-1] == expected_marker
    assert (
        "failed" not in events
        if expected_marker == "transient"
        else "transient" not in events
    )
    if dns_result is None:
        assert "fetch" not in events
    else:
        assert "fetch" in events


def _load_health_cron(*, rows, accepted, batch=10, recovery_node=""):
    selected = []
    queries = []

    async def ensure_schema(_env):
        return None

    async def d1_all(_env, sql, *args):
        queries.append((sql, args))
        return rows(sql, args) if callable(rows) else list(rows)

    async def accepted_refs(_env):
        return accepted

    async def health_one(_env, row, refs):
        selected.append((row["node_name"], refs))
        return True

    namespace = {
        "Date": type("D", (), {"now": staticmethod(lambda: 1_000)}),
        "HTTPS_MIRROR_HEALTH_BATCH": batch,
        "HTTPS_MIRROR_STATUS_FRESH_MS": 10_000,
        "ensure_schema": ensure_schema,
        "d1_all": d1_all,
        "_https_mirror_accepted_forkmesh_refs": accepted_refs,
        "_https_mirror_health_one": health_one,
        "_https_mirror_flagship_recovery_node": (
            lambda _env: recovery_node),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 80,
    }
    exec(_function_source("https_mirror_health_cron"), namespace)
    asyncio.run(namespace["https_mirror_health_cron"](object()))
    return selected, queries


def test_health_cron_skips_endpoint_checks_without_accepted_refs():
    selected, queries = _load_health_cron(
        rows=[{
            "node_bi": "mirror-bi",
            "node_name": "mirror",
            "base_url": "https://mirror.example",
            "public_key": "node-public-key",
        }],
        accepted=frozenset(),
    )

    assert len(queries) == 1
    assert selected == []


def test_health_cron_retries_one_fresh_unaccepted_endpoint_when_all_are_inactive():
    inactive = [{
        "node_bi": "mirror-bi",
        "node_name": "mirror29",
        "base_url": "https://mirror29.example",
        "public_key": "node-public-key",
    }]

    def rows(sql, args):
        # Every healthy, recently answering endpoint rotates: an inactive
        # row with no stored digest (a signed mid-generation "unavailable"
        # proof) must not fall out of the cron for good.
        assert "healthy=1" in sql
        assert "forkmesh_refs_sha256!=''" not in sql
        assert "forkmesh_verified_at>=?" not in sql
        assert args == (-9_000, "", 1)
        return inactive

    refs = frozenset({"a" * 64})
    selected, queries = _load_health_cron(
        rows=rows,
        accepted=refs,
        batch=1,
    )

    assert len(queries) == 1
    assert selected == [("mirror29", refs)]


def test_health_cron_prioritizes_designated_successor_when_all_are_inactive():
    inactive = [{
        "node_bi": "mirror-bi",
        "node_name": "mirror29",
        "base_url": "https://mirror29.example",
        "public_key": "node-public-key",
    }]

    def rows(sql, args):
        assert (
            "CASE WHEN forkmesh_active=0 AND node_name=? THEN 0 ELSE 1 END"
            in sql
        )
        assert args == (-9_000, "mirror29", 1)
        return inactive

    refs = frozenset({"a" * 64})
    selected, queries = _load_health_cron(
        rows=rows,
        accepted=refs,
        batch=1,
        recovery_node="mirror29",
    )

    assert len(queries) == 1
    assert selected == [("mirror29", refs)]


def test_health_cron_rechecks_successor_after_its_signed_generation_advances():
    endpoint = {
        "node_bi": "mirror-bi",
        "node_name": "mirror29",
        "base_url": "https://mirror29.example",
        "public_key": "node-public-key",
    }
    old_refs = frozenset({"a" * 64})
    new_refs = frozenset({"b" * 64})
    accepted_calls = []
    health_calls = []

    async def ensure_schema(_env):
        return None

    async def d1_all(_env, sql, *_args):
        return [endpoint]

    async def accepted_refs(_env):
        accepted_calls.append(True)
        return old_refs if len(accepted_calls) == 1 else new_refs

    async def health_one(_env, row, refs):
        assert row == endpoint
        health_calls.append(refs)
        return refs == new_refs

    namespace = {
        "Date": type("D", (), {"now": staticmethod(lambda: 1_000)}),
        "HTTPS_MIRROR_HEALTH_BATCH": 1,
        "HTTPS_MIRROR_STATUS_FRESH_MS": 10_000,
        "MAX_NODE_NAME": 80,
        "ensure_schema": ensure_schema,
        "d1_all": d1_all,
        "_https_mirror_accepted_forkmesh_refs": accepted_refs,
        "_https_mirror_health_one": health_one,
        "_https_mirror_flagship_recovery_node": lambda _env: "mirror29",
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    exec(_function_source("https_mirror_health_cron"), namespace)
    asyncio.run(namespace["https_mirror_health_cron"](object()))

    assert len(accepted_calls) == 2
    assert health_calls == [old_refs, new_refs]


def test_health_cron_schedules_by_latest_attempt_not_old_success():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        """
        CREATE TABLE mirror_https_endpoints (
            node_bi TEXT PRIMARY KEY,
            node_name TEXT NOT NULL,
            base_url TEXT NOT NULL,
            public_key TEXT NOT NULL,
            checked_at INTEGER NOT NULL,
            forkmesh_verified_at INTEGER NOT NULL,
            forkmesh_refs_sha256 TEXT NOT NULL,
            forkmesh_active INTEGER NOT NULL,
            healthy INTEGER NOT NULL,
            integrity TEXT NOT NULL,
            abuse_blocked INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            mirror_url TEXT NOT NULL DEFAULT ''
        );
        INSERT INTO mirror_https_endpoints VALUES
            ('flaky-bi', 'flaky', 'https://flaky.example', 'key',
             900, 900, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 1, 1, 'ok', 0, 900, ''),
            ('old-bi', 'old', 'https://old.example', 'key',
             100, 100, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 1, 1, 'ok', 0, 100, ''),
            ('middle-bi', 'middle', 'https://middle.example', 'key',
             200, 200, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 1, 1, 'ok', 0, 50, '');
        """
    )

    def query_rows(sql, args):
        return [
            dict(row)
            for row in db.execute(sql, args).fetchall()
        ]

    refs = frozenset({"a" * 64})
    selected, queries = _load_health_cron(
        rows=query_rows,
        accepted=refs,
        batch=2,
    )

    assert selected == [("old", refs), ("middle", refs)]
    sql, args = queries[0]
    assert "updated_at" in sql
    assert "checked_at" in sql
    assert args == (-9_000, "", 2)


def test_health_cron_rotates_active_and_recoverable_rows_and_advances_throwing_attempts():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        """
        CREATE TABLE mirror_https_endpoints (
            node_bi TEXT PRIMARY KEY,
            node_name TEXT NOT NULL,
            base_url TEXT NOT NULL,
            public_key TEXT NOT NULL,
            checked_at INTEGER NOT NULL,
            forkmesh_verified_at INTEGER NOT NULL,
            forkmesh_refs_sha256 TEXT NOT NULL,
            forkmesh_active INTEGER NOT NULL,
            healthy INTEGER NOT NULL,
            integrity TEXT NOT NULL,
            abuse_blocked INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            mirror_url TEXT NOT NULL DEFAULT ''
        );
        """
    )
    for index in range(11):
        db.execute(
            "INSERT INTO mirror_https_endpoints VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                f"mirror-{index}-bi",
                f"mirror-{index}",
                f"https://mirror-{index}.example",
                "key",
                index,
                index,
                "a" * 64,
                1 if index < 5 else 0,
                1,
                "ok",
                0,
                index,
                "",
            ),
        )
    db.commit()
    attempted = []
    clock = [1_000]

    async def ensure_schema(_env):
        return None

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def accepted_refs(_env):
        return frozenset({"a" * 64})

    async def health_one(_env, row, _refs):
        attempted.append(row["node_name"])
        clock[0] += 1
        if row["node_name"] == "mirror-0":
            raise RuntimeError("bounded verifier failure")
        await d1_run(
            _env,
            "UPDATE mirror_https_endpoints SET updated_at=? WHERE node_bi=?",
            clock[0],
            row["node_bi"],
        )
        return True

    async def mark_transient(env, row, now):
        await d1_run(
            env,
            "UPDATE mirror_https_endpoints SET updated_at=? WHERE node_bi=?",
            now,
            row["node_bi"],
        )

    namespace = {
        "Date": type("D", (), {"now": staticmethod(lambda: clock[0])}),
        "HTTPS_MIRROR_HEALTH_BATCH": 1,
        "HTTPS_MIRROR_STATUS_FRESH_MS": 10_000,
        "ensure_schema": ensure_schema,
        "d1_all": d1_all,
        "_https_mirror_accepted_forkmesh_refs": accepted_refs,
        "_https_mirror_health_one": health_one,
        "_https_mirror_mark_transient": mark_transient,
        "_https_mirror_flagship_recovery_node": lambda _env: "",
    }
    exec(_function_source("https_mirror_health_cron"), namespace)
    for _ in range(6):
        asyncio.run(namespace["https_mirror_health_cron"](object()))

    assert attempted == [f"mirror-{index}" for index in range(6)]


def test_health_cron_completes_a_quorum_for_a_commit_in_one_tick():
    # One endpoint a minute meant each commit deactivated the mirrors in turn,
    # and git_hosting went red with three honest mirrors serving the same
    # refs (2026-09-14). A probe that finds its endpoint ahead of the pins now
    # challenges one more endpoint so two agreeing proofs form the quorum.
    old, new = "a" * 64, "b" * 64
    rows = [
        {"node_bi": "m35", "node_name": "mirror35"},
        {"node_bi": "m36", "node_name": "mirror36"},
        {"node_bi": "m37", "node_name": "mirror37"},
    ]
    calls = []
    stored = set()
    limits = []

    async def ensure_schema(_env):
        return None

    async def d1_all(_env, _sql, *args):
        limits.append(args[-1])
        return rows[:args[-1]]

    async def accepted_refs(_env):
        return frozenset({old, new}) if len(stored) >= 2 else frozenset({old})

    async def health_one(_env, row, refs):
        calls.append((row["node_name"], new in refs))
        stored.add(row["node_name"])
        return new in refs

    async def mark_transient(_env, _row, _now):
        return None

    def run():
        namespace = {
            "Date": type("D", (), {"now": staticmethod(lambda: 1_000)}),
            "HTTPS_MIRROR_HEALTH_BATCH": 1,
            "HTTPS_MIRROR_STATUS_FRESH_MS": 10_000,
            "MAX_NODE_NAME": 80,
            "ensure_schema": ensure_schema,
            "d1_all": d1_all,
            "_https_mirror_accepted_forkmesh_refs": accepted_refs,
            "_https_mirror_health_one": health_one,
            "_https_mirror_mark_transient": mark_transient,
            "_https_mirror_flagship_recovery_node": lambda _env: "",
            "clean_string": lambda value, limit: str(value or "")[:limit],
        }
        exec(_function_source("https_mirror_health_cron"), namespace)
        asyncio.run(namespace["https_mirror_health_cron"](object()))

    run()
    assert limits == [1, 2]
    assert calls == [
        ("mirror35", False),
        ("mirror36", False),
        ("mirror36", True),
        ("mirror35", True),
    ]

    # A tick whose first probe is admitted issues one query and one probe.
    calls.clear()
    limits.clear()
    run()
    assert limits == [1]
    assert calls == [("mirror35", True)]


def test_health_batch_has_ten_minute_lease_margin_and_bounded_writes():
    source = ENTRY.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(ENTRY))
    batch = next(
        ast.literal_eval(node.value)
        for node in tree.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name)
            and target.id == "HTTPS_MIRROR_HEALTH_BATCH"
            for target in node.targets
        )
    )
    assert batch == 1
    assert (5 + batch - 1) // batch == 5
    # Conservatively bill each successful endpoint update as table + health
    # index, plus one unindexed-column node activity update.
    assert batch * 3 * 1440 == 4320


# --- A healthy mirror mid-update keeps its unexpired lease -------------------
#
# Every merge to main reaches the whole fleet within seconds, and each mirror
# re-registers (and is challenged) right then. A signed proof taken between the
# refs landing on disk and the gateway adopting them says "unavailable"; one
# taken just after names a digest the pin set has not admitted. Revoking on
# either zeroed all leases at once: git_hosting went red for a minute or two
# after every merge (2026-09-16/17).

FRESH_MS = 10 * 60 * 1000


def _lease_db(*, active, verified_at, checked_at=None, refs="a" * 64):
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute(
        """CREATE TABLE mirror_https_endpoints (
               node_bi TEXT PRIMARY KEY, checked_at INTEGER, latency_ms INTEGER,
               healthy INTEGER, integrity TEXT, health_sig TEXT,
               forkmesh_verified_at INTEGER, forkmesh_refs_sha256 TEXT,
               forkmesh_operations_sha256 TEXT, forkmesh_operations_json TEXT,
               forkmesh_active INTEGER, health_message TEXT,
               updated_at INTEGER)""")
    db.execute("CREATE TABLE nodes (node_bi TEXT PRIMARY KEY, last_seen INTEGER)")
    db.execute(
        "INSERT INTO mirror_https_endpoints VALUES "
        "('mirror-bi',?,?,1,'ok','old-sig',?,?,'old-ops','[\"tree\"]',?,"
        "'old-message',?)",
        (
            verified_at if checked_at is None else checked_at, 80,
            verified_at, refs, 1 if active else 0, verified_at,
        ),
    )
    db.execute("INSERT INTO nodes VALUES ('mirror-bi', 0)")
    return db


def _lease_namespace(db):
    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)

    namespace = {
        "d1_first": d1_first,
        "d1_run": d1_run,
        "json": json,
        "HTTPS_MIRROR_STATUS_FRESH_MS": FRESH_MS,
    }
    exec(_function_source("_https_mirror_mark_transient"), namespace)
    exec(_function_source("_https_mirror_hold_lease"), namespace)
    return namespace


def _hold(db, *, refs_digest):
    namespace = _lease_namespace(db)
    return asyncio.run(namespace["_https_mirror_hold_lease"](
        object(), {"node_bi": "mirror-bi"}, NOW, 42,
        signature="new-sig", message="new-message", refs_digest=refs_digest,
        operations_digest="new-ops", operations=["blob", "tree"],
    ))


def _lease_row(db):
    return dict(db.execute("SELECT * FROM mirror_https_endpoints").fetchone())


def test_mid_generation_proof_holds_a_live_lease_untouched():
    verified = NOW - 3 * 60 * 1000
    db = _lease_db(active=True, verified_at=verified)

    assert _hold(db, refs_digest="") is True

    row = _lease_row(db)
    # Only the attempt is recorded; the last admitted proof stays the evidence.
    assert row["updated_at"] == NOW
    assert row["forkmesh_active"] == 1
    assert row["forkmesh_verified_at"] == verified
    assert row["checked_at"] == verified
    assert row["forkmesh_refs_sha256"] == "a" * 64
    assert row["health_sig"] == "old-sig"
    assert db.execute("SELECT last_seen FROM nodes").fetchone()[0] == NOW


def test_unadmitted_digest_is_published_for_the_quorum_without_renewing():
    verified = NOW - 3 * 60 * 1000
    db = _lease_db(active=True, verified_at=verified)

    assert _hold(db, refs_digest="b" * 64) is True

    row = _lease_row(db)
    assert row["forkmesh_active"] == 1
    # Never advanced here: only an admitted digest renews a lease, so a mirror
    # that stays divergent still drops out at the freshness deadline.
    assert row["forkmesh_verified_at"] == verified
    assert row["checked_at"] == NOW
    assert row["healthy"] == 1 and row["integrity"] == "ok"
    assert row["forkmesh_refs_sha256"] == "b" * 64
    assert row["forkmesh_operations_json"] == '["blob","tree"]'
    assert (row["health_sig"], row["health_message"]) == (
        "new-sig", "new-message")


@pytest.mark.parametrize("active, verified_at", [
    (False, NOW - 60 * 1000),
    (True, NOW - FRESH_MS - 1),
    (True, 0),
    (True, NOW + 60 * 1000),
])
def test_no_live_lease_is_never_held(active, verified_at):
    db = _lease_db(active=active, verified_at=verified_at)
    before = _lease_row(db)

    assert _hold(db, refs_digest="b" * 64) is False
    assert _lease_row(db) == before


def _signed_health_namespace(*, proof, lease_held, events, node_ok=True):
    async def signing_pubkeys(_env, _node):
        return ("node-public-key",)

    async def dns_ok(_base_url):
        return True

    message = "signed-health-message"
    body = {
        "ok": node_ok,
        "integrity": "ok" if node_ok else "degraded",
        "node": "mirror",
        "publicKey": "node-public-key",
        "transport": "direct-https",
        "repositoryProof": proof,
        "challenge": {
            "messageType": "forkmesh-https-health-repository-v1",
            "nonce": "nonce",
            "issuedAt": NOW,
            "algorithm": "Ed25519",
            "encoding": "base64url-no-padding",
            "messageSha256": __import__("hashlib").sha256(
                message.encode("utf-8")).hexdigest(),
            "signature": "sig",
        },
    }

    async def fetch_text(*_args):
        return 200, json.dumps(body)

    async def verify(_key, _signature, _message):
        return True

    async def hold_lease(_env, _row, _now, _latency, **fields):
        events.append(("hold", fields["refs_digest"]))
        return lease_held

    async def d1_run(_env, sql, *args):
        events.append(("run", " ".join(sql.split()), args))

    async def mark_failed(_env, _row, _now):
        events.append(("failed",))

    namespace = {
        "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: value == "mirror",
        "valid_node_pubkey": lambda value: value == "node-public-key",
        "https_routing": SimpleNamespace(
            normalize_base_url=lambda value: str(value or ""),
            operations_sha256=lambda operations: "ops-digest",
            repository_health_challenge=lambda *_args: message,
        ),
        "_claimed_node_signing_pubkeys": signing_pubkeys,
        "_https_mirror_cloudflare_dns_ok": dns_ok,
        "_https_mirror_fetch_text": fetch_text,
        "_https_mirror_mark_failed": mark_failed,
        "_https_mirror_hold_lease": hold_lease,
        "_https_mirror_refs_match": (
            lambda digest, accepted: digest in accepted),
        "HTTPS_MIRROR_REQUIRED_FORKMESH_OPERATIONS": frozenset({"tree"}),
        "ed25519_verify": verify,
        "d1_run": d1_run,
        "_b64url_encode": lambda _value: "nonce",
        "_random_bytes": lambda _size: b"nonce",
        "quote": quote,
        "json": json,
        "hashlib": __import__("hashlib"),
        "HTTPS_MIRROR_MANIFEST_MAX_BYTES": 16_384,
        "HTTPS_MIRROR_CONTROL_FETCH_TIMEOUT_SECONDS": 5,
        "HTTPS_MIRROR_HEALTH_TRANSIENT_STATUSES": {502, 503},
    }
    exec(_function_source("_https_mirror_health_one"), namespace)
    return namespace["_https_mirror_health_one"]


_ENDPOINT = {
    "node_bi": "mirror-bi",
    "node_name": "mirror",
    "base_url": "https://mirror.example",
    "public_key": "node-public-key",
}
_UNAVAILABLE = {
    "owner": "forkmesh", "repository": "forkmesh", "available": False,
    "integrity": "unavailable", "refsSha256": "e" * 64, "operations": [],
    "operationsSha256": "ops-digest",
}
_AHEAD = {
    "owner": "forkmesh", "repository": "forkmesh", "available": True,
    "integrity": "ok", "refsSha256": "b" * 64, "operations": ["tree"],
    "operationsSha256": "ops-digest",
}


@pytest.mark.parametrize("proof, published", [
    (_UNAVAILABLE, ""),
    (_AHEAD, "b" * 64),
])
def test_signed_mid_update_proof_does_not_revoke_a_live_lease(
        proof, published):
    events = []
    health_one = _signed_health_namespace(
        proof=proof, lease_held=True, events=events)

    active = asyncio.run(health_one(
        object(), dict(_ENDPOINT), frozenset({"a" * 64})))

    # Not renewed, so callers keep retrying; not revoked either.
    assert active is False
    assert events == [("hold", published)]


def test_signed_mid_update_proof_without_a_lease_records_inactive():
    events = []
    health_one = _signed_health_namespace(
        proof=_AHEAD, lease_held=False, events=events)

    active = asyncio.run(health_one(
        object(), dict(_ENDPOINT), frozenset({"a" * 64})))

    assert active is False
    assert events[0] == ("hold", "b" * 64)
    kind, sql, args = events[1]
    assert kind == "run" and "forkmesh_active=?" in sql
    # checked_at, latency, healthy, integrity, sig, verified_at, refs ...
    assert args[5] == 0 and args[6] == "b" * 64 and args[9] == 0


def test_admitted_proof_renews_without_consulting_the_lease():
    events = []
    health_one = _signed_health_namespace(
        proof=_AHEAD, lease_held=True, events=events)

    active = asyncio.run(health_one(
        object(), dict(_ENDPOINT), frozenset({"b" * 64})))

    assert active is True
    assert not any(event[0] == "hold" for event in events)
    assert events[0][2][5] == NOW


def test_degraded_node_is_revoked_even_with_a_live_lease():
    events = []
    health_one = _signed_health_namespace(
        proof=_AHEAD, lease_held=True, events=events, node_ok=False)

    active = asyncio.run(health_one(
        object(), dict(_ENDPOINT), frozenset({"a" * 64})))

    # The hold covers a node whose own health is green and nothing else.
    assert active is False
    assert not any(event[0] == "hold" for event in events)
    _, sql, args = events[0]
    assert "forkmesh_active=?" in sql
    assert args[2] == 0 and args[3] == "degraded" and args[5] == 0


def test_held_lease_never_takes_the_zero_write_renewal_path():
    namespace = {"HTTPS_MIRROR_ENDPOINT_FAST_RENEW_MS": 5 * 60_000}
    exec(_function_source("_https_mirror_endpoint_renewal_state"), namespace)
    classify = namespace["_https_mirror_endpoint_renewal_state"]
    registration = {"baseUrl": "https://m.example", "publicKey": "key"}
    existing = {
        "base_url": "https://m.example", "public_key": "key",
        "checked_at": NOW - 60_000, "forkmesh_verified_at": NOW - 60_000,
        "healthy": 1, "integrity": "ok", "abuse_blocked": 0,
        "forkmesh_active": 1,
    }
    assert classify(existing, registration, NOW) == "fresh"
    # Checked after it was last verified: the lease is being held through an
    # unadmitted digest and must be challenged for real.
    held = {**existing, "checked_at": NOW - 10_000}
    assert classify(held, registration, NOW) == "stale"


def test_git_hosting_stays_green_when_the_whole_fleet_is_challenged_mid_update():
    """The /status query itself, over a fleet challenged right after a merge."""
    status_source = (ENTRY.parent / "status_monitoring.py").read_text(
        encoding="utf-8")
    end = status_source.index('ok["git_hosting"] = int(')
    start = status_source.rindex(
        '"SELECT COUNT(*) AS n FROM mirror_https_endpoints "', 0, end)
    fragment = status_source[start:end]
    status_sql = "".join(
        ast.literal_eval(line.strip().rstrip(","))
        for line in fragment.splitlines() if line.strip().startswith('"'))
    assert "forkmesh_active=1" in status_sql

    db = _lease_db(active=True, verified_at=NOW - 4 * 60 * 1000)
    db.execute("ALTER TABLE mirror_https_endpoints ADD abuse_blocked INTEGER DEFAULT 0")
    for name in ("m36", "m37", "m38"):
        db.execute(
            "INSERT INTO mirror_https_endpoints SELECT ?,checked_at,latency_ms,"
            "healthy,integrity,health_sig,forkmesh_verified_at,"
            "forkmesh_refs_sha256,forkmesh_operations_sha256,"
            "forkmesh_operations_json,forkmesh_active,health_message,"
            "updated_at,0 FROM mirror_https_endpoints WHERE node_bi='mirror-bi'",
            (name,))
        db.execute("INSERT INTO nodes VALUES (?, 0)", (name,))
    namespace = _lease_namespace(db)
    # Two mirrors are caught between generations, two already serve the new,
    # not yet admitted digest.
    for node, digest in (
            ("mirror-bi", ""), ("m36", ""), ("m37", "b" * 64), ("m38", "b" * 64)):
        assert asyncio.run(namespace["_https_mirror_hold_lease"](
            object(), {"node_bi": node}, NOW, 42,
            signature="sig", message="message", refs_digest=digest,
            operations_digest="ops", operations=["tree"],
        )) is True

    def green(at):
        cutoff = at - FRESH_MS
        return db.execute(status_sql, (cutoff, cutoff)).fetchone()[0]

    assert green(NOW) == 4
    # Held, never renewed: the same leases end at the ordinary deadline.
    assert green(NOW + 6 * 60 * 1000 + 1) == 0
