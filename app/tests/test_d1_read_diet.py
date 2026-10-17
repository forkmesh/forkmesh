#!/usr/bin/env python3
"""D1 rows-read diet (2026-09-11).

The Cloudflare D1 dashboard showed the Worker reading ~1.4M rows/day on a
5M/day Free allowance, dominated by a handful of queries that ran on a timer
while their data was unchanged: the public catalog scan refilled on every
per-minute cron tick, the status recorder rescanned and rewrote its state
rows every minute, the network boards double-scanned online_hourly_nodes,
and COUNT(*) FROM repositories was recomputed hundreds of times a day. These
tests pin the change-driven replacements: catalog_rev-keyed memos, an exact
IN-list plus a Python change gate for the status recorder, one shared read
for the online boards, and the settled-minute cache behind /api/status.
"""

import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = SRC / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
STATUS_TEXT = (SRC / "status_monitoring.py").read_text(encoding="utf-8")
ACCOUNTS_TEXT = (SRC / "account_users.py").read_text(encoding="utf-8")
CRON_ACTIVITY_TEXT = (SRC / "cron_activity.py").read_text(encoding="utf-8")
CATALOG_ROUTES_TEXT = (SRC / "catalog_routes.py").read_text(encoding="utf-8")


def _load(source, names, extra=None):
    tree = ast.parse(source)
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {node.name for node in selected}
    assert found == set(names), sorted(set(names) - found)
    namespace = dict(extra or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), "<diet>", "exec"),
        namespace)
    return namespace


def _clock(value):
    return type("Clock", (), {"now": staticmethod(lambda: value)})


# --- public catalog memo -------------------------------------------------

def _catalog_env(rev_box, scans, rev_reads):
    async def d1_all(env, sql, *args):
        scans.append(sql)
        return [{"key_bi": "k1", "owner_bi": "o1", "data": "cipher:a",
                 "is_private": 0}]

    async def d1_first(env, sql, *args):
        assert "catalog_rev" in sql
        rev_reads.append(sql)
        return {"rev": rev_box["rev"]} if rev_box["rev"] is not None else None

    async def decrypt_row(env, blob):
        return {"payload": blob, "visibility": "public"}

    return _load(ENTRY_TEXT, ["_decrypted_public_catalog", "_catalog_rev_current"], {
        "_PUBLIC_CATALOG_MEMO": {
            "ts": 0, "rows": None, "refresh_ts": 0, "rev": None, "scan_ts": 0},
        "PUBLIC_CATALOG_MEMO_TTL_MS": 30_000,
        "PUBLIC_CATALOG_MEMO_MAX_AGE_MS": 600_000,
        "_CATALOG_REV_PROBE": {"ts": 0, "rev": None},
        "CATALOG_REV_PROBE_TTL_MS": 10_000,
        "_CATALOG_ROW_DECRYPT_MEMO": {},
        "CATALOG_ROW_DECRYPT_MEMO_MAX": 4096,
        "Date": _clock(0),
        "d1_all": d1_all,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
    })


def test_catalog_memo_renews_from_catalog_rev_instead_of_rescanning():
    rev = {"rev": 7}
    scans, rev_reads = [], []
    ns = _catalog_env(rev, scans, rev_reads)
    run = ns["_decrypted_public_catalog"]

    first = asyncio.run(run(object(), 0))
    assert [r["data"]["payload"] for r in first] == ["cipher:a"]
    assert first[0]["owner_bi"] == "o1"
    assert len(scans) == 1

    # The per-minute cron lapses the 30s TTL every tick: only the one-row
    # rev probe runs while the counter is unchanged.
    for tick in (60_000, 120_000, 180_000):
        rows = asyncio.run(run(object(), tick))
        assert rows is first
    assert len(scans) == 1
    assert len(rev_reads) == 4

    # A catalog mutation bumps the counter; the next lapse rescans.
    rev["rev"] = 8
    asyncio.run(run(object(), 240_000))
    assert len(scans) == 2

    # Even an unchanged rev rescans once the last scan is older than the max
    # age, so a write path that forgot to bump still converges.
    asyncio.run(run(object(), 240_000 + 600_000 + 1))
    assert len(scans) == 3


def test_catalog_memo_without_a_trusted_rev_keeps_the_old_ttl_rescan():
    rev = {"rev": None}
    scans, rev_reads = [], []
    ns = _catalog_env(rev, scans, rev_reads)
    run = ns["_decrypted_public_catalog"]
    asyncio.run(run(object(), 0))
    asyncio.run(run(object(), 31_000))
    asyncio.run(run(object(), 62_000))
    assert len(scans) == 3


def test_catalog_memo_rescans_when_the_rev_probe_itself_fails():
    scans = []

    async def d1_all(env, sql, *args):
        scans.append(sql)
        return []

    ns = _load(ENTRY_TEXT, ["_decrypted_public_catalog"], {
        "_PUBLIC_CATALOG_MEMO": {"ts": 0, "rows": None, "refresh_ts": 0},
        "PUBLIC_CATALOG_MEMO_TTL_MS": 30_000,
        "PUBLIC_CATALOG_MEMO_MAX_AGE_MS": 600_000,
        "_CATALOG_ROW_DECRYPT_MEMO": {},
        "CATALOG_ROW_DECRYPT_MEMO_MAX": 4096,
        "d1_all": d1_all,
    })
    # No _catalog_rev_current at all (the AST harness shape): still serves.
    asyncio.run(ns["_decrypted_public_catalog"](object(), 0))
    asyncio.run(ns["_decrypted_public_catalog"](object(), 31_000))
    assert len(scans) == 2


def test_local_catalog_purge_forgets_the_rev_probe_and_count_memo():
    ns = _load(ENTRY_TEXT, ["_https_mirror_public_context_cache_clear"], {
        "_HTTPS_MIRROR_PUBLIC_CONTEXT_MEMO": {("a", "b"): {}},
        "_PUBLIC_CATALOG_MEMO": {
            "ts": 5, "rows": [1], "refresh_ts": 5, "rev": 3, "scan_ts": 5},
        "_CATALOG_ROW_DECRYPT_MEMO": {"x": {}},
        "_CATALOG_REV_PROBE": {"ts": 5, "rev": 3},
        "_REPOSITORIES_COUNT_MEMO": {"ts": 5, "count": 9, "rev": 3},
    })
    ns["_https_mirror_public_context_cache_clear"]()
    # 2026-09-13: a routing-context clear (pin admission, lease renewal) no
    # longer throws the decrypted rows away — it only forgets the rev probe,
    # so the next read re-checks the one-row counter and rescans solely when
    # a mutation actually bumped it.
    assert ns["_PUBLIC_CATALOG_MEMO"]["rows"] == [1]
    assert ns["_PUBLIC_CATALOG_MEMO"]["rev"] == 3
    assert ns["_PUBLIC_CATALOG_MEMO"]["ts"] == 0
    assert ns["_CATALOG_REV_PROBE"] == {"ts": 0, "rev": None}
    assert ns["_REPOSITORIES_COUNT_MEMO"]["count"] == 9
    assert ns["_REPOSITORIES_COUNT_MEMO"]["ts"] == 0


