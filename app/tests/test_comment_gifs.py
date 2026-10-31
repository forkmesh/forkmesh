"""The GIPHY proxy: the key stays server-side, results are rebuilt on GIPHY's
media host, searches are cached, rate limited and need a session, and the
settings are administrator-only."""

import asyncio
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import comment_gifs as gifs  # noqa: E402
import schema  # noqa: E402

KEY = "TESTKEY0123456789abcdef"


def bi(text):
    return hashlib.sha256(str(text).encode()).hexdigest()


def run(coro):
    return asyncio.run(coro)


class Upstream:
    def __init__(self):
        self.calls = []
        self.status = 200
        self.items = [
            {"id": "abc123XYZ", "title": "Thank   you  so much",
             "images": {"fixed_width": {"width": "200", "height": "150",
                                        "url": "https://evil.example/x.gif"}}},
            {"id": "../../etc", "title": "bad id"},
            {"id": "q1w2e3r4", "title": "", "images": {}},
        ]

    async def fetch(self, url, init, timeout):
        self.calls.append(url)
        body = json.dumps({"data": self.items,
                           "pagination": {"total_count": 100, "count": 3, "offset": 0}})
        return SimpleNamespace(status=self.status, text=lambda: _async(body))


async def _async(value):
    return value


@pytest.fixture
def rt(monkeypatch):
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    for statement in schema.SCHEMA_STATEMENTS:
        if re.search(r"instance_settings|comment_extras_rate", statement):
            db.execute(statement)
    clock = SimpleNamespace(now=10_000_000_000)
    upstream = Upstream()

    async def d1_all(env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def d1_first(env, sql, *args):
        rows = await d1_all(env, sql, *args)
        return rows[0] if rows else None

    async def d1_run(env, sql, *args):
        db.execute(sql, args)

    async def session(env, request, data=None):
        return (bi(request.account), {"name": request.account}) if request.account else ("", None)

    async def is_admin(env, name):
        return name == "alice"

    async def encrypt_row(env, obj):
        return "enc:" + json.dumps(obj)

    async def decrypt_row(env, stored):
        return json.loads(stored[4:])

    async def body(request, max_bytes=None):
        return request.body

    async def nothing(*_args, **_kwargs):
        return None

    def json_response(data, status=200, cache_control=None, extra_headers=None, **_):
        return SimpleNamespace(status=status, data=data, cache_control=cache_control,
                               headers=extra_headers or {})

    for name, value in {
        "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
        "_account_session_record": session, "_is_admin": is_admin,
        "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
        "bounded_json_request": body, "json_response": json_response,
        "ensure_schema": nothing, "_audit_sensitive_action": nothing,
        "js_fetch_with_timeout": upstream.fetch,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, n=240: value.strip()[:n] if isinstance(value, str) else "",
        "MAX_NODE_NAME": 63, "Date": SimpleNamespace(now=lambda: clock.now),
    }.items():
        monkeypatch.setattr(gifs, name, value, raising=False)
    gifs.forget()
    return SimpleNamespace(db=db, clock=clock, upstream=upstream)


def call(rest, method="GET", account="", query="", body=None):
    request = SimpleNamespace(method=method, account=account, body=body,
                              url="https://forge.test/api/gifs/%s%s" % (rest, query))
    return run(gifs.handler(SimpleNamespace(), request, rest))


def configure(rt, key=KEY, rating="pg"):
    reply = call("settings", "POST", "alice", body={"key": key, "rating": rating})
    assert reply.status == 200
    return reply


def test_no_key_means_no_gif_button(rt):
    config = call("config")
    assert config.data == {"ok": True, "enabled": False, "rating": "pg",
                           "attribution": "Powered by GIPHY"}
    assert config.cache_control.startswith("public")
    assert call("trending", account="bob").status == 404
    assert rt.upstream.calls == []


def test_settings_are_admin_only_and_never_echo_the_key(rt):
    assert call("settings", "POST", "bob", body={"key": KEY}).status == 403
    assert call("settings", "POST", "", body={"key": KEY}).status == 401
    assert call("settings", "POST", "alice", body={"key": "bad key!"}).data["error"] == "invalid_key"
    assert call("settings", "POST", "alice", body={"rating": "nc-17"}).data["error"] == "invalid_rating"
    saved = configure(rt, rating="g")
    assert KEY not in json.dumps(saved.data)
    stored = rt.db.execute("SELECT doc FROM instance_settings WHERE k='giphy'").fetchone()[0]
    # Sealed with encrypt_row (the fake here marks it "enc:"), never stored bare.
    assert json.loads(stored)["key"].startswith("enc:") and json.loads(stored)["rating"] == "g"
    read = call("settings", account="alice")
    assert read.data["configured"] is True and KEY not in json.dumps(read.data)
    assert call("settings", account="bob").status == 403
    config = call("config")
    assert config.data["enabled"] is True and KEY not in json.dumps(config.data)
    cleared = call("settings", "POST", "alice", body={"clear": True, "rating": "g"})
    assert cleared.data["configured"] is False
    assert call("config").data["enabled"] is False


def test_searches_use_the_key_server_side_and_answer_canonical_media_urls(rt):
    configure(rt)
    assert call("search", account="", query="?q=thanks").status == 401
    reply = call("search", account="bob", query="?q=thank%20you&offset=24")
    assert reply.status == 200
    sent = urlparse(rt.upstream.calls[-1])
    params = parse_qs(sent.query)
    assert sent.netloc == "api.giphy.com" and sent.path == "/v1/gifs/search"
    assert params["api_key"] == [KEY] and params["rating"] == ["pg"]
    assert params["q"] == ["thank you"] and params["offset"] == ["24"]
    assert KEY not in json.dumps(reply.data)
    assert reply.data["attribution"] == "Powered by GIPHY"
    assert [gif["id"] for gif in reply.data["gifs"]] == ["abc123XYZ", "q1w2e3r4"]
    first = reply.data["gifs"][0]
    assert first == {"id": "abc123XYZ", "title": "Thank you so much",
                     "url": "https://media.giphy.com/media/abc123XYZ/200.gif",
                     "preview": "https://media.giphy.com/media/abc123XYZ/200w.gif",
                     "still": "https://media.giphy.com/media/abc123XYZ/200w_s.gif",
                     "width": 200, "height": 150}
    assert reply.data["gifs"][1]["title"] == "GIF"
    for gif in reply.data["gifs"]:
        for key in ("url", "preview"):
            assert gifs.GIF_URL_RE.fullmatch(gif[key])
    assert reply.data["next"] == 48
    assert call("search", account="bob", query="?q=").data["error"] == "query_required"


def test_results_are_cached_and_upstream_failures_are_reported(rt):
    configure(rt)
    call("trending", account="bob")
    call("trending", account="carol")
    assert len(rt.upstream.calls) == 1
    rt.clock.now += gifs.TRENDING_SECONDS * 1000 + 1
    call("trending", account="bob")
    assert len(rt.upstream.calls) == 2
    rt.upstream.status = 500
    assert call("search", account="bob", query="?q=nope").status == 502


def test_searches_are_rate_limited_per_account(rt, monkeypatch):
    configure(rt)
    monkeypatch.setattr(gifs, "RATE_MAX_PER_WINDOW", 2)
    assert call("search", account="bob", query="?q=a").status == 200
    assert call("search", account="bob", query="?q=b").status == 200
    limited = call("search", account="bob", query="?q=c")
    assert limited.status == 429 and limited.headers["Retry-After"]
    # A cached answer costs nothing.
    assert call("search", account="bob", query="?q=a").status == 200
    assert call("search", account="carol", query="?q=c").status == 200


def test_only_giphy_media_hosts_count_as_gifs():
    good = ["https://media.giphy.com/media/abc123/giphy.gif",
            "https://media2.giphy.com/media/abc123/200w.gif",
            "https://media0.giphy.com/media/v1.Y2lkPTc5MGI3NjEx/abc123/giphy-downsized.gif"]
    bad = ["http://media.giphy.com/media/abc123/giphy.gif",
           "https://media.giphy.com.evil.example/media/abc123/giphy.gif",
           "https://evil.example/media.giphy.com/media/abc123/giphy.gif",
           "https://media.giphy.com/media/abc123/giphy.gif?x=1",
           "https://media9.giphy.com/media/abc123/giphy.gif",
           "https://i.giphy.com/abc123.gif",
           "https://media.giphy.com/media/../abc/giphy.gif"]
    assert all(gifs.GIF_URL_RE.fullmatch(url) for url in good)
    assert not any(gifs.GIF_URL_RE.fullmatch(url) for url in bad)
    assert gifs.gif_still(good[0]) == "https://media.giphy.com/media/abc123/giphy_s.gif"
    assert gifs.gif_still(bad[1]) == ""
    sys.path.insert(0, str(ROOT / "host"))
    from forkmesh_host import repo_ssr_issue
    js = (ROOT / "public/dashboard/js/06-repo-content.js").read_text("utf-8")
    pattern = re.search(r"const MD_GIPHY_GIF = /(.+)/;", js).group(1).replace("\\/", "/")
    for url in good + bad:
        expected = bool(gifs.GIF_URL_RE.fullmatch(url))
        assert bool(repo_ssr_issue.GIPHY_GIF_RE.match(url)) == expected, url
        assert bool(re.fullmatch(pattern.strip("^$"), url)) == expected, url
