from __future__ import annotations

import asyncio
import json
import time

from forkmesh_host import js as _js
from forkmesh_host import request_guard
from forkmesh_host.asgi import (
    REQUEST_CHECK_PATH, HostASGI, WorkerFirstRules)
from forkmesh_host.request_policy import (
    CrawlerVerifier, RateLimiter, RequestPolicy, Rules)

ROWS = [
    {"rule_id": "a", "match_type": "user_agent_keyword", "pattern": "BadBot"},
    {"rule_id": "b", "match_type": "user_agent_keyword", "pattern": "spider"},
    {"rule_id": "c", "match_type": "ip", "pattern": "198.51.100.7"},
    {"rule_id": "d", "match_type": "cidr", "pattern": "203.0.113.0/24"},
    {"rule_id": "e", "match_type": "ip_range",
     "pattern": "192.0.2.10 - 192.0.2.20"},
    {"rule_id": "f", "match_type": "cidr", "pattern": "2001:db8::/32"},
    {"rule_id": "g", "match_type": "ip", "pattern": "not-an-ip"},
]
BROWSER = "Mozilla/5.0 (X11; Linux x86_64) Firefox/140.0"


class _Statement:
    def __init__(self, db, sql):
        self.db = db
        self.sql = sql

    async def all(self):
        self.db.queries.append(self.sql)
        if self.db.fail:
            raise RuntimeError("D1_ERROR: D1 DB is overloaded")
        return _js.JsObject(results=[dict(row) for row in self.db.rows])


class _DB:
    def __init__(self, rows):
        self.rows = rows
        self.queries = []
        self.fail = False

    def prepare(self, sql):
        return _Statement(self, sql)


class _Env:
    FORKMESH_PLATFORM = "vps"
    APP_ORIGIN = "https://app.next.forkmesh.com"
    WORLD_ORIGIN = "https://world.next.forkmesh.com"
    WWW_EDGE_SECRET = "edge-secret-for-tests"

    def __init__(self, rows=()):
        self.DB = _DB(list(rows))


class _Runtime:
    def __init__(self, env):
        self.env = env
        self.sealed = False
        self.durable = None
        self.seen = []

    async def fetch(self, request):
        self.seen.append(request)
        return _js.Response(b"ok", 200, "",
                            _js.Headers({"content-type": "text/plain"}))


class _Verifier:
    def __init__(self, good=()):
        self.good = set(good)
        self.asked = []

    async def verified(self, ip):
        self.asked.append(ip)
        return ip in self.good


def _app(rows=ROWS, monkeypatch=None, limiter=None, verifier=None):
    if monkeypatch is not None:
        monkeypatch.delenv("FORKMESH_PUBLIC_HOSTS", raising=False)
    runtime = _Runtime(_Env(rows))
    app = HostASGI(runtime, serve_assets=False,
                   worker_first=WorkerFirstRules([]))
    app.request_policy = RequestPolicy(
        limiter=limiter or RateLimiter(requests=1000, period=10, timeout=10),
        verifier=verifier or _Verifier())
    return app, runtime


def _scope(path="/api/x", ip="192.0.2.99", agent=BROWSER, headers=(),
           client=("127.0.0.1", 50000)):
    raw = [(b"host", b"app.next.forkmesh.com")]
    if agent is not None:
        raw.append((b"user-agent", agent.encode()))
    if ip is not None:
        raw.append((b"cf-connecting-ip", ip.encode()))
    raw += [(k.encode(), v.encode()) for k, v in headers]
    return {"type": "http", "method": "GET", "path": path,
            "raw_path": path.encode(), "query_string": b"", "headers": raw,
            "client": client, "scheme": "http", "http_version": "1.1"}


def _call(app, scope):
    sent = []

    async def receive():
        return {"type": "http.request", "body": b"", "more_body": False}

    async def send(message):
        sent.append(message)

    async def run():
        await app(scope, receive, send)
        await app.request_policy.close()

    asyncio.run(run())
    headers = dict(sent[0].get("headers") or [])
    return sent[0]["status"], headers