def test_a_local_catalog_mutation_marks_the_memos_stale():
    ns = _load(ENTRY_TEXT, [
        "_https_mirror_public_context_cache_clear",
        "_catalog_memos_mark_stale",
    ], {
        "_HTTPS_MIRROR_PUBLIC_CONTEXT_MEMO": {("a", "b"): {}},
        "_PUBLIC_CATALOG_MEMO": {
            "ts": 5, "rows": [1], "refresh_ts": 5, "rev": 3, "scan_ts": 5},
        "_CATALOG_ROW_DECRYPT_MEMO": {"x": {}},
        "_CATALOG_REV_PROBE": {"ts": 5, "rev": 3},
        "_REPOSITORIES_COUNT_MEMO": {"ts": 5, "count": 9, "rev": 3},
    })
    ns["_catalog_memos_mark_stale"]()
    # Rows stay for single-flight staleness, but the rev they were read
    # under is gone so the next reader must rescan.
    assert ns["_PUBLIC_CATALOG_MEMO"]["rows"] == [1]
    assert ns["_PUBLIC_CATALOG_MEMO"]["rev"] is None
    assert ns["_CATALOG_REV_PROBE"] == {"ts": 0, "rev": None}
    assert ns["_REPOSITORIES_COUNT_MEMO"]["count"] is None
    assert ns["_CATALOG_ROW_DECRYPT_MEMO"] == {}
    assert ns["_HTTPS_MIRROR_PUBLIC_CONTEXT_MEMO"] == {}
    # Every mutation path reaches it: the catalog purge and the rev bump.
    purge = ENTRY_TEXT[
        ENTRY_TEXT.index("async def purge_catalog_related_caches("):
        ENTRY_TEXT.index("async def schedule_public_catalog_republish(")]
    assert "_catalog_memos_mark_stale()" in purge
    bump = CATALOG_ROUTES_TEXT[
        CATALOG_ROUTES_TEXT.index("async def _bump_catalog_rev("):
        CATALOG_ROUTES_TEXT.index("async def _catalog_etag(")]
    assert 'globals().get("_catalog_memos_mark_stale")' in bump


def test_catalog_publish_cap_check_uses_the_count_memo():
    publish = CATALOG_ROUTES_TEXT[
        CATALOG_ROUTES_TEXT.index("# Cap: keep only the most-recent"):]
    assert 'globals().get("_repositories_count")' in publish[:900]


def test_a_catalog_rescan_only_follows_a_rev_bump_after_a_context_clear():
    rev = {"rev": 7}
    scans, rev_reads = [], []
    ns = _catalog_env(rev, scans, rev_reads)
    ns.update(_load(ENTRY_TEXT, [
        "_https_mirror_public_context_cache_clear",
        "_catalog_memos_mark_stale",
    ], {
        "_HTTPS_MIRROR_PUBLIC_CONTEXT_MEMO": {},
        "_PUBLIC_CATALOG_MEMO": ns["_PUBLIC_CATALOG_MEMO"],
        "_CATALOG_ROW_DECRYPT_MEMO": ns["_CATALOG_ROW_DECRYPT_MEMO"],
        "_CATALOG_REV_PROBE": ns["_CATALOG_REV_PROBE"],
        "_REPOSITORIES_COUNT_MEMO": {"ts": 0, "count": None, "rev": None},
    }))
    run = ns["_decrypted_public_catalog"]
    asyncio.run(run(object(), 0))
    assert len(scans) == 1
    # Ten pin admissions in a row: ten probes, zero rescans.
    for tick in range(1, 11):
        ns["_https_mirror_public_context_cache_clear"]()
        asyncio.run(run(object(), tick * 40_000))
    assert len(scans) == 1
    assert len(rev_reads) == 11
    # A real mutation on this isolate rescans immediately...
    ns["_catalog_memos_mark_stale"]()
    asyncio.run(run(object(), 450_000))
    assert len(scans) == 2
    # ...and one on another isolate is picked up through the counter.
    rev["rev"] = 8
    ns["_https_mirror_public_context_cache_clear"]()
    asyncio.run(run(object(), 500_000))
    assert len(scans) == 3


# --- notification cap trim ---------------------------------------------------

def _notification_db():
    import sqlite3
    db = sqlite3.connect(":memory:")
    db.executescript("""
        CREATE TABLE notifications (
            dedupe_bi TEXT PRIMARY KEY, recipient_bi TEXT NOT NULL,
            ts INTEGER NOT NULL, read_at INTEGER NOT NULL DEFAULT 0,
            data TEXT NOT NULL, content_bi TEXT NOT NULL DEFAULT '');
        CREATE INDEX idx_notifications_recipient_ts
            ON notifications(recipient_bi, ts);
    """)
    return db


def _notification_writer(db, memo):
    runs = []

    async def d1_run(_env, sql, *args):
        runs.append(" ".join(sql.split()))
        db.execute(sql, args)
        db.commit()

    async def blind_index(_env, value):
        return "bi:" + value

    async def encrypt_row(_env, payload):
        return json.dumps(payload)

    async def noop(*_args, **_kwargs):
        return True

    ns = _load(ENTRY_TEXT, ["enqueue_notification", "_notification_trim_due"], {
        "json": json,
        "ensure_schema": noop,
        "clean_string": lambda value, size: str(value or "")[:size],
        "MAX_NODE_NAME": 40,
        "valid_node_name": lambda value: bool(value),
        "NOTIFICATION_KINDS": {"issue"},
        "notification_payload": lambda kind, title, **kw: {
            "kind": kind, "title": title, **kw},
        "blind_index": blind_index,
        "encrypt_row": encrypt_row,
        "d1_run": d1_run,
        "notify_account_event": noop,
        "_queue_instant_notification": noop,
        "MAX_NOTIFICATIONS_PER_RECIPIENT": 3,
        "NOTIFICATION_RETAIN_MS": 10_000_000,
        "_NOTIFICATION_TRIM_MEMO": memo,
        "NOTIFICATION_TRIM_INTERVAL_MS": 900_000,
        "NOTIFICATION_TRIM_MEMO_MAX": 512,
        "Date": _clock(1_000),
    })
    return ns["enqueue_notification"], runs


