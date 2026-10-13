import ast
import asyncio
import json
import re
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
HOST_DIR = ROOT / "host"
WALLET = "A1ice1111111111111111111111111111111111111x"


def _load(names, namespace):
    wanted = set(names)
    for node in ast.parse(ENTRY_TEXT).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted:
                exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
        elif isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id in wanted
                for t in node.targets):
            exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
    return namespace


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})

    async def text(self):
        return self.body


def _json_response(data, status=200, cache_seconds=None, cache_control=None,
                   extra_headers=None):
    headers = {"content-type": "application/json; charset=utf-8"}
    if cache_control is not None:
        headers["cache-control"] = cache_control
    elif cache_seconds is not None:
        headers["cache-control"] = "public, max-age=%d" % cache_seconds
    return FakeResponse(json.dumps(data), status, headers)


class Db:
    def __init__(self, follows=(), repos=None):
        self.follows = set(follows)
        self.repos = repos or {}
        self.statements = []

    async def first(self, _env, sql, *args):
        self.statements.append(sql)
        if "is_following" in sql:
            target, _, viewer, _ = args
            return {
                "followers": sum(1 for f in self.follows if f[1] == target),
                "following": sum(1 for f in self.follows if f[0] == target),
                "is_following": int((viewer, target) in self.follows),
            }
        assert "SELECT 1 AS yes FROM profile_follows" in sql
        return {"yes": 1} if tuple(args) in self.follows else None

    async def all(self, _env, sql, *args):
        self.statements.append(sql)
        assert "owner_bi IN (" in sql
        return [{"data": data} for bi in args for data in self.repos.get(bi, [])]


def _profile_scope(db, rows=None):
    async def blind_index(_env, value):
        return "bi:" + value

    async def decrypt_row(_env, value):
        return json.loads(value)

    async def account_row(_env, name):
        return "bi:" + name, (rows or {}).get(name)

    namespace = {
        "re": re, "json": json, "parse_qs": parse_qs, "urlparse": urlparse,
        "clean_string": lambda v, n: str(v or "")[:n],
        "d1_first": db.first, "d1_all": db.all,
        "blind_index": blind_index, "decrypt_row": decrypt_row,
        "_account_row": account_row, "json_response": _json_response,
    }
    return _load([
        "NODE_NAME_RE", "MAX_NODE_NAME", "valid_node_name", "_owned_nodes",
        "_profile_owner_names_from_rec", "_profile_catalog_owner_names",
        "_account_mirror_count", "_account_social_counts",
        "_account_is_following", "_account_lookup_for_viewer"], namespace)


def test_social_counts_are_one_statement_plus_one_repository_read():
    public = json.dumps({"visibility": "public"})
    db = Db(follows={("bi:ann", "bi:jett"), ("bi:jett", "bi:bob")},
            repos={"bi:jett": [public], "bi:jettnode": [
                public, json.dumps({"visibility": "private"})]})
    scope = _profile_scope(db)
    rec = {"nodes": ["jettnode", "jettnode", "Bad Name"]}
    social = asyncio.run(scope["_account_social_counts"](
        object(), "jett", "ann", rec))
    assert social == {"followers": 1, "following": 1, "mirrorCount": 2,
                      "isFollowing": True}
    assert len(db.statements) == 2
    self_view = asyncio.run(scope["_account_social_counts"](
        object(), "jett", "jett", rec))
    assert self_view["isFollowing"] is False


def test_mirror_count_without_a_record_reads_the_owner_list():
    public = json.dumps({"visibility": "public"})
    db = Db(repos={"bi:jett": [public], "bi:n1": [public, public]})
    scope = _profile_scope(db, rows={"jett": {"nodes": ["n1"]}})
    assert asyncio.run(scope["_account_mirror_count"](object(), "jett")) == 3
    assert asyncio.run(scope["_account_mirror_count"](
        object(), "jett", {"nodes": []})) == 1