def test_rules_compile_every_match_type_and_skip_bad_rows():
    rules = Rules.from_rows(ROWS)
    assert rules.keywords == ("badbot", "spider")
    assert len(rules) == 6
    ip = request_guard.ipaddress.ip_address
    assert rules.address_blocked(ip("198.51.100.7"))
    assert rules.address_blocked(ip("203.0.113.200"))
    assert rules.address_blocked(ip("192.0.2.10"))
    assert rules.address_blocked(ip("192.0.2.20"))
    assert not rules.address_blocked(ip("192.0.2.21"))
    assert rules.address_blocked(ip("2001:db8::1"))
    assert not rules.address_blocked(ip("2001:db9::1"))
    assert rules.agent_blocked("Mozilla/5.0 (compatible; BADBOT/1.0)")
    assert not rules.agent_blocked(BROWSER)


def test_browser_passes_and_rules_come_from_the_table(monkeypatch):
    app, runtime = _app(monkeypatch=monkeypatch)
    status, _ = _call(app, _scope())
    assert status == 200 and len(runtime.seen) == 1
    assert runtime.env.DB.queries
    assert "FROM request_block_rules" in runtime.env.DB.queries[0]
    assert "enabled=1" in runtime.env.DB.queries[0]


def test_blocked_user_agent_and_addresses_get_403(monkeypatch):
    app, runtime = _app(monkeypatch=monkeypatch)
    for scope in (_scope(agent="Mozilla/5.0 (compatible; Baiduspider/2.0)"),
                  _scope(agent="badbot"),
                  _scope(ip="198.51.100.7"),
                  _scope(ip="203.0.113.9"),
                  _scope(ip="192.0.2.15"),
                  _scope(ip="2001:db8::5")):
        status, _ = _call(app, scope)
        assert status == 403
    assert runtime.seen == []
    assert app.request_policy.stats["blocked"] == 6


def test_blocked_rules_cover_pages_not_only_api(monkeypatch):
    app, runtime = _app(monkeypatch=monkeypatch)
    for path in ("/", "/forkmesh/forkmesh", "/robots.txt", "/sitemap.xml"):
        status, _ = _call(app, _scope(path, agent="spider"))
        assert status == 403
    assert runtime.seen == []


def test_health_and_fleet_paths_are_never_blocked(monkeypatch):
    app, _ = _app(monkeypatch=monkeypatch)
    assert app.request_policy.exempt("/readyz")
    assert app.request_policy.exempt("/health")
    assert app.request_policy.exempt("/api/fleet/status")
    status, _ = _call(app, _scope("/health", agent="spider"))
    assert status == 200


def test_agent_guestbook_and_notes_get_keyword_rules_too(monkeypatch):
    app, runtime = _app(monkeypatch=monkeypatch)
    agent = "Mozilla/5.0 (compatible; Zxq-User/1.0; BadBot; +https://example.test/x)"
    for path in ("/api/chat/agent-guestbook", "/api/chat/agent-guestbook/",
                 "/api/agent-notes", "/api/agent-notes/"):
        status, _ = _call(app, _scope(path, agent=agent))
        assert status == 403
    assert runtime.seen == []
    status, _ = _call(app, _scope("/api/agent-notes"))
    assert status == 200


def test_chatgpt_agents_pass_keyword_rules_on_every_path(monkeypatch):
    rows = ROWS + [{"rule_id": "h", "match_type": "user_agent_keyword",
                    "pattern": "bot"}]
    app, runtime = _app(rows=rows, monkeypatch=monkeypatch)
    agents = ("Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko); compatible;"
              " ChatGPT-User/1.0; +https://openai.com/bot",
              "Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko); compatible;"
              " OAI-SearchBot/1.0; +https://openai.com/searchbot")
    for agent in agents:
        for path in ("/", "/login", "/api/chat/agent-guestbook", "/api/x"):
            status, _ = _call(app, _scope(path, agent=agent))
            assert status == 200, (agent, path)
    assert len(runtime.seen) == 8
    status, _ = _call(app, _scope(agent="GPTBot/1.1; +https://openai.com/gptbot"))
    assert status == 403
    status, _ = _call(app, _scope(ip="198.51.100.7", agent=agents[0]))
    assert status == 403