def test_notification_cap_trim_is_an_indexed_range_delete_that_keeps_the_newest():
    db = _notification_db()
    enqueue, runs = _notification_writer(db, None)  # no memo: trim every time
    for number in range(1, 6):
        assert asyncio.run(enqueue(
            object(), "alice", "issue", "t", ts=number,
            dedupe="d%d" % number))
    kept = [row[0] for row in db.execute(
        "SELECT ts FROM notifications WHERE recipient_bi='bi:alice' "
        "ORDER BY ts").fetchall()]
    assert kept == [3, 4, 5]
    trims = [sql for sql in runs if "LIMIT 1 OFFSET" in sql]
    assert len(trims) == 5
    assert "NOT IN" not in trims[0]
    assert "ts < COALESCE((" in trims[0]


def test_notification_cap_trim_runs_once_per_recipient_per_interval():
    db = _notification_db()
    memo = {}
    enqueue, runs = _notification_writer(db, memo)
    for number in range(1, 6):
        asyncio.run(enqueue(
            object(), "alice", "issue", "t", ts=number,
            dedupe="d%d" % number))
    asyncio.run(enqueue(object(), "bob", "issue", "t", ts=9, dedupe="b1"))
    trims = [sql for sql in runs if "LIMIT 1 OFFSET" in sql]
    # One trim for alice (the first insert), one for bob; the retention
    # DELETE (indexed ts range) still runs every time.
    assert len(trims) == 2
    assert len([sql for sql in runs if "AND ts < ?" in sql]) == 6
    assert memo == {"bi:alice": 1, "bi:bob": 9}
    # The cap is enforced again once the interval has passed.
    memo["bi:alice"] = 1 - 900_000
    asyncio.run(enqueue(object(), "alice", "issue", "t", ts=10, dedupe="d10"))
    assert len([sql for sql in runs if "LIMIT 1 OFFSET" in sql]) == 3
    kept = [row[0] for row in db.execute(
        "SELECT ts FROM notifications WHERE recipient_bi='bi:alice' "
        "ORDER BY ts").fetchall()]
    assert kept == [4, 5, 10]


# --- chat history --------------------------------------------------------------

def test_chat_replay_bounds_the_window_scan_to_the_replay_ceiling():
    recent = ENTRY_TEXT[
        ENTRY_TEXT.index("async def chat_history_recent("):
        ENTRY_TEXT.index("def chat_history_replay_body(")]
    assert ("FROM (SELECT body,ts,msg_id FROM chat_history "
            "WHERE room_key=? AND ts>=? AND acked_at=0 "
            "ORDER BY ts DESC,msg_id DESC LIMIT ?)"
            in recent.replace('"\n        "', ""))


def test_chat_prune_bounds_run_on_the_first_store_and_then_every_nth():
    ns = _load(ENTRY_TEXT, ["_chat_history_prune_due"], {
        "_CHAT_HISTORY_PRUNE_MEMO": {},
        "CHAT_HISTORY_PRUNE_EVERY_STORES": 4,
    })
    due = ns["_chat_history_prune_due"]
    assert [due("room") for _ in range(9)] == [
        True, False, False, False, True, False, False, False, True]
    assert due("other") is True
    # Harnesses without the memo keep the prune-on-every-store behaviour.
    assert _load(ENTRY_TEXT, ["_chat_history_prune_due"], {})[
        "_chat_history_prune_due"]("room") is True
    assert "\nCHAT_HISTORY_PRUNE_EVERY_STORES = 16\n" in ENTRY_TEXT


# --- users directory -------------------------------------------------------------

def test_users_directory_referrer_scan_is_memoized_per_isolate():
    scans = []

    async def d1_all(_env, sql, *args):
        scans.append(" ".join(sql.split())[:40])
        return [{"user_bi": "u1", "commits": 4, "referred_bi": "u1",
                 "referrer_name": "ref"}]

    memos = {
        "_CREDITED_REFERRERS_MEMO": {"ts": 0, "map": None},
        "CREDITED_REFERRERS_MEMO_TTL_MS": 300_000,
    }
    ns = _load(ACCOUNTS_TEXT, ["_credited_referrers_by_user"], {
        "d1_all": d1_all, "Date": _clock(1_000),
        "clean_string": lambda value, size: str(value or "")[:size],
        "MAX_NODE_NAME": 40, "valid_node_name": lambda value: bool(value),
        **memos,
    })
    ns.update(_load(ENTRY_TEXT, [
        "_contribution_commit_totals", "_users_directory_memos_clear"], {
        "d1_all": d1_all, "_contribution_tally": int, **memos,
    }))
    for _ in range(3):
        assert asyncio.run(ns["_credited_referrers_by_user"](object())) == {
            "u1": "ref"}
    assert len(scans) == 1
    ns["_users_directory_memos_clear"]()
    asyncio.run(ns["_credited_referrers_by_user"](object()))
    assert len(scans) == 2
    # Without the read guard (as in these AST harnesses) totals still compute;
    # test_d1_read_guard pins the shared copy they normally come from.
    assert asyncio.run(ns["_contribution_commit_totals"](object())) == {
        "u1": 4}
    directory = ACCOUNTS_TEXT[
        ACCOUNTS_TEXT.index("async def _account_users_directory("):]
    scan = directory.index("SELECT u.user_bi,")
    assert directory.index("async def scan():") < scan < directory.index(
        'shared(env, "users-directory", scan)')
    # Every roster invalidation also drops the memos.
    for text in (ENTRY_TEXT, ACCOUNTS_TEXT):
        assert text.count("await _users_directory_cache_forget()") \
            == text.count('globals().get("_users_directory_memos_clear")')
    assert "\nUSERS_DIRECTORY_TTL = 120\n" in ENTRY_TEXT


# --- state history pins: one slot per repository ----------------------------------

def test_state_history_pin_memo_keeps_one_slot_per_repository():
    history_reads = []

    async def d1_first(env, sql, *args):
        return {"ts": 100}

    async def d1_all(env, sql, *args):
        history_reads.append(args[0])
        return [{"state_hash": args[0][0] * 64}]

    ns = _load(ENTRY_TEXT, ["_https_mirror_state_history_pins"], {
        "_HTTPS_MIRROR_STATE_PIN_MEMO": {"entries": {}},
        "HTTPS_MIRROR_STATE_PIN_MEMO_MAX_AGE_MS": 600_000,
        "HTTPS_MIRROR_STATE_PIN_MEMO_MAX_KEYS": 64,
        "STATE_PIN_HISTORY": 100,
        "Date": _clock(1_000),
        "re": __import__("re"),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "d1_first": d1_first,
        "d1_all": d1_all,
    })
    pins = ns["_https_mirror_state_history_pins"]
    # The fleet alternates between two repositories: the single-slot memo
    # this replaced re-read the whole history on every switch.
    for _ in range(5):
        assert asyncio.run(pins(object(), "a")) == frozenset({"a" * 64})
        assert asyncio.run(pins(object(), "b")) == frozenset({"b" * 64})
    assert history_reads == ["a", "b"]


