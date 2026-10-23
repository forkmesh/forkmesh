import ast
import asyncio
import contextvars
import json
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]
ENTRY_TEXT = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
USERS_TEXT = (ROOT / "src" / "account_users.py").read_text(encoding="utf-8")
ROUTES_TEXT = (ROOT / "src" / "route_handlers.py").read_text(
    encoding="utf-8")
TOKEN = "v2." + "s" * 24 + "." + "t" * 32
WALLET = "A1ice1111111111111111111111111111111111111x"


PARSED = {}


def _parsed(text):
    if text not in PARSED:
        PARSED[text] = ast.parse(text).body
    return PARSED[text]


def _load(names, namespace, sources=(ENTRY_TEXT, USERS_TEXT, ROUTES_TEXT)):
    wanted = set(names)
    found = set()
    for text in sources:
        for node in _parsed(text):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.name in wanted and node.name not in found:
                    exec(ast.get_source_segment(text, node), namespace)
                    found.add(node.name)
            elif isinstance(node, ast.Assign):
                targets = {t.id for t in node.targets
                           if isinstance(t, ast.Name)} & wanted
                export = (isinstance(node.value, ast.Call)
                          and isinstance(node.value.func, ast.Attribute)
                          and node.value.func.attr == "export")
                if targets and not export and not targets & found:
                    exec(ast.get_source_segment(text, node), namespace)
                    found |= targets
    assert found == wanted, wanted - found
    return namespace


def _function(name):
    for node in _parsed(ENTRY_TEXT):
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name == name):
            return ast.get_source_segment(ENTRY_TEXT, node)
    raise AssertionError(name)


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
    if extra_headers:
        headers.update(extra_headers)
    return FakeResponse(json.dumps(data, separators=(",", ":")), status,
                        headers)


class Clock:
    def __init__(self):
        self.now = 50_000_000

    def date(self):
        return SimpleNamespace(now=lambda: self.now)


SESSION_NAMES = [
    "ACCOUNT_SESSION_TOKEN_RE", "ACCOUNT_SESSION_MEMO_MS",
    "ACCOUNT_SESSION_MEMO_MAX", "_ACCOUNT_SESSION_MEMO",
    "_ACCOUNT_SESSION_MEMO_EPOCH",
    "_account_session_memo_forget", "_account_session_memo_store",
    "_account_session_lookup_memo", "_account_session_record", "_is_admin",
    "_is_admin_flag",
    "_D1_WATCH_TABLES_RE", "_D1_WATCH_WRITE_RE", "_D1_WATCH_SESSION_TABLES",
    "_D1_WATCH_SESSION_TOUCH", "_D1_WATCH_GENERATIONS", "_D1_WATCH_NODE_TOUCH",
    "_d1_write_watch",
]


class SessionHarness:
    def __init__(self, join=False):
        self.clock = Clock()
        self.lookups = 0
        self.admin_reads = 0
        self.valid = True
        self.admin = True
        self.flights = []
        harness = self

        async def lookup(_env, token, touch=True):
            harness.lookups += 1
            await asyncio.sleep(0)
            if not harness.valid:
                return "", None, ""
            return "bi:jett", {"name": "jett", "prefs": {"a": 1}}, "sess1"

        async def d1_first(_env, sql, *args):
            harness.admin_reads += 1
            return {"is_admin": 1 if harness.admin else 0}

        async def blind_index(_env, value):
            return "bi:" + value

        async def account_row(_env, name):
            return "bi:" + name, {"name": name, "totp_enrolled": True}

        class Join:
            def __init__(self):
                self.flights = {}

            async def __call__(self, key, factory):
                harness.flights.append(key)
                task = self.flights.get(key)
                if task is None:
                    task = asyncio.ensure_future(factory())
                    self.flights[key] = task
                return await asyncio.shield(task)

        joiner = Join() if join else None
        self.scope = _load(SESSION_NAMES, {
            "re": re, "json": json, "Date": self.clock.date(),
            "clean_string": lambda v, n: str(v or "")[:n],
            "_account_session_token_digest": lambda _env, t: "digest:" + t,
            "_host_single_flight": lambda _env: joiner,
            "_account_session_lookup": lookup,
            "d1_first": d1_first, "blind_index": blind_index,
            "_account_row": account_row,
            "method_name": lambda request: request.method,
            "_request_account_session_token":
                lambda request, payload: (request.token, False),
            "_request_origin_allowed": lambda env, request: True,
            "UNVERIFIED_SAFE_METHODS": ("GET", "HEAD", "OPTIONS"),
            "_account_email_unverified": lambda rec: False,
        })
        self.host_env = SimpleNamespace(HOST_BACKGROUND=object())
        self.worker_env = SimpleNamespace()

    def record(self, method="GET", env=None, token=TOKEN):
        request = SimpleNamespace(method=method, token=token)
        return asyncio.run(self.scope["_account_session_record"](
            env or self.host_env, request))