def test_a_viewer_lookup_reuses_the_guest_copy_with_a_live_follow_flag():
    db = Db(follows={("bi:ann", "bi:jett")})
    scope = _profile_scope(db)
    guest = {"ok": True, "name": "jett", "followers": 1,
             "isFollowing": False,
             "social": {"followers": 1, "isFollowing": False}}

    def lookup(viewer):
        request = SimpleNamespace(
            url="https://app.example/api/accounts/jett?viewer=" + viewer)
        return asyncio.run(scope["_account_lookup_for_viewer"](
            object(), request, FakeResponse(json.dumps(guest)), "jett"))

    ann = json.loads(lookup("ann").body)
    assert ann["isFollowing"] is True and ann["social"]["isFollowing"] is True
    assert {k: v for k, v in ann.items() if k not in (
        "isFollowing", "social")} == {
            k: v for k, v in guest.items() if k not in (
                "isFollowing", "social")}
    for viewer in ("jett", "bob", "", "Bad Name"):
        response = lookup(viewer)
        assert json.loads(response.body)["isFollowing"] is False
        assert "cache-control" not in response.headers
    assert asyncio.run(scope["_account_lookup_for_viewer"](
        object(), SimpleNamespace(url="https://x/?viewer=a"),
        FakeResponse("not json"), "jett")) is None


def test_profile_handler_serves_viewers_from_the_guest_copy_after_privacy():
    start = ENTRY_TEXT.index("        profile_private = bool(rec.get(\"profile_private\"))")
    handler = ENTRY_TEXT[start:ENTRY_TEXT.index(
        "async def _account_lookup_for_viewer(")]
    privacy = handler.index("if profile_private:")
    reuse = handler.index("_account_lookup_for_viewer(")
    assert privacy < handler.index("if not profile_private:") < reuse
    assert "guest_social = {**social, \"isFollowing\": False}" in handler
    follow = ENTRY_TEXT[ENTRY_TEXT.index("async def _account_follow("):]
    follow = follow[:follow.index("\n\n\n")]
    assert "for purge_name in (target, follower):" in follow


class WalletHarness:
    def __init__(self, background=True):
        self.cache = {}
        self.calls = 0
        self.now = 10_000_000
        self.scheduled = []
        self.answer = {"ok": True, "address": WALLET, "lamports": 5,
                       "sol": 5e-9, "checkedAt": 1, "txBucket": "today"}
        harness = self

        async def badge(_env, _request):
            harness.calls += 1
            return _json_response(dict(harness.answer), cache_seconds=3600)

        async def edge_cache_match(key):
            return harness.cache.get(key)

        def host_background(_env, key, factory):
            if not background:
                return False
            harness.scheduled.append((key, factory))
            return True

        self.scope = _load([
            "SOLANA_RE", "WORLD_WALLET_CACHE_PREFIX", "WORLD_WALLET_STALE_MS",
            "WORLD_WALLET_STALE_CACHE_CONTROL", "WORLD_WALLET_LAST_MAX",
            "_WORLD_WALLET_LAST", "_world_wallet_address",
            "_world_wallet_remember", "_world_wallet_refresh",
            "world_wallet_swr_handler"], {
                "re": re, "json": json, "parse_qs": parse_qs,
                "urlparse": urlparse,
                "method_name": lambda request: request.method,
                "world_wallet_badge_handler": badge,
                "edge_cache_match": edge_cache_match,
                "_host_background": host_background,
                "json_response": _json_response,
                "Response": FakeResponse,
                "Date": SimpleNamespace(now=lambda: harness.now),
            })

    def get(self, query="address=" + WALLET, method="GET"):
        request = SimpleNamespace(
            method=method, url="https://app.example/api/world/wallet?" + query)
        return asyncio.run(self.scope["world_wallet_swr_handler"](
            object(), request))


def test_wallet_serves_the_last_reading_while_the_host_refreshes():
    harness = WalletHarness()
    first = harness.get()
    assert harness.calls == 1 and json.loads(first.body)["lamports"] == 5
    assert first.headers["cache-control"] == "public, max-age=3600"
    harness.answer["lamports"] = 9
    stale = harness.get()
    assert harness.calls == 1
    assert json.loads(stale.body)["lamports"] == 5
    assert stale.headers["cache-control"] == "public, max-age=60"
    [(key, factory)] = harness.scheduled
    assert key == "world-wallet:" + WALLET
    asyncio.run(factory())
    assert harness.calls == 2
    assert json.loads(harness.get().body)["lamports"] == 9