# --- mirrors projection runs under the ingress envelope -----------------------------

def test_repo_mirrors_projection_is_served_from_the_ingress_envelope():
    import re as _re
    from types import SimpleNamespace as _NS
    ns = _load(ENTRY_TEXT, ["_repository_projection_http_request"], {
        "re": _re, "method_name": lambda request: request.method,
        "JsURL": None,
    })
    check = ns["_repository_projection_http_request"]
    request = _NS(method="GET", url="https://app.forkmesh.com/")
    assert check(request, "/api/repo/forkmesh/forkmesh/mirrors")
    assert check(request, "/api/repo/forkmesh/forkmesh/mirrors/")
    assert check(
        request, "/api/repo/forkmesh/forkmesh/mirrors/mirror35/reachability")
    assert check(request, "/api/repo/forkmesh/forkmesh/issues")
    assert not check(request, "/api/repo/forkmesh/forkmesh/mirrors/x")
    assert not check(request, "/api/repo/forkmesh/forkmesh/tree/main")


# --- repository count memo -----------------------------------------------

def test_repository_count_is_reused_while_catalog_rev_is_unchanged():
    rev = {"rev": 1}
    counts, probes = [], []

    async def d1_first(env, sql, *args):
        if "catalog_rev" in sql:
            probes.append(sql)
            return {"rev": rev["rev"]}
        counts.append(sql)
        return {"n": 55}

    ns = _load(ENTRY_TEXT, ["_repositories_count", "_catalog_rev_current"], {
        "_REPOSITORIES_COUNT_MEMO": {"ts": 0, "count": None, "rev": None},
        "REPOSITORIES_COUNT_MEMO_MAX_AGE_MS": 600_000,
        "_CATALOG_REV_PROBE": {"ts": 0, "rev": None},
        "CATALOG_REV_PROBE_TTL_MS": 10_000,
        "Date": _clock(0),
        "d1_first": d1_first,
    })
    count = ns["_repositories_count"]
    assert asyncio.run(count(object(), 0)) == 55
    assert asyncio.run(count(object(), 20_000)) == 55
    assert asyncio.run(count(object(), 40_000)) == 55
    assert len(counts) == 1
    # Probes within CATALOG_REV_PROBE_TTL_MS share one read.
    assert asyncio.run(count(object(), 41_000)) == 55
    assert len(probes) == 3
    rev["rev"] = 2
    asyncio.run(count(object(), 60_000))
    assert len(counts) == 2


def test_network_stats_and_status_history_use_the_count_memo():
    stats = ENTRY_TEXT[
        ENTRY_TEXT.index("async def network_stats("):
        ENTRY_TEXT.index("\nasync def _network_payout_nodes(")]
    assert "await _repositories_count(env, now)" in stats
    assert "SELECT COUNT(*) AS n FROM repositories" not in stats
    history = STATUS_TEXT[STATUS_TEXT.index("async def status_history("):]
    assert "await _repositories_count(env, now)" in history
    assert "SELECT COUNT(*) AS n FROM repositories" not in history


# --- catalog_rev bumps ride the republish doorbell ------------------------

def test_catalog_republish_doorbell_bumps_catalog_rev_first():
    calls = []

    async def bump(env):
        calls.append("bump")

    async def paths(env, *args):
        calls.append("paths")
        raise RuntimeError("no snapshot paths")

    ns = _load(ENTRY_TEXT, ["schedule_public_catalog_republish"], {
        "_bump_catalog_rev": bump,
        "_public_catalog_snapshot_paths": paths,
    })
    assert asyncio.run(
        ns["schedule_public_catalog_republish"](object(), "a", "b")) is False
    assert calls == ["bump", "paths"]
    assert '_bump_catalog_rev = _catalog_routes.export("_bump_catalog_rev")' \
        in ENTRY_TEXT


# --- shared online_hourly_nodes read ----------------------------------------

def test_online_hourly_rows_are_read_once_for_both_network_boards():
    reads = []

    async def d1_all(env, sql, *args):
        reads.append(args)
        return [
            {"node_key": "n1", "label": "one", "hour_ts": 1_000, "node_minutes": 5},
            {"node_key": "n1", "label": "one", "hour_ts": 5_000, "node_minutes": 7},
        ]

    ns = _load(ENTRY_TEXT, ["_online_hourly_node_rows"], {
        "_ONLINE_HOURLY_NODES_MEMO": {"ts": 0, "start": 0, "rows": None},
        "ONLINE_HOURLY_NODES_MEMO_TTL_MS": 120_000,
        "d1_all": d1_all,
    })
    rows = ns["_online_hourly_node_rows"]
    wide = asyncio.run(rows(object(), 0, 10_000))
    narrow = asyncio.run(rows(object(), 4_000, 10_500))
    assert len(wide) == 2 and len(narrow) == 1
    assert reads == [(0,)]
    # A wider window than the memo holds must read again.
    asyncio.run(rows(object(), -1, 11_000))
    assert len(reads) == 2
    leaderboards = ENTRY_TEXT[
        ENTRY_TEXT.index("async def network_leaderboards("):
        ENTRY_TEXT.index("\n# --- Referral program")]
    history = ENTRY_TEXT[
        ENTRY_TEXT.index("async def online_history("):
        ENTRY_TEXT.index("\nasync def network_overview(")]
    assert "_online_hourly_node_rows(env, window_start, now)" in leaderboards
    assert "_online_hourly_node_rows(env, start, now)" in history
    assert "FROM online_hourly_nodes" not in leaderboards
    assert "FROM online_hourly_nodes" not in history
    # The leaderboards reuse the decrypted catalog memo, not their own scan.
    assert "await _decrypted_public_catalog(env, now)" in leaderboards
    assert "FROM repositories" not in leaderboards
    assert "cache_seconds=NETWORK_LEADERBOARDS_TTL" in leaderboards
    assert "cache_seconds=ONLINE_HISTORY_TTL" in history


def test_network_edge_ttls_match_their_client_cadence():
    assert "\nNETWORK_STATS_TTL = 60\n" in ENTRY_TEXT
    assert "\nNETWORK_LEADERBOARDS_TTL = 300\n" in ENTRY_TEXT
    assert "\nONLINE_HISTORY_TTL = 300\n" in ENTRY_TEXT


# --- mirror-health: state history pins ------------------------------------