def test_a_get_reuses_the_session_for_five_seconds_on_the_host():
    harness = SessionHarness()
    first = harness.record()
    assert first == ("bi:jett", {"name": "jett", "prefs": {"a": 1}})
    harness.clock.now += 4_000
    assert harness.record() == first
    assert harness.lookups == 1
    harness.clock.now += 1_500
    harness.record()
    assert harness.lookups == 2


def test_unsafe_methods_and_the_worker_always_look_the_session_up():
    harness = SessionHarness()
    harness.record()
    harness.record(method="POST")
    harness.record(method="DELETE")
    harness.record(env=harness.worker_env)
    harness.record(env=harness.worker_env)
    assert harness.lookups == 5


def test_a_failed_lookup_is_not_remembered():
    harness = SessionHarness()
    harness.valid = False
    assert harness.record() == ("", None)
    assert harness.record() == ("", None)
    assert harness.lookups == 2
    assert harness.record(token="not-a-token") == ("", None)
    assert harness.lookups == 2


def test_each_caller_gets_its_own_copy_of_the_record():
    harness = SessionHarness()
    _, rec = harness.record()
    rec["prefs"]["a"] = 99
    rec["name"] = "mallory"
    _, again = harness.record()
    assert again == {"name": "jett", "prefs": {"a": 1}}
    assert harness.lookups == 1


def test_a_write_to_sessions_devices_or_users_drops_the_memo():
    harness = SessionHarness()
    watch = harness.scope["_d1_write_watch"]
    for sql in (
            "UPDATE account_sessions SET revoked_at=? WHERE account_bi=?",
            "UPDATE account_devices SET revoked_at=?, enabled=0 WHERE x=?",
            "INSERT INTO users (user_bi, data) VALUES (?,?) ON CONFLICT",
            "DELETE FROM users WHERE user_bi=?",
            "  update users set is_admin=1 where user_bi=?"):
        before = harness.lookups
        harness.record()
        harness.record()
        assert harness.lookups == before + 1
        watch(sql)
    harness.record()
    assert harness.lookups == 6


def test_reads_and_the_session_touch_keep_the_memo():
    harness = SessionHarness()
    harness.record()
    watch = harness.scope["_d1_write_watch"]
    for sql in (
            "SELECT data FROM users WHERE user_bi=?",
            "WITH x AS (SELECT 1 FROM users) SELECT * FROM x",
            "UPDATE account_sessions SET last_seen_at=?,"
            "expires_at=MAX(expires_at,created_at+?) WHERE session_id=?",
            "INSERT INTO world_user_activity (account_bi) VALUES (?)",
            "UPDATE account_users_log SET x=1",
            "PRAGMA table_info(users)"):
        watch(sql)
    harness.record()
    assert harness.lookups == 1