def _check_scope(uri="/login", agent=BROWSER, ip="192.0.2.99", headers=(),
                 client=("127.0.0.1", 50000), method="GET"):
    extra = [("x-forwarded-uri", uri), ("x-forwarded-method", method)]
    return _scope(REQUEST_CHECK_PATH, ip=ip, agent=agent,
                  headers=extra + list(headers), client=client)


def test_caddy_check_endpoint_answers_the_rule_verdict(monkeypatch):
    verifier = _Verifier(good={"66.249.66.1"})
    app, runtime = _app(monkeypatch=monkeypatch, verifier=verifier)
    agent = "Mozilla/5.0 (compatible; Googlebot/2.1; spider)"
    assert _call(app, _check_scope(ip="66.249.66.1", agent=agent))[0] == 204
    assert _call(app, _check_scope(ip="192.0.2.99", agent=agent))[0] == 403
    assert _call(app, _check_scope(agent="badbot"))[0] == 403
    assert _call(app, _check_scope(ip="198.51.100.7"))[0] == 403
    assert _call(app, _check_scope())[0] == 204
    assert _call(app, _check_scope(uri="/health", agent="badbot"))[0] == 204
    assert _call(app, _check_scope(uri="/api/fleet/x?y=1",
                                   agent="badbot"))[0] == 204
    assert _call(app, _check_scope(uri="/api/agent-notes",
                                   agent="badbot"))[0] == 403
    assert runtime.seen == []
    assert app.request_policy.stats["blocked"] == 4
    assert app.request_policy.limiter._clients == {}


def test_caddy_check_endpoint_is_loopback_only(monkeypatch):
    app, runtime = _app(monkeypatch=monkeypatch)
    status, _ = _call(app, _check_scope(agent="badbot",
                                        client=("203.0.113.50", 4000)))
    assert status == 403
    status, _ = _call(app, _check_scope(client=("192.0.2.1", 4000)))
    assert status == 200
    assert runtime.seen[-1].url.endswith(REQUEST_CHECK_PATH)


def test_caddy_check_reads_the_signed_visitor_without_using_it_up(monkeypatch):
    verifier = _Verifier(good={"66.249.66.1"})
    app, runtime = _app(monkeypatch=monkeypatch, verifier=verifier)
    agent = "Mozilla/5.0 (compatible; Googlebot/2.1; spider)"
    value = request_guard.edge_sign(
        _Env.WWW_EDGE_SECRET, "66.249.66.1", "US", int(time.time()), "GET",
        "/dashboard.js")
    edge = [(request_guard.EDGE_HEADER, value)]
    status, _ = _call(app, _check_scope(uri="/dashboard.js?v=1", agent=agent,
                                        ip="104.16.0.1", headers=edge))
    assert status == 204
    assert verifier.asked == ["66.249.66.1"]
    assert app.edge_verifier.accepted == 0
    scope = _scope("/dashboard.js", ip="104.16.0.1", agent=agent, headers=edge)
    assert _call(app, scope)[0] == 200
    assert runtime.seen[-1].headers.get("cf-connecting-ip") == "66.249.66.1"
    assert app.edge_verifier.accepted == 1


def test_loaded_rules_are_exported_for_caddy_only_when_they_change(tmp_path):
    target = tmp_path / "request-rules.json"
    env = _Env(ROWS)
    policy = RequestPolicy(verifier=_Verifier(), export_path=str(target))
    assert asyncio.run(policy.load(env)) is True
    data = json.loads(target.read_text())
    assert data == {
        "version": 1, "agents": ["badbot", "spider"],
        "networks": ["198.51.100.7/32", "2001:db8::/32", "203.0.113.0/24"],
        "ranges": [["192.0.2.10", "192.0.2.20"]]}
    stamp = target.stat().st_mtime_ns
    assert asyncio.run(policy.load(env)) is True
    assert target.stat().st_mtime_ns == stamp
    assert policy.stats["exports"] == 1
    env.DB.fail = True
    assert asyncio.run(policy.load(env)) is False
    assert json.loads(target.read_text())["agents"] == ["badbot", "spider"]
    env.DB.fail = False
    env.DB.rows = []
    assert asyncio.run(policy.load(env)) is True
    assert json.loads(target.read_text())["agents"] == []
    assert policy.stats["exports"] == 2
    assert not list(tmp_path.glob("*.tmp"))