def test_state_history_pins_are_reused_while_the_newest_ts_is_unchanged():
    newest = {"ts": 100}
    history_reads, probes = [], []

    async def d1_first(env, sql, *args):
        probes.append(args)
        return {"ts": newest["ts"]}

    async def d1_all(env, sql, *args):
        history_reads.append(args)
        return [{"state_hash": "a" * 64}, {"state_hash": "bad"}]

    ns = _load(ENTRY_TEXT, ["_https_mirror_state_history_pins"], {
        "_HTTPS_MIRROR_STATE_PIN_MEMO": {
            "key_bi": None, "max_ts": None, "ts": 0, "pins": frozenset()},
        "HTTPS_MIRROR_STATE_PIN_MEMO_MAX_AGE_MS": 600_000,
        "STATE_PIN_HISTORY": 100,
        "Date": _clock(1_000),
        "re": __import__("re"),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "d1_first": d1_first,
        "d1_all": d1_all,
    })
    pins = ns["_https_mirror_state_history_pins"]
    assert asyncio.run(pins(object(), "repo-bi")) == frozenset({"a" * 64})
    assert asyncio.run(pins(object(), "repo-bi")) == frozenset({"a" * 64})
    assert history_reads == [("repo-bi", 100)]
    assert len(probes) == 2
    newest["ts"] = 101
    asyncio.run(pins(object(), "repo-bi"))
    assert len(history_reads) == 2
    accepted = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _https_mirror_accepted_forkmesh_refs("):
        ENTRY_TEXT.index("\ndef _https_mirror_refs_match(")]
    assert "await _https_mirror_state_history_pins(env, key_bi)" in accepted
    assert "FROM repo_state_history" not in accepted


# --- status recorder ---------------------------------------------------------

SYSTEMS = [("edge_api", "Edge API"), ("website", "Website")]


def _status_recorder(monitors):
    reads, writes, pings = [], [], []

    async def d1_all(_env, sql, *args):
        assert "FROM repository_monitor_state" in sql
        reads.append((sql, args))
        return [dict(row) for row in monitors.values()]

    async def d1_run(_env, sql, *args):
        assert sql.startswith("INSERT INTO repository_monitor_state")
        writes.append(json.loads(args[0]))
        for row in json.loads(args[0]):
            monitors[row["monitorId"]] = {
                "monitor_id": row["monitorId"], "is_up": row["isUp"],
                "changed_at": row["changedAt"],
                "outage_started_at": row["outageStartedAt"],
                "checked_at": row["checkedAt"], "reason": row["reason"],
                "notified_state": row["notifiedState"],
                "pinged_state": row["pingedState"],
            }

    async def enqueue(_env, alerts, _now):
        pings.append([alert["system"] for alert in alerts])
        return True

    async def sent(_env, alerts):
        for alert in alerts:
            monitors["status:" + alert["system"]]["pinged_state"] = alert["state"]

    async def settings(_env, _owner, _repo):
        return {}

    ns = _load(STATUS_TEXT, [
        "_record_status_monitor_transitions", "_status_monitor_prior_rows",
        "_status_monitor_row_changed", "_status_monitor_rows_remember",
    ], {
        "json": json,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "clean_string": lambda value, size: str(value or "")[:size],
        "STATUS_SYSTEMS": SYSTEMS,
        "STATUS_MIRROR_PREFIX": "mirror:",
        "STATUS_CORE_PREFIX": "core:",
        "STATUS_CORE_MAX": 32,
        "STATUS_RUNNER_PREFIX": "runner:",
        "STATUS_RUNNER_MAX": 32,
        "STATUS_RUNNER_SEEN_FRESH_MS": 180000,
        "STATUS_ALERT_CONTINUAL_BATCH": 16,
        "STATUS_SAMPLE_WINDOW_MS": 60_000,
        "STATUS_ALERT_CONTINUAL_INTERVAL_MS": 30 * 60_000,
        "STATUS_ALERT_DELIVERY_BATCH": 8,
        "FLAGSHIP_MONITOR_ID": "forkmesh/forkmesh",
        "_repo_alert_settings_get": settings,
        "_status_monitor_alert_setting": lambda *_a: False,
        "_enqueue_operational_alert_pings": enqueue,
        "_record_operational_alert_pings_sent": sent,
        "_STATUS_MONITOR_STATE_MEMO": {"ts": 0, "ids": (), "rows": None},
        "STATUS_MONITOR_STATE_MEMO_TTL_MS": 600_000,
        "STATUS_MONITOR_STATE_IN_LIST_MAX": 90,
    })
    return ns, reads, writes, pings


def _record(ns, now, down=()):
    ok = {system: system not in down for system, _ in SYSTEMS}
    reason = {system: "boom" for system in down}
    return asyncio.run(ns["_record_status_monitor_transitions"](
        SimpleNamespace(), ok, reason, now, SYSTEMS, delivery_channel="ping"))


def _green_rows(changed_at):
    return {
        "status:" + system: {
            "monitor_id": "status:" + system, "is_up": 1,
            "changed_at": changed_at, "outage_started_at": 0,
            "checked_at": changed_at, "reason": "",
            "notified_state": "up", "pinged_state": "up",
        } for system, _ in SYSTEMS
    }


def test_all_green_minutes_read_the_exact_roster_once_and_never_write():
    ns, reads, writes, pings = _status_recorder(_green_rows(60_000))
    assert _record(ns, 120_000) is False
    assert len(reads) == 1
    sql, args = reads[0]
    assert "WHERE monitor_id IN (?,?)" in sql
    assert args == ("status:edge_api", "status:website")
    assert "LIKE" not in sql
    assert writes == []
    # The next minutes are answered from the memo: no read, no write.
    for now in (180_000, 240_000, 300_000):
        assert _record(ns, now) is False
    assert len(reads) == 1 and writes == [] and pings == []


def test_a_transition_reverifies_against_d1_and_writes_only_changed_rows():
    ns, reads, writes, pings = _status_recorder(_green_rows(60_000))
    _record(ns, 120_000)
    assert len(reads) == 1
    assert _record(ns, 180_000, down=("website",)) is True
    # Memo said "up"; the recorder re-read before acting, then wrote only the
    # row that changed and announced the outage once.
    assert len(reads) == 2
    assert [[row["monitorId"] for row in batch] for batch in writes] == [
        ["status:website"]]
    assert pings == [["website"]]
    # A delivery moved pinged_state, so the memo was forgotten: the next
    # steady minute reads once more, then settles again.
    assert _record(ns, 240_000, down=("website",)) is False
    assert len(reads) == 3
    assert _record(ns, 300_000, down=("website",)) is False
    assert len(reads) == 3
    assert len(writes) == 1
    # Recovery: verified against D1, one changed row, one ping.
    assert _record(ns, 360_000) is True
    assert len(reads) == 4
    assert [row["monitorId"] for row in writes[-1]] == ["status:website"]
    assert pings == [["website"], ["website"]]