def test_a_lookup_overtaken_by_a_write_is_not_stored():
    harness = SessionHarness()
    original = harness.scope["_account_session_lookup"]

    async def racing(env, token, touch=True):
        result = await original(env, token, touch)
        harness.scope["_d1_write_watch"](
            "UPDATE account_sessions SET revoked_at=? WHERE session_id=?")
        return result

    harness.scope["_account_session_lookup"] = racing
    harness.record()
    harness.record()
    assert harness.lookups == 2


def test_concurrent_gets_share_one_lookup():
    harness = SessionHarness(join=True)

    async def scenario():
        requests = [SimpleNamespace(method="GET", token=TOKEN)
                    for _ in range(4)]
        return await asyncio.gather(*(
            harness.scope["_account_session_record"](harness.host_env, r)
            for r in requests))

    results = asyncio.run(scenario())
    assert harness.lookups == 1
    assert len({id(rec) for _, rec in results}) == 4
    assert harness.flights == ["account-session:digest:" + TOKEN] * 4


def test_admin_flag_is_read_fresh_on_every_check():
    harness = SessionHarness()
    is_admin = harness.scope["_is_admin"]
    env = harness.host_env
    assert asyncio.run(is_admin(env, "jett")) is True
    harness.admin = False
    assert asyncio.run(is_admin(env, "Jett ")) is False
    harness.admin = True
    assert asyncio.run(is_admin(env, "jett")) is True
    assert harness.admin_reads == 3


def test_every_logged_statement_reaches_the_write_watch():
    body = _function("_log_d1_query")
    assert 'globals().get("_d1_write_watch")' in body
    assert "watch(sql)" in body


def test_contribution_writes_move_the_view_generation():
    harness = SessionHarness()
    watch = harness.scope["_d1_write_watch"]
    generations = harness.scope["_D1_WATCH_GENERATIONS"]
    watch("SELECT * FROM profile_contribution_projects")
    watch("WITH p AS (SELECT * FROM repositories) SELECT * FROM p")
    assert generations["contributions"] == 0
    watch("UPDATE profile_contribution_projects SET is_public=0 WHERE x=?")
    watch("DELETE FROM profile_contribution_days WHERE source_repo_bi=?")
    watch("UPDATE repositories SET is_private=1 WHERE key_bi=?")
    assert generations["contributions"] == 3
    assert generations["repositories"] == 1


def test_endpoint_and_signing_key_writes_move_their_generations():
    harness = SessionHarness()
    watch = harness.scope["_d1_write_watch"]
    generations = harness.scope["_D1_WATCH_GENERATIONS"]
    watch("SELECT node_name FROM mirror_https_endpoints")
    watch("SELECT data FROM nodes WHERE node_bi=?")
    watch("UPDATE runner_nodes SET last_seen=? WHERE id=?")
    watch("UPDATE nodes SET last_seen=? WHERE node_bi=?")
    assert generations["mirror_https_endpoints"] == 0
    assert generations["signing_keys"] == 0
    watch("UPDATE mirror_https_endpoints SET healthy=0 WHERE node_name=?")
    assert generations["mirror_https_endpoints"] == 1
    watch("INSERT INTO nodes (node_bi, user_bi, pubkey) VALUES (?,?,?)")
    watch("UPDATE account_devices SET revoked_at=? WHERE device_bi=?")
    watch("DELETE FROM users WHERE user_bi=?")
    assert generations["signing_keys"] == 3


LOOKUP_NAMES = [
    "ACCOUNT_LOOKUP_CLIENT_SECONDS", "ACCOUNT_LOOKUP_FRESH_MS",
    "ACCOUNT_LOOKUP_STALE_SECONDS", "ACCOUNT_LOOKUP_BUILT_HEADER",
    "ACCOUNT_LOOKUP_REFRESH_ORIGIN", "_ACCOUNT_LOOKUP_REFRESH",
    "_account_lookup_store", "_account_lookup_cached",
    "_account_lookup_refresh",
]