def test_export_path_comes_from_the_environment(monkeypatch, tmp_path):
    monkeypatch.setenv("FORKMESH_REQUEST_RULES_FILE", str(tmp_path / "r.json"))
    assert RequestPolicy().export_path == str(tmp_path / "r.json")
    monkeypatch.delenv("FORKMESH_REQUEST_RULES_FILE")
    policy = RequestPolicy()
    assert policy.export_path == ""
    assert asyncio.run(policy.load(_Env(ROWS))) is True


def test_export_failure_is_counted_not_raised(tmp_path):
    policy = RequestPolicy(verifier=_Verifier(),
                           export_path=str(tmp_path / "missing" / "r.json"))
    assert asyncio.run(policy.load(_Env(ROWS))) is True
    assert policy.stats["export_failures"] == 1
    assert policy.rules.agent_blocked("spider")


def test_google_named_agent_passes_only_when_verified(monkeypatch):
    verifier = _Verifier(good={"66.249.66.1"})
    app, runtime = _app(monkeypatch=monkeypatch, verifier=verifier)
    agent = "Mozilla/5.0 (compatible; Googlebot/2.1; spider)"
    status, _ = _call(app, _scope(ip="66.249.66.1", agent=agent))
    assert status == 200
    status, _ = _call(app, _scope(ip="192.0.2.99", agent=agent))
    assert status == 403
    assert verifier.asked == ["66.249.66.1", "192.0.2.99"]
    status, _ = _call(app, _scope(ip="198.51.100.7", agent=agent))
    assert status == 403


def test_forward_confirmed_lookup_is_cached_and_bounded():
    calls = []

    def lookup(ip):
        calls.append(ip)
        return ip == "66.249.66.1"

    verifier = CrawlerVerifier(lookup=lookup)

    async def run():
        return [await verifier.verified("66.249.66.1"),
                await verifier.verified("66.249.66.1"),
                await verifier.verified("192.0.2.1")]

    assert asyncio.run(run()) == [True, True, False]
    assert calls == ["66.249.66.1", "192.0.2.1"]

    def slow(ip):
        time.sleep(0.3)
        return True

    slow_verifier = CrawlerVerifier(lookup=slow, timeout=0.05)
    assert asyncio.run(slow_verifier.verified("66.249.66.1")) is False


def test_rate_limit_answers_429_with_retry_after(monkeypatch):
    now = [1000.0]
    limiter = RateLimiter(requests=3, period=10, timeout=30,
                          clock=lambda: now[0])
    app, runtime = _app(monkeypatch=monkeypatch, limiter=limiter)
    statuses = [_call(app, _scope(ip="192.0.2.50"))[0] for _ in range(4)]
    assert statuses == [200, 200, 200, 429]
    status, headers = _call(app, _scope(ip="192.0.2.50"))
    assert status == 429 and headers[b"retry-after"] == b"30"
    assert _call(app, _scope(ip="192.0.2.51"))[0] == 200
    now[0] += 31
    assert _call(app, _scope(ip="192.0.2.50"))[0] == 200
    assert len(runtime.seen) == 5
    assert app.request_policy.stats["rate_limited"] == 2


def test_rate_limit_window_resets_and_zero_disables():
    now = [0.0]
    limiter = RateLimiter(requests=2, period=10, timeout=0,
                          clock=lambda: now[0])
    assert [limiter.retry_after("a") for _ in range(3)] == [0, 0, 10]
    now[0] = 10.0
    assert limiter.retry_after("a") == 0
    off = RateLimiter(requests=0, period=10, timeout=10)
    assert not off.enabled
    assert all(off.retry_after("a") == 0 for _ in range(1000))


def test_rate_limit_settings_come_from_the_environment(monkeypatch):
    monkeypatch.setenv("FORKMESH_RATE_LIMIT_REQUESTS", "7")
    monkeypatch.setenv("FORKMESH_RATE_LIMIT_PERIOD_SECONDS", "30")
    monkeypatch.setenv("FORKMESH_RATE_LIMIT_TIMEOUT_SECONDS", "60")
    limiter = RateLimiter()
    assert (limiter.requests, limiter.period, limiter.timeout) == (7, 30, 60)
    monkeypatch.setenv("FORKMESH_RATE_LIMIT_REQUESTS", "junk")
    assert RateLimiter().requests == 240