def test_prior_row_read_falls_back_to_the_prefix_scan_for_huge_rosters():
    reads = []

    async def d1_all(_env, sql, *args):
        reads.append((sql, args))
        return []

    ns = _load(STATUS_TEXT, ["_status_monitor_prior_rows"], {
        "d1_all": d1_all,
        "STATUS_MONITOR_STATE_IN_LIST_MAX": 90,
        "STATUS_MONITOR_STATE_MEMO_TTL_MS": 600_000,
    })
    ids = ["status:m%d" % i for i in range(120)]
    prior, from_memo = asyncio.run(
        ns["_status_monitor_prior_rows"](object(), ids, 0))
    assert prior == {} and from_memo is False
    assert "WHERE monitor_id LIKE 'status:%'" in reads[0][0]
    assert reads[0][1] == ()


# --- status minute reads ------------------------------------------------------

def _minute_records_env(reads, cache=None, memo=None, now=0):
    async def d1_all(_env, sql, *args):
        reads.append((sql, args))
        if "system_status_minute_batch" in sql:
            start, end = args
            return [
                {"minute_ts": minute, "data": json.dumps({
                    "schemaVersion": 1,
                    "systems": {"website": {"ok": True, "reason": None}},
                })}
                for minute in range(start, end + 1, 60_000)
            ]
        return []

    extra = {
        "d1_all": d1_all,
        "STATUS_SAMPLE_WINDOW_MS": 60_000,
        "STATUS_MINUTE_RECORDS_CACHE_MAX": 180,
        "STATUS_SYSTEMS": [("website", "Website")],
        "STATUS_MIRROR_PREFIX": "mirror:",
        "STATUS_CORE_PREFIX": "core:",
        "STATUS_CORE_MAX": 32,
        "STATUS_RUNNER_PREFIX": "runner:",
        "STATUS_RUNNER_MAX": 32,
        "STATUS_RUNNER_SEEN_FRESH_MS": 180000,
        "STATUS_ALERT_CONTINUAL_BATCH": 16,
        "STATUS_RETIRED_MIRRORS": frozenset(),
        "STATUS_SYSTEM_SUCCESSORS": (),
        "STATUS_MIRROR_MAX": 50,
        "STATUS_BATCH_SCHEMA_VERSION": 1,
        "STATUS_BATCH_MAX_BYTES": {"minute": 65536, "hour": 65536, "day": 65536},
        "json": json,
        "re": __import__("re"),
        "clean_string": lambda value, size: str(value or "")[:size],
        "Date": _clock(now),
    }
    if cache is not None:
        extra["_STATUS_MINUTE_RECORDS_CACHE"] = cache
    if memo is not None:
        extra["_STATUS_MINUTE_BATCH_MEMO"] = memo
    return _load(STATUS_TEXT, [
        "_status_minute_records", "_status_batch_decode",
        "_status_batch_system_id", "_status_batch_retired_mirror",
    ], extra)


def test_the_minute_just_written_is_served_from_the_sampler_memo():
    reads = []
    ns = _minute_records_env(reads, memo={
        "minute_ts": 600_000,
        "systems": {"website": {"ok": False, "reason": "boom"}},
    })
    rows = asyncio.run(ns["_status_minute_records"](object(), 600_000, 600_000))
    assert rows == [{"minute_ts": 600_000, "system": "website", "ok": 0,
                     "reason": "boom"}]
    assert reads == []
    # A different minute, or a window, still reads.
    asyncio.run(ns["_status_minute_records"](object(), 540_000, 540_000))
    assert len(reads) == 2


def test_settled_minutes_are_cached_so_the_status_window_reads_only_new_ones():
    reads = []
    cache = {}
    minute = 60_000
    now = 100 * minute
    ns = _minute_records_env(reads, cache=cache, now=now)
    first = asyncio.run(ns["_status_minute_records"](
        object(), 30 * minute, 89 * minute))
    assert len(first) == 60
    assert len([r for r in reads if "system_status_minute_batch" in r[0]]) == 1
    # Everything older than two windows before "now" settled into the cache;
    # the live tail (98/99) would not have, but this window ends at 89.
    assert set(cache) == {m * minute for m in range(30, 90)}
    reads.clear()
    second = asyncio.run(ns["_status_minute_records"](
        object(), 31 * minute, 91 * minute))
    assert len(second) == 61
    batch_reads = [r for r in reads if "system_status_minute_batch" in r[0]]
    assert batch_reads[0][1] == (90 * minute, 91 * minute)


def test_the_sampler_write_feeds_the_minute_memo():
    memo = {"minute_ts": None, "systems": None}
    writes = []

    async def d1_all(_env, _sql, *args):
        return []

    async def d1_run(_env, sql, *args):
        writes.append(args)

    ns = _load(STATUS_TEXT, [
        "_write_status_minute_batch", "_status_batch_encode",
        "_status_batch_decode", "_status_batch_system_id",
        "_status_batch_retired_mirror",
    ], {
        "d1_all": d1_all, "d1_run": d1_run, "json": json,
        "re": __import__("re"),
        "clean_string": lambda value, size: str(value or "")[:size],
        "STATUS_SYSTEMS": [("website", "Website")],
        "STATUS_MIRROR_PREFIX": "mirror:",
        "STATUS_CORE_PREFIX": "core:",
        "STATUS_CORE_MAX": 32,
        "STATUS_RUNNER_PREFIX": "runner:",
        "STATUS_RUNNER_MAX": 32,
        "STATUS_RUNNER_SEEN_FRESH_MS": 180000,
        "STATUS_ALERT_CONTINUAL_BATCH": 16,
        "STATUS_RETIRED_MIRRORS": frozenset(),
        "STATUS_SYSTEM_SUCCESSORS": (),
        "STATUS_MIRROR_MAX": 50,
        "STATUS_BATCH_SCHEMA_VERSION": 1,
        "STATUS_BATCH_MAX_BYTES": {"minute": 65536, "hour": 65536, "day": 65536},
        "_STATUS_MINUTE_BATCH_MEMO": memo,
    })
    asyncio.run(ns["_write_status_minute_batch"](
        object(), 120_000, {"website": {"ok": True, "reason": None}}))
    assert len(writes) == 1
    assert memo == {"minute_ts": 120_000,
                    "systems": {"website": {"ok": True, "reason": None}}}