class LookupHarness:
    def __init__(self, background=True):
        self.clock = Clock()
        self.cache = {}
        self.epochs = {}
        self.scheduled = []
        self.private = False
        self.builds = 0
        self.handled = []
        harness = self

        async def edge_cache_match(key):
            return harness.cache.get(key)

        async def edge_cache_put(key, response):
            harness.cache[key] = response

        def host_background(_env, key, factory):
            if not background:
                return False
            harness.scheduled.append((key, factory))
            return True

        async def accounts_handler(env, request):
            harness.handled.append(request.url)
            key = "https://internal/api/accounts/jett"
            if harness.private:
                return None
            cached = await harness.scope["_account_lookup_cached"](
                env, key, "jett")
            if cached is not None:
                return cached
            harness.builds += 1
            if harness.purge_during_build:
                harness.epochs[key] = harness.epochs.get(key, 0) + 1
            await harness.scope["_account_lookup_store"](
                env, key, {"ok": True, "build": harness.builds})

        self.purge_during_build = False
        self.scope = _load(LOOKUP_NAMES, {
            "contextvars": contextvars,
            "Date": self.clock.date(), "json_response": _json_response,
            "Response": FakeResponse, "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "edge_cache_epoch": lambda key: harness.epochs.get(key, 0),
            "_host_background": host_background,
            "accounts_handler": accounts_handler,
            "JsRequest": SimpleNamespace(
                new=lambda url: SimpleNamespace(url=url, method="GET")),
            "clean_string": lambda v, n: str(v or "")[:n],
            "MAX_NODE_NAME": 64, "quote": quote,
        }, sources=(USERS_TEXT,))
        self.env = SimpleNamespace(HOST_BACKGROUND=object())
        self.key = "https://internal/api/accounts/jett"

    def run(self, coro):
        return asyncio.run(coro)

    def cached(self, env=None):
        return self.run(self.scope["_account_lookup_cached"](
            env or self.env, self.key, "Jett"))

    def refresh(self):
        return self.run(self.scope["_account_lookup_refresh"](
            self.env, self.key, "Jett"))


def test_the_host_keeps_the_guest_profile_and_serves_it_stale_while_refreshing():
    harness = LookupHarness()
    harness.refresh()
    assert harness.handled == ["https://forkmesh.internal/api/accounts/jett"]
    stored = harness.cache[harness.key]
    assert stored.headers["cache-control"] == "public, max-age=900"
    assert stored.headers["x-forkmesh-built"] == str(harness.clock.now)
    fresh = harness.cached()
    assert json.loads(fresh.body)["build"] == 1
    assert fresh.headers == {
        "content-type": "application/json; charset=utf-8",
        "cache-control": "public, max-age=60"}
    assert harness.scheduled == []
    harness.clock.now += 14 * 60 * 1000
    stale = harness.cached()
    assert json.loads(stale.body)["build"] == 1
    [(key, factory)] = harness.scheduled
    assert key == "account-lookup:" + harness.key
    harness.run(factory())
    assert harness.builds == 2
    assert json.loads(harness.cached().body)["build"] == 2


def test_without_a_host_background_a_stale_guest_copy_is_a_miss():
    harness = LookupHarness(background=False)
    harness.refresh()
    assert harness.cached() is not None
    harness.clock.now += 61_000
    assert harness.cached() is None


def test_the_worker_keeps_its_sixty_second_guest_copy():
    harness = LookupHarness()
    worker = SimpleNamespace()
    harness.run(harness.scope["_account_lookup_store"](
        worker, harness.key, {"ok": True}))
    stored = harness.cache[harness.key]
    assert stored.headers["cache-control"] == "public, max-age=60"
    assert "x-forkmesh-built" not in stored.headers
    assert harness.cached(worker) is stored


