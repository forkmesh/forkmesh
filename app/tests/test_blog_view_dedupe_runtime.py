#!/usr/bin/env python3
"""Runtime behaviour of per-post blog reach (adhoc #1684).

Two claims the rendered "Post reach" panel makes are load-bearing enough to
run rather than only grep for:

  * a reload is not another view — the same reader folded again inside the
    window leaves ``views`` alone, and only counts again once the window has
    actually passed, measured from the last read that counted;
  * a network that is demonstrably sending readers fills its "Posted on" slot,
    so a post announced on a Mastodon instance stops reading as unannounced
    before anyone pastes the permalink back in.

The real function bodies are lifted out of ``src/entry.py`` and run against an
in-memory SQLite standing in for D1, so the SQL, the window arithmetic and the
host classification are all exercised as written.
"""

import ast
import asyncio
import hashlib
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
TEXT = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")

sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(REPO / "world" / "src"))

import world_visitors  # noqa: E402
from catalog import clean_string  # noqa: E402
from schema import SCHEMA_STATEMENTS  # noqa: E402


_TREE = ast.parse(TEXT)


def _source(name):
    """Source of one top-level function or assignment in ``entry.py``."""
    for node in _TREE.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name == name:
                return ast.get_source_segment(TEXT, node)
        elif isinstance(node, ast.Assign):
            targets = [
                t.id for t in node.targets if isinstance(t, ast.Name)
            ]
            if name in targets:
                return ast.get_source_segment(TEXT, node)
    raise AssertionError(name)


class FakeD1:
    """The slice of D1 the reach path uses, backed by SQLite."""

    def __init__(self):
        self.connection = sqlite3.connect(":memory:")
        self.connection.row_factory = sqlite3.Row
        for statement in SCHEMA_STATEMENTS:
            if "blog_post_" in statement:
                self.connection.execute(statement)

    async def run(self, env, sql, *args):
        self.connection.execute(sql, args)

    async def first(self, env, sql, *args):
        row = self.connection.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.connection.execute(sql, args)]


def _namespace(clock):
    db = FakeD1()

    async def blind_index(env, value):
        return hashlib.sha256(value.encode("utf-8")).hexdigest()

    async def ensure_schema(env):
        return None

    namespace = {
        "re": re,
        "urlparse": urlparse,
        "instance_privacy": SimpleNamespace(active=lambda _env: False,
                                            is_pseudonym=lambda _raw: False),
        "clean_string": clean_string,
        "world_visitor_metrics": world_visitors,
        "Date": SimpleNamespace(now=lambda: clock["now"]),
        "blind_index": blind_index,
        "ensure_schema": ensure_schema,
        "d1_run": db.run,
        "d1_first": db.first,
        "d1_all": db.all,
        # The handler's shape is not under test here, so the payload is
        # returned straight through instead of being serialised.
        "json_response": lambda data, **kwargs: data,
    }
    for name in (
        "MAX_SITE_REFERRER_HOST", "SITE_REFERRER_SELF_HOSTS",
        "_SITE_REFERRER_HOST_RE", "BLOG_POST_SLUG_RE",
        "BLOG_POST_UNIQUE_PRECISION", "BLOG_POST_REFERRER_LIMIT",
        "BLOG_POST_REFERRER_SCAN_LIMIT", "BLOG_POST_NETWORKS",
        "BLOG_POST_LINK_MAX", "BLOG_POST_VIEW_WINDOW_MS",
        "BLOG_POST_VIEW_SEEN_TTL_MS", "BLOG_POST_NETWORK_HOSTS",
        "BLOG_POST_MASTODON_HOST_MARKERS",
        "_site_referrer_host", "_blog_post_slug", "_blog_post_permalink",
        "_blog_post_network_for_host", "blog_post_links",
        "_fold_blog_post_view", "blog_post_metrics",
        "prune_blog_post_view_seen", "_visitor_count_address",
    ):
        exec(_source(name), namespace)
    return namespace, db


def _request(address="203.0.113.9", agent="Mozilla/5.0 (X11; Linux x86_64)"):
    return SimpleNamespace(headers={
        "cf-connecting-ip": address,
        "user-agent": agent,
    })


def _read(namespace, clock, referer="", address="203.0.113.9", at=None):
    if at is not None:
        clock["now"] = at
    return asyncio.run(namespace["_fold_blog_post_view"](
        None, _request(address), "actions", referer, "forkmesh.com"))


def _metrics(namespace):
    return asyncio.run(namespace["blog_post_metrics"](None, "actions"))


DAY = 24 * 60 * 60 * 1000


def test_a_reload_is_not_another_view():
    clock = {"now": 1_700_000_000_000}
    namespace, db = _namespace(clock)

    start = clock["now"]
    _read(namespace, clock, referer="https://mastodon.social/@forkmesh/1")
    assert _metrics(namespace)["views"] == 1

    # Seven refreshes over the next few hours: the same reader, still one view.
    for index in range(1, 8):
        _read(
            namespace, clock,
            referer="https://mastodon.social/@forkmesh/1",
            at=start + index * 60 * 60 * 1000,
        )
    payload = _metrics(namespace)
    assert payload["views"] == 1
    assert payload["uniqueViews"] == 1

    # The reads still happened — they are recorded, just not as extra views.
    hits, counted_at = db.connection.execute(
        "SELECT hits, counted_at FROM blog_post_view_seen").fetchone()
    assert hits == 8
    assert counted_at == 1_700_000_000_000


