"""A level=none miss is re-asked at level=weak: round-robin DNS sends one
client's consecutive requests to different nodes, so a row written a moment
ago through node A can still be missing from node B's replica."""

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from forkmesh_host import d1_rqlite  # noqa: E402


def _db(confirm=True):
    db = d1_rqlite.make_d1({"url": "http://127.0.0.1:1",
                            "confirm_misses": confirm})
    calls = []

    async def fake_post(payload, transaction=False, level=None):
        calls.append(level)
        if level == "none":  # the follower's stale replica: no row yet
            return {"results": [{"types": {"n": "text"}}]}
        return {"results": [{"types": {"n": "text"}, "rows": [{"n": "fresh"}]}]}

    db._post = fake_post
    return db, calls


def test_miss_is_confirmed_with_the_leader():
    db, calls = _db()
    row = asyncio.run(db.prepare("SELECT n FROM t WHERE k=?").bind("x").first())
    assert row == {"n": "fresh"}
    assert calls == ["none", "weak"]


def test_hit_stays_local_and_writes_are_untouched():
    db, calls = _db()

    async def hit(payload, transaction=False, level=None):
        calls.append(level)
        return {"results": [{"types": {"n": "text"}, "rows": [{"n": "local"}]}]}

    db._post = hit
    row = asyncio.run(db.prepare("SELECT n FROM t").first())
    assert row == {"n": "local"} and calls == ["none"]


def test_confirmation_can_be_turned_off():
    db, calls = _db(confirm="0")
    assert asyncio.run(db.prepare("SELECT n FROM t").first()) is None
    assert calls == ["none"]


def test_confirmations_are_budgeted_then_answered_locally(capsys):
    """M8: past the per-process token bucket, a miss keeps the local answer
    (no leader read), is counted, and is logged at most once a minute."""
    db = d1_rqlite.make_d1({"url": "http://127.0.0.1:1",
                            "RQLITE_CONFIRM_RATE": "0",
                            "RQLITE_CONFIRM_BURST": "3"})
    calls = []

    async def fake_post(payload, transaction=False, level=None):
        calls.append(level)
        if level == "none":
            return {"results": [{"types": {"n": "text"}}]}
        return {"results": [{"types": {"n": "text"}, "rows": [{"n": "fresh"}]}]}

    db._post = fake_post

    async def five():
        return [await db.prepare("SELECT n FROM t").first() for _ in range(5)]

    rows = asyncio.run(five())
    assert rows == [{"n": "fresh"}] * 3 + [None, None]
    assert calls.count("weak") == 3 and calls.count("none") == 5
    assert db.stats["confirm_skipped"] == 2
    out = capsys.readouterr().out
    assert out.count("confirm-miss budget spent") == 1


def test_token_bucket_refills():
    now = [0.0]
    bucket = d1_rqlite.TokenBucket(2, 2, clock=lambda: now[0])
    assert bucket.take() and bucket.take() and not bucket.take()
    now[0] = 0.5
    assert bucket.take() and not bucket.take()
    now[0] = 10.0
    assert bucket.take() and bucket.take() and not bucket.take()


def test_local_read_miss_is_confirmed(tmp_path):
    """read_mode=local: the miss path awaited its confirmation (it used to
    return an un-awaited coroutine)."""
    import sqlite3
    path = str(tmp_path / "db.sqlite")
    con = sqlite3.connect(path)
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("CREATE TABLE t (n TEXT)")
    con.commit()
    db = d1_rqlite.make_d1({"url": "http://127.0.0.1:1", "read_mode": "local",
                            "db_path": path})
    calls = []

    async def fake_post(payload, transaction=False, level=None):
        calls.append(level)
        return {"results": [{"types": {"n": "text"}, "rows": [{"n": "fresh"}]}]}

    db._post = fake_post
    row = asyncio.run(db.prepare("SELECT n FROM t").first())
    con.close()
    assert row == {"n": "fresh"} and calls == ["weak"]