def test_synthetic_probe_results_are_remembered_per_isolate():
    reads, probes, writes = [], [], []

    async def d1_first(_env, sql, *args):
        reads.append(args)
        return None

    async def d1_run(_env, sql, *args):
        writes.append(args)

    async def probe(_env):
        probes.append(1)
        return True, ""

    ns = _load(STATUS_TEXT, [
            "_cached_synthetic_status_probe",
            "_status_probe_failure_confirmed", "_status_probe_verdict"], {
        "d1_first": d1_first, "d1_run": d1_run,
        "clean_string": lambda value, size: str(value or "")[:size],
        "STATUS_SYNTHETIC_HEALTHY_INTERVAL_MS": 600_000,
        "STATUS_SYNTHETIC_FAILED_RETRY_MS": 60_000,
        "_STATUS_SYNTHETIC_PROBE_MEMO": {},
    })
    run = ns["_cached_synthetic_status_probe"]
    assert asyncio.run(run(object(), "m", 0, probe)) == (True, "")
    assert asyncio.run(run(object(), "m", 60_000, probe)) == (True, "")
    assert asyncio.run(run(object(), "m", 599_000, probe)) == (True, "")
    assert len(reads) == 1 and len(probes) == 1 and len(writes) == 1
    asyncio.run(run(object(), "m", 600_000, probe))
    assert len(reads) == 2 and len(probes) == 2


# --- admin directory ---------------------------------------------------------

def test_admin_signup_fields_are_memoized_per_isolate():
    scans = []

    async def d1_all(_env, sql, *args):
        scans.append(sql)
        return []

    async def referred(_env):
        return {}

    ns = _load(ACCOUNTS_TEXT, ["_account_admin_signup_fields"], {
        "d1_all": d1_all,
        "_credited_referrals_by_referrer": referred,
        "_ADMIN_SIGNUP_FIELDS_MEMO": {"ts": 0, "fields": None},
        "ADMIN_SIGNUP_FIELDS_MEMO_TTL_MS": 60_000,
        "Date": _clock(1_000),
    })
    assert asyncio.run(ns["_account_admin_signup_fields"](object())) == {}
    assert len(scans) == 4
    assert asyncio.run(ns["_account_admin_signup_fields"](object())) == {}
    assert len(scans) == 4


def test_session_cookie_alone_never_opens_the_admin_directory_projection():
    authed = []

    async def _authed_account_name(_env, _request):
        authed.append(1)
        return "admin"

    async def _is_admin(_env, actor):
        return actor == "admin"

    from urllib.parse import parse_qs, urlparse
    ns = _load(ACCOUNTS_TEXT, ["_account_users_admin_actor"], {
        "parse_qs": parse_qs, "urlparse": urlparse,
        "clean_string": lambda value, size: str(value or "")[:size],
        "MAX_NODE_NAME": 40,
        "ACCOUNT_USERS_ADMIN_PROOF": "proof",
        "_authed_account_name": _authed_account_name,
        "_is_admin": _is_admin,
        "_ts_ok": lambda _ts: False,
        "_verify_owner_signature": None,
    })
    actor = ns["_account_users_admin_actor"]

    def request(headers):
        return SimpleNamespace(
            headers=headers, url="https://forkmesh.com/api/accounts/users")

    # An admin's chat/dashboard tab: cookie only -> public roster.
    assert asyncio.run(actor(object(), request(
        {"cookie": "forkmesh.session=abc"}))) == ""
    assert authed == []
    # The desktop Users page: bearer session -> admin projection.
    assert asyncio.run(actor(object(), request(
        {"authorization": "Bearer abc"}))) == "admin"
    assert authed == [1]


# --- cron cadence --------------------------------------------------------------

def test_feedback_email_scan_runs_four_times_a_day():
    ns = _load(ENTRY_TEXT, ["_cron_due_phases"], {
        "PUBLIC_SNAPSHOT_FREQUENT_CADENCE_MINUTES": 3,
        "PUBLIC_SNAPSHOT_SLOW_CADENCE_MINUTES": 5,
        "FLAGSHIP_CHECK_INTERVAL_MINUTES": 360,
        "WORLD_SATELLITE_REFRESH_CRON_MINUTES": 180,
        "WORLD_SATELLITE_REFRESH_CRON_OFFSET": 11,
    })
    due = [minute for minute in range(1440)
           if "feedback-email" in ns["_cron_due_phases"](minute)]
    assert due == [22, 382, 742, 1102]
    assert '("feedback-email", 360, (22,), "notifications",' in CRON_ACTIVITY_TEXT


# --- cross-isolate catalog snapshot (2026-09-13) --------------------------
#
# Isolate churn defeated the per-isolate memo: the schema fingerprint fast
# path ran ~570 times in 2.5 hours, and every cold isolate's first catalog
# read rescanned and re-decrypted the table. The scan result is now published
# as one encrypted public_catalog_snapshot row keyed by catalog_rev; a cold
# isolate hydrates from that single row and scans only when the rev moved or
# the snapshot aged past the memo's max age.

SNAPSHOT_NAMES = [
    "_decrypted_public_catalog", "_catalog_rev_current",
    "_repositories_count", "_public_catalog_snapshot_read",
    "_public_catalog_snapshot_store",
]


def _snapshot_world():
    """One fake D1 shared by every simulated isolate."""
    return {
        "rev": 7,
        "snapshot": None,
        "repositories": [
            {"key_bi": "k1", "owner_bi": "o1", "data": "cipher:a",
             "is_private": 0},
            {"key_bi": "k2", "owner_bi": "o2", "data": "cipher:b",
             "is_private": 0},
        ],
        "scans": [], "counts": [], "snapshot_reads": [], "stores": [],
    }


def _snapshot_isolate(world, now_box):
    """A fresh isolate: empty memos, same database."""
    async def d1_all(env, sql, *args):
        assert "FROM repositories" in sql
        world["scans"].append(sql)
        return list(world["repositories"])

    async def d1_first(env, sql, *args):
        if "catalog_rev" in sql:
            return {"rev": world["rev"]}
        if "public_catalog_snapshot" in sql:
            world["snapshot_reads"].append(sql)
            return dict(world["snapshot"]) if world["snapshot"] else None
        assert "COUNT(*)" in sql
        world["counts"].append(sql)
        return {"n": 55}

    async def d1_run(env, sql, *args):
        assert "public_catalog_snapshot" in sql
        world["stores"].append(args)
        rev, count, ts, blob, max_age = args
        prior = world["snapshot"]
        if prior and not (rev >= prior["rev"] or ts - prior["ts"] >= max_age):
            return None
        world["snapshot"] = {
            "rev": rev, "repo_count": count, "ts": ts, "data": blob}
        return None

    async def encrypt_row(env, obj):
        return "snap:" + json.dumps(obj, sort_keys=True)

    async def decrypt_row(env, blob):
        if blob.startswith("snap:"):
            return json.loads(blob[5:])
        return {"payload": blob, "visibility": "public"}

    clock = type("Clock", (), {
        "now": staticmethod(lambda: now_box["now"])})
    return _load(ENTRY_TEXT, SNAPSHOT_NAMES, {
        "_PUBLIC_CATALOG_MEMO": {
            "ts": 0, "rows": None, "refresh_ts": 0, "rev": None, "scan_ts": 0},
        "PUBLIC_CATALOG_MEMO_TTL_MS": 30_000,
        "PUBLIC_CATALOG_MEMO_MAX_AGE_MS": 600_000,
        "PUBLIC_CATALOG_SNAPSHOT_MAX_BYTES": 1_000_000,
        "_CATALOG_REV_PROBE": {"ts": 0, "rev": None},
        "CATALOG_REV_PROBE_TTL_MS": 10_000,
        "_REPOSITORIES_COUNT_MEMO": {"ts": 0, "count": None, "rev": None},
        "REPOSITORIES_COUNT_MEMO_MAX_AGE_MS": 600_000,
        "_CATALOG_ROW_DECRYPT_MEMO": {},
        "CATALOG_ROW_DECRYPT_MEMO_MAX": 4096,
        "Date": clock,
        "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
        "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
    })