def test_a_second_reader_and_a_return_read_both_count():
    clock = {"now": 1_700_000_000_000}
    namespace, _ = _namespace(clock)

    _read(namespace, clock)
    _read(namespace, clock, address="198.51.100.4")
    assert _metrics(namespace)["views"] == 2

    # A genuine return read the next day is a real read and counts again.
    _read(namespace, clock, at=clock["now"] + DAY)
    assert _metrics(namespace)["views"] == 3


def test_reloading_forever_cannot_push_the_window_ahead_of_itself():
    # The window runs from the last read that counted, not the last hit, so a
    # reader refreshing every few hours still counts once a day rather than
    # never again.
    clock = {"now": 1_700_000_000_000}
    namespace, _ = _namespace(clock)

    start = clock["now"]
    _read(namespace, clock)
    for hour in range(1, 30):
        _read(namespace, clock, at=start + hour * 60 * 60 * 1000)
    assert _metrics(namespace)["views"] == 2


def test_a_reload_does_not_credit_its_referrer_twice():
    clock = {"now": 1_700_000_000_000}
    namespace, _ = _namespace(clock)

    # document.referrer survives a reload, so the beacon reports the same
    # source again; the leaderboard must not walk up with it.
    for index in range(4):
        _read(
            namespace, clock,
            referer="https://mastodon.social/@forkmesh/1",
            at=clock["now"] + index * 60 * 1000,
        )
    board = _metrics(namespace)["referrers"]
    assert [(row["host"], row["visits"]) for row in board] == [
        ("mastodon.social", 1),
    ]

    # Arriving from a genuinely different source inside the same window is a
    # new source and does count.
    _read(
        namespace, clock,
        referer="https://news.ycombinator.com/item?id=1",
        at=clock["now"] + 10 * 60 * 1000,
    )
    hosts = {
        row["host"]: row["visits"]
        for row in _metrics(namespace)["referrers"]
    }
    assert hosts == {"mastodon.social": 1, "news.ycombinator.com": 1}


def test_a_detected_mastodon_instance_fills_its_posted_on_slot():
    clock = {"now": 1_700_000_000_000}
    namespace, _ = _namespace(clock)

    _read(namespace, clock, referer="https://mastodon.social/@forkmesh/1")
    payload = _metrics(namespace)

    assert payload["links"] == {}
    assert payload["detected"]["mastodon"] == {
        "network": "mastodon",
        "host": "mastodon.social",
        "url": "https://mastodon.social/",
        "visits": 1,
    }
    # Reddit and X were not detected, so their slots stay honestly empty.
    assert set(payload["detected"]) == {"mastodon"}


def test_a_saved_permalink_wins_over_a_detected_instance():
    clock = {"now": 1_700_000_000_000}
    namespace, db = _namespace(clock)

    _read(namespace, clock, referer="https://mastodon.social/@forkmesh/1")
    db.connection.execute(
        "INSERT INTO blog_post_links (slug,network,url,updated_at,"
        "updated_by_bi) VALUES (?,?,?,?,?)",
        ("actions", "mastodon", "https://mastodon.social/@forkmesh/1",
         clock["now"], "admin-bi"))

    payload = _metrics(namespace)
    assert payload["links"] == {
        "mastodon": "https://mastodon.social/@forkmesh/1",
    }
    # Detection is a fallback for a blank slot, never a competing value.
    assert payload["detected"] == {}


@pytest.mark.parametrize(
    "referer,network",
    [
        ("https://mastodon.social/@forkmesh/1", "mastodon"),
        ("https://hachyderm.io/@forkmesh/1", "mastodon"),
        ("https://mstdn.example.org/@forkmesh/1", "mastodon"),
        ("https://mastodon.example.org/@forkmesh/1", "mastodon"),
        ("https://www.reddit.com/r/opensource/comments/1/", "reddit"),
        ("https://old.reddit.com/r/opensource/comments/1/", "reddit"),
        ("https://x.com/forkmesh/status/1", "twitter"),
        ("https://t.co/abcd", "twitter"),
        ("https://news.ycombinator.com/item?id=1", ""),
    ],
)
def test_referring_hosts_classify_to_the_network_that_sent_them(
        referer, network):
    clock = {"now": 1_700_000_000_000}
    namespace, _ = _namespace(clock)

    _read(namespace, clock, referer=referer)
    detected = _metrics(namespace)["detected"]
    assert set(detected) == ({network} if network else set())


def test_detection_reads_past_the_ten_rows_the_board_shows():
    # The instance a post was announced on is usually a small referrer. If
    # detection only saw the visible top ten, a busy post's Mastodon slot
    # would flip back to "not posted" exactly when the post did well.
    clock = {"now": 1_700_000_000_000}
    namespace, db = _namespace(clock)

    _read(namespace, clock, referer="https://mastodon.social/@forkmesh/1")
    for index in range(20):
        db.connection.execute(
            "INSERT INTO blog_post_referrers (slug,host,visits,last_ts) "
            "VALUES (?,?,?,?)",
            ("actions", "aggregator%02d.example.com" % index, 100 + index,
             clock["now"]))

    payload = _metrics(namespace)
    assert len(payload["referrers"]) == 10
    shown = {row["host"] for row in payload["referrers"]}
    assert "mastodon.social" not in shown
    assert payload["detected"]["mastodon"]["host"] == "mastodon.social"


def test_dedupe_rows_age_out_and_leave_the_counters_alone():
    clock = {"now": 1_700_000_000_000}
    namespace, db = _namespace(clock)

    _read(namespace, clock)
    clock["now"] += namespace["BLOG_POST_VIEW_SEEN_TTL_MS"] + 1
    asyncio.run(namespace["prune_blog_post_view_seen"](None))

    assert db.connection.execute(
        "SELECT COUNT(*) FROM blog_post_view_seen").fetchone()[0] == 0
    assert _metrics(namespace)["views"] == 1