def test_a_refresh_skips_private_profiles_and_purges_during_the_build():
    harness = LookupHarness()
    harness.private = True
    harness.refresh()
    assert harness.cache == {}
    harness.private = False
    harness.purge_during_build = True
    harness.refresh()
    assert harness.builds == 1 and harness.cache == {}
    harness.purge_during_build = False
    harness.refresh()
    assert harness.cache[harness.key] is not None


def test_the_profile_handler_reads_and_writes_through_the_lookup_cache():
    start = ENTRY_TEXT.index("        profile_private = bool(rec.get(\"profile_private\"))")
    handler = ENTRY_TEXT[start:ENTRY_TEXT.index("\n\n\n", start)]
    assert "await _account_lookup_cached(" in handler
    assert "edge_cache_match(lookup_cache_key)" not in handler
    assert "edge_cache_put(" not in handler
    assert handler.index("if profile_private:") < handler.index(
        "await _account_lookup_cached(")
    assert handler.count("await _account_lookup_store(") == 2
    assert "return json_response(payload, cache_seconds=60)" in handler


VIEW_NAMES = [
    "PROFILE_CONTRIBUTION_VIEW_FRESH_MS",
    "PROFILE_CONTRIBUTION_VIEW_STALE_SECONDS",
    "PROFILE_CONTRIBUTION_VIEW_BUILT_HEADER", "_contribution_cache_key",
    "_contribution_view_key", "_contribution_view_put",
    "_contribution_view_cached",
]


class ViewHarness:
    def __init__(self, background=True):
        self.clock = Clock()
        self.cache = {}
        self.scheduled = []
        self.refreshes = []
        self.generations = {"contributions": 0}
        harness = self

        async def edge_cache_match(key):
            return harness.cache.get(key)

        async def edge_cache_put(key, response):
            harness.cache[key] = response

        def host_background(_env, key, factory):
            if not background:
                return False
            harness.scheduled.append((key, factory))
            return True

        async def api(env, request, name, view_refresh=False):
            harness.refreshes.append((name, view_refresh))

        self.scope = _load(VIEW_NAMES, {
            "json": json, "quote": quote, "Date": self.clock.date(),
            "json_response": _json_response,
            "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "_host_background": host_background,
            "_contribution_profile_api": api,
            "_D1_WATCH_GENERATIONS": self.generations,
            "PROFILE_CONTRIBUTION_CACHE_PREFIX": "https://internal/pc/",
        })
        self.payload = {"ok": True, "profile": "jett",
                        "range": {"from": "2025-10-01", "to": "2026-09-30"},
                        "summary": {"commits": 3}}

    def key(self):
        return self.scope["_contribution_view_key"](
            "bi:jett", "2025-10-01", "2026-09-30")

    def put(self):
        asyncio.run(self.scope["_contribution_view_put"](
            self.key(), self.payload))

    def get(self, profile="jett", to="2026-09-30"):
        return asyncio.run(self.scope["_contribution_view_cached"](
            object(), SimpleNamespace(url="u"), self.key(), profile,
            "2025-10-01", to))


def test_a_fresh_contribution_view_answers_without_queries():
    harness = ViewHarness()
    harness.put()
    served = harness.get()
    assert json.loads(served.body) == harness.payload
    assert served.headers["cache-control"] == (
        "no-store, max-age=0, must-revalidate")
    assert harness.scheduled == []
    assert harness.cache[harness.key()].headers["cache-control"] == (
        "public, max-age=600")


def test_a_stale_contribution_view_is_served_while_the_host_recomputes():
    harness = ViewHarness()
    harness.put()
    harness.clock.now += 5 * 60 * 1000
    served = harness.get()
    assert json.loads(served.body) == harness.payload
    [(key, factory)] = harness.scheduled
    assert key == "contribution-view:" + harness.key()
    asyncio.run(factory())
    assert harness.refreshes == [("jett", True)]