def test_a_cold_isolate_hydrates_the_catalog_from_the_shared_snapshot():
    world = _snapshot_world()
    now = {"now": 1_000}

    # First isolate anywhere: no snapshot yet, so it scans once, counts once
    # and publishes both for everyone else.
    first = _snapshot_isolate(world, now)
    rows = asyncio.run(first["_decrypted_public_catalog"](object(), 1_000))
    assert [r["data"]["payload"] for r in rows] == ["cipher:a", "cipher:b"]
    assert len(world["scans"]) == 1
    assert len(world["counts"]) == 1
    assert world["snapshot"]["rev"] == 7
    assert world["snapshot"]["repo_count"] == 55
    assert world["snapshot"]["ts"] == 1_000

    # Every later cold isolate reads the rev and the one snapshot row —
    # never the repositories table — and comes up with identical rows and
    # a warm count memo.
    for start in (5_000, 90_000, 400_000):
        now["now"] = start
        cold = _snapshot_isolate(world, now)
        hydrated = asyncio.run(cold["_decrypted_public_catalog"](object(), start))
        assert hydrated == rows
        assert cold["_PUBLIC_CATALOG_MEMO"]["rev"] == 7
        assert cold["_PUBLIC_CATALOG_MEMO"]["scan_ts"] == 1_000
        assert asyncio.run(cold["_repositories_count"](object(), start)) == 55
    assert len(world["scans"]) == 1
    assert len(world["counts"]) == 1
    assert len(world["stores"]) == 1

    # A catalog mutation bumps the rev: the next reader scans and republishes
    # under the new rev, and the isolates after it hydrate again.
    world["rev"] = 8
    now["now"] = 420_000
    scanner = _snapshot_isolate(world, now)
    asyncio.run(scanner["_decrypted_public_catalog"](object(), 420_000))
    assert len(world["scans"]) == 2
    assert world["snapshot"]["rev"] == 8
    now["now"] = 425_000
    follower = _snapshot_isolate(world, now)
    asyncio.run(follower["_decrypted_public_catalog"](object(), 425_000))
    assert len(world["scans"]) == 2

    # Past the max age the snapshot is not trusted even under the same rev,
    # so a write path that forgot to bump still converges: one scan, then a
    # fresh snapshot for the fleet.
    now["now"] = 420_000 + 600_000 + 1
    late = _snapshot_isolate(world, now)
    asyncio.run(late["_decrypted_public_catalog"](object(), now["now"]))
    assert len(world["scans"]) == 3
    assert world["snapshot"]["ts"] == now["now"]
    now["now"] += 1_000
    after = _snapshot_isolate(world, now)
    asyncio.run(after["_decrypted_public_catalog"](object(), now["now"]))
    assert len(world["scans"]) == 3


def test_a_cold_isolate_counts_repositories_from_the_snapshot_row():
    world = _snapshot_world()
    now = {"now": 1_000}
    world["snapshot"] = {"rev": 7, "repo_count": 61, "ts": 500, "data": "x"}
    cold = _snapshot_isolate(world, now)
    assert asyncio.run(cold["_repositories_count"](object(), 1_000)) == 61
    assert world["counts"] == []
    # Count-only readers must not select the payload column.
    assert world["snapshot_reads"] and "data" not in world["snapshot_reads"][-1]
    # A snapshot under another rev is ignored and the table is counted.
    world["rev"] = 9
    other = _snapshot_isolate(world, now)
    assert asyncio.run(other["_repositories_count"](object(), 1_000)) == 55
    assert len(world["counts"]) == 1


def test_a_slow_scan_never_overwrites_a_newer_snapshot():
    world = _snapshot_world()
    now = {"now": 1_000}
    world["snapshot"] = {"rev": 9, "repo_count": 1, "ts": 900, "data": "keep"}
    slow = _snapshot_isolate(world, now)
    stored = asyncio.run(slow["_public_catalog_snapshot_store"](
        object(), 8, [{"key_bi": "k"}], 2, 1_000))
    assert stored is True  # the upsert itself ran; the WHERE guard decided
    assert world["snapshot"]["rev"] == 9 and world["snapshot"]["data"] == "keep"
    sql = ENTRY_TEXT[ENTRY_TEXT.index("async def _public_catalog_snapshot_store"):]
    sql = sql[:sql.index("\n\n\n")]
    assert "WHERE excluded.rev >= public_catalog_snapshot.rev" in sql
    assert "OR excluded.ts - public_catalog_snapshot.ts >= ?" in sql


def test_an_oversized_snapshot_is_skipped_rather_than_written():
    world = _snapshot_world()
    now = {"now": 1_000}
    ns = _snapshot_isolate(world, now)
    ns["PUBLIC_CATALOG_SNAPSHOT_MAX_BYTES"] = 40
    stored = asyncio.run(ns["_public_catalog_snapshot_store"](
        object(), 7, [{"key_bi": "k", "data": {"name": "x" * 100}}], 3, 1_000))
    assert stored is False
    assert world["stores"] == []
    # The reader treats a snapshot that fails to decrypt as absent.
    world["snapshot"] = {"rev": 7, "repo_count": 3, "ts": 1_000, "data": "snap:[]"}
    assert asyncio.run(ns["_public_catalog_snapshot_read"](
        object(), 7, 1_000, True)) is None


def test_admin_stats_and_the_schema_share_the_snapshot_contract():
    stats = ENTRY_TEXT[ENTRY_TEXT.index("async def admin_stats(env)"):]
    stats = stats[:stats.index("\n\n\n")]
    assert "await _repositories_count(env)" in stats
    assert "COUNT(*) AS n FROM repositories" not in stats
    schema_text = (SRC / "schema.py").read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS public_catalog_snapshot" in schema_text
    # data stays the last column so count-only reads skip its overflow pages.
    table = schema_text[schema_text.index("public_catalog_snapshot ("):]
    table = table[:table.index(")\"\"\"")]
    assert table.rstrip().endswith("data TEXT NOT NULL DEFAULT ''")