def test_wallet_passes_through_refresh_bad_addresses_and_old_readings():
    harness = WalletHarness()
    harness.get("address=" + WALLET + "&refresh=1")
    harness.get("address=" + WALLET + "&refresh=1")
    assert harness.calls == 2 and harness.scheduled == []
    harness.get("address=nope")
    assert harness.calls == 3
    harness.get()
    harness.now += 25 * 60 * 60 * 1000
    harness.get()
    assert harness.calls == 5 and harness.scheduled == []
    harness.answer = {"ok": True, "lamports": None, "txBucket": ""}
    other = "B" + WALLET[1:]
    harness.get("address=" + other)
    harness.get("address=" + other)
    assert harness.calls == 7


def test_wallet_without_a_host_fetches_inline():
    harness = WalletHarness(background=False)
    harness.get()
    harness.get()
    assert harness.calls == 2


def test_the_app_routes_the_wallet_through_the_stale_while_revalidate_wrapper():
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    assert "world_wallet_swr_handler(self.env, request)" in runtime
    assert "world_wallet_badge_handler(self.env, request)" not in runtime


@pytest.fixture
def background_module():
    if str(HOST_DIR) not in sys.path:
        sys.path.insert(0, str(HOST_DIR))
    from forkmesh_host import background
    return background


def test_host_background_runs_one_task_per_key(background_module):
    tracked = []
    runner = background_module.BackgroundRefresh(tracked.append)
    runs = []

    async def scenario():
        gate = asyncio.Event()

        async def work():
            runs.append(1)
            await gate.wait()

        assert runner("k", work) is True
        assert runner("k", work) is True
        assert runner("", work) is False
        await asyncio.sleep(0)
        assert list(runner.running) == ["k"]
        gate.set()
        await asyncio.gather(*tracked)
        assert runner.running == {}

    asyncio.run(scenario())
    assert runs == [1]
    assert runner("k", lambda: None) is False


def test_host_background_survives_a_failing_refresh(background_module):
    runner = background_module.BackgroundRefresh()

    async def boom():
        raise RuntimeError("down")

    async def scenario():
        assert runner("x", boom) is True
        await asyncio.sleep(0.01)

    asyncio.run(scenario())
    assert runner.failed == 1 and runner.running == {}


def test_host_background_is_bounded(background_module):
    runner = background_module.BackgroundRefresh(max_running=1)

    async def scenario():
        gate = asyncio.Event()
        assert runner("a", gate.wait) is True
        assert runner("b", gate.wait) is False
        gate.set()
        await asyncio.sleep(0.01)

    asyncio.run(scenario())


def test_host_join_shares_one_flight(background_module):
    runner = background_module.BackgroundRefresh()
    calls = []

    async def scenario():
        gate = asyncio.Event()

        async def load():
            calls.append(1)
            await gate.wait()
            return {"v": 1}

        waiters = [asyncio.ensure_future(runner.join("k", load))
                   for _ in range(3)]
        await asyncio.sleep(0)
        gate.set()
        return await asyncio.gather(*waiters)

    assert asyncio.run(scenario()) == [{"v": 1}] * 3
    assert calls == [1] and runner.joined == 2 and runner.flights == {}


def test_host_warm_waits_for_unseal_and_calls_the_entry(background_module):
    warmed = []

    async def warm(env):
        warmed.append(env)

    runtime = SimpleNamespace(
        entry=SimpleNamespace(host_warm_caches=warm), env=object(),
        sealed=True)

    async def scenario():
        task = asyncio.ensure_future(background_module.warm_caches(
            runtime, wait_seconds=5, poll_seconds=0.01))
        await asyncio.sleep(0.05)
        assert warmed == []
        runtime.sealed = False
        return await task

    assert asyncio.run(scenario()) is True
    assert warmed == [runtime.env]
    runtime.sealed = True
    assert asyncio.run(background_module.warm_caches(
        runtime, wait_seconds=0.02, poll_seconds=0.01)) is False
    assert background_module.warm_enabled({"FORKMESH_HOST_WARM": "0"}) is False
    assert background_module.warm_enabled({}) is True


def test_the_warm_entry_builds_the_world_roster_and_inventory():
    warm = ENTRY_TEXT[ENTRY_TEXT.index("async def host_warm_caches(env):"):]
    warm = warm[:warm.index("\n\n\n")]
    assert "await _users_world_refresh(env)" in warm
    assert "await _world_system_capacity(env)" in warm