def test_a_contribution_view_misses_without_background_or_on_mismatch():
    harness = ViewHarness(background=False)
    harness.put()
    assert harness.get() is not None
    assert harness.get(profile="bob") is None
    assert harness.get(to="2026-09-29") is None
    harness.clock.now += 61_000
    assert harness.get() is None


def test_a_contribution_write_retires_every_view():
    harness = ViewHarness()
    harness.put()
    before = harness.key()
    harness.generations["contributions"] += 1
    assert harness.key() != before
    assert harness.get() is None


def test_the_contribution_api_checks_the_view_before_any_heavy_query():
    body = _function("_contribution_profile_api")
    view = body.index("await _contribution_view_cached(")
    assert body.index('rec.get("profile_private")') < view
    assert view < body.index("_contribution_cached_reporter_summary(env)")
    assert view < body.index("_contribution_profile_revision(")
    assert 'getattr(env, "HOST_BACKGROUND", None) is not None' in body
    assert body.count("await _contribution_view_put(view_key,") == 2


class WalletWarmHarness:
    def __init__(self, users):
        self.remembered = []
        harness = self
        body = json.dumps({"ok": True, "users": users})
        self.cache = {"users-world": FakeResponse(body)}

        async def edge_cache_match(key):
            return harness.cache.get(key)

        async def remember(_env, request, address):
            assert request.url.endswith(address)
            harness.remembered.append(address)
            if address.startswith("F"):
                raise RuntimeError("rpc down")

        self.last = {}
        self.scope = _load(["WORLD_WALLET_WARM_MAX", "_world_wallet_warm"], {
            "json": json, "edge_cache_match": edge_cache_match,
            "USERS_WORLD_CACHE_KEY": "users-world",
            "SOLANA_RE": re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$"),
            "_WORLD_WALLET_LAST": self.last,
            "_world_wallet_remember": remember,
            "JsRequest": SimpleNamespace(
                new=lambda url: SimpleNamespace(url=url, method="GET")),
            "WORLD_WALLET_CACHE_PREFIX": "https://internal/wallet?address=",
        })

    def warm(self):
        return asyncio.run(self.scope["_world_wallet_warm"](object()))


def test_the_host_warms_the_wallets_the_world_roster_publishes():
    other = "B" + WALLET[1:]
    failing = "F" + WALLET[1:]
    known = "C" + WALLET[1:]
    harness = WalletWarmHarness([
        {"name": "a", "solana": WALLET}, {"name": "b", "solana": ""},
        {"name": "c", "solana": WALLET}, {"name": "d", "solana": "bad"},
        {"name": "e", "solana": failing}, {"name": "f", "solana": other},
        {"name": "g", "solana": known}, "junk"])
    harness.last[known] = ("{}", 1)
    assert harness.warm() == 2
    assert harness.remembered == [WALLET, failing, other]


def test_the_wallet_warm_is_capped_and_needs_the_roster():
    users = [{"solana": "ABCDEFGHJKLMNPQRSTUVWXYZ"[i % 24] + WALLET[1:-2]
              + "%d%d" % (i // 9 + 1, i % 9 + 1)} for i in range(40)]
    harness = WalletWarmHarness(users)
    harness.warm()
    assert len(harness.remembered) == 32
    harness.cache.clear()
    assert harness.warm() == 0


def test_the_warm_entry_schedules_the_wallets_after_the_roster():
    warm = _function("host_warm_caches")
    assert warm.index("await _users_world_refresh(env)") < warm.index(
        "_world_wallet_warm(env)")
    assert '_host_background(env, "world-wallet-warm"' in warm


def test_the_world_roster_copy_outlives_an_idle_quarter_hour():
    stale = re.search(r"^USERS_WORLD_STALE_SECONDS = (.+)$", ENTRY_TEXT, re.M)
    assert eval(stale.group(1)) >= 30 * 60
    fresh = re.search(r"^USERS_WORLD_FRESH_MS = (.+)$", ENTRY_TEXT, re.M)
    assert eval(fresh.group(1)) == 30 * 1000