def test_rate_limiter_memory_is_bounded():
    now = [0.0]
    limiter = RateLimiter(requests=5, period=10, timeout=10,
                          clock=lambda: now[0], max_clients=100)
    for index in range(1000):
        limiter.retry_after("client-%d" % index)
        now[0] += 0.5
    assert len(limiter._clients) <= 100


def test_local_tools_without_a_visitor_are_not_rate_limited(monkeypatch):
    limiter = RateLimiter(requests=1, period=10, timeout=10)
    app, _ = _app(monkeypatch=monkeypatch, limiter=limiter)
    statuses = [_call(app, _scope(ip=None))[0] for _ in range(3)]
    assert statuses == [200, 200, 200]


def test_signed_edge_visitor_is_the_key_and_header_verifies_once(monkeypatch):
    limiter = RateLimiter(requests=1, period=10, timeout=10)
    app, runtime = _app(monkeypatch=monkeypatch, limiter=limiter)

    stamps = iter(range(int(time.time()), 0, -1))

    def relayed(visitor):
        value = request_guard.edge_sign(
            _Env.WWW_EDGE_SECRET, visitor, "US", next(stamps), "GET",
            "/api/x")
        return _scope(ip="104.16.0.1",
                      headers=[(request_guard.EDGE_HEADER, value)])

    assert _call(app, relayed("192.0.2.60"))[0] == 200
    assert runtime.seen[-1].headers.get("cf-connecting-ip") == "192.0.2.60"
    assert runtime.seen[-1].headers.get(request_guard.EDGE_HEADER) is None
    assert _call(app, relayed("192.0.2.61"))[0] == 200
    assert _call(app, relayed("192.0.2.60"))[0] == 429
    assert app.edge_verifier.rejected == 0
    status, _ = _call(app, relayed("198.51.100.7"))
    assert status == 403


def test_table_outage_keeps_the_last_rules(monkeypatch):
    app, runtime = _app(monkeypatch=monkeypatch)
    policy = app.request_policy
    assert _call(app, _scope(agent="spider"))[0] == 403
    runtime.env.DB.fail = True
    assert asyncio.run(policy.load(runtime.env)) is False
    assert policy.stats["load_failures"] == 1
    assert _call(app, _scope(agent="spider"))[0] == 403
    runtime.env.DB.fail = False
    runtime.env.DB.rows = []
    assert asyncio.run(policy.load(runtime.env)) is True
    assert _call(app, _scope(agent="spider"))[0] == 200


def test_rules_refresh_in_the_background_after_the_interval():
    now = [0.0]
    env = _Env(ROWS)
    policy = RequestPolicy(verifier=_Verifier(), clock=lambda: now[0],
                           refresh=60)

    async def run():
        first = await policy.blocked(env, "192.0.2.1", "spider")
        env.DB.rows = []
        now[0] = 30
        cached = await policy.blocked(env, "192.0.2.1", "spider")
        now[0] = 61
        await policy.blocked(env, "192.0.2.1", "spider")
        await asyncio.sleep(0)
        await asyncio.sleep(0)
        refreshed = await policy.blocked(env, "192.0.2.1", "spider")
        await policy.close()
        return first, cached, refreshed

    assert asyncio.run(run()) == (True, True, False)
    assert len(env.DB.queries) == 2


def test_websocket_handshake_is_refused_for_blocked_visitors(monkeypatch):
    app, runtime = _app(monkeypatch=monkeypatch)
    scope = _scope("/api/ws", agent="spider")
    scope["type"] = "websocket"
    scope["extensions"] = {"websocket.http.response": {}}
    sent = []
    messages = iter([{"type": "websocket.connect"}])

    async def receive():
        return next(messages)

    async def send(message):
        sent.append(message)

    async def run():
        await app(scope, receive, send)
        await app.request_policy.close()

    asyncio.run(run())
    assert sent[0]["type"] == "websocket.http.response.start"
    assert sent[0]["status"] == 403
    assert runtime.seen == []
