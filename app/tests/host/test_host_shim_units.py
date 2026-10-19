"""Unit tests for the CPython host shims (js, pyodide.ffi, workers, bindings)."""

from __future__ import annotations

import asyncio
import json
import math

import pytest

from forkmesh_host import js, workers
from forkmesh_host.bindings import LocalKV, LocalQueue, StaticAssets
from forkmesh_host.d1_sqlite import D1Sqlite, apply_migrations
from forkmesh_host.durable import LocalDurableHost
from forkmesh_host.ffi import JsArray, JsObject, jsnull, to_js


def run(coro):
    return asyncio.run(coro)


def test_top_level_shims_resolve_to_host_modules():
    import js as top_js
    import pyodide.ffi
    import workers as top_workers
    assert top_js is js
    assert top_workers is workers
    assert pyodide.ffi.jsnull is jsnull
    assert type(jsnull).__name__ == "JsNull"
    assert not jsnull


def test_to_js_with_object_from_entries_gives_attribute_objects():
    value = to_js({"a": {"b": 1}, "c": [1, {"d": 2}]},
                  dict_converter=js.Object.fromEntries)
    assert value.a.b == 1 and value.c[1].d == 2
    assert value.to_py() == {"a": {"b": 1}, "c": [1, {"d": 2}]}
    plain = to_js({"x": 1})
    assert plain == {"x": 1}
    assert isinstance(to_js(b"ab"), js.Uint8Array)
    assert isinstance(to_js(["a"]), JsArray)


def test_uint8array_copy_view_and_to_py():
    data = js.Uint8Array.new(to_js(b"\x01\x02\x03"))
    assert bytes(data.to_py()) == b"\x01\x02\x03"
    assert data[0] == 1 and data[5] is None and len(data) == 3
    view = js.Uint8Array.new(js.ArrayBuffer(b"abcd"))
    view[0] = ord("z")
    assert view.to_bytes() == b"zbcd"
    copy = js.Uint8Array.new(view)
    copy[1] = ord("y")
    assert view.to_bytes() == b"zbcd" and copy.to_bytes() == b"zycd"
    assert js.Uint8Array.new(4).to_bytes() == b"\0\0\0\0"
    assert view.subarray(1, 3).to_bytes() == b"bc"


def test_date_json_object():
    assert isinstance(js.Date.now(), int)
    assert js.Date.parse("2026-01-02T03:04:05.678Z") == 1767323045678
    assert js.Date.parse("2026-01-02T03:04:05+01:00") == 1767319445000
    assert math.isnan(js.Date.parse("not a date"))
    assert js.JSON.stringify({"a": [1, "x"]}) == '{"a":[1,"x"]}'
    parsed = js.JSON.parse('{"a":{"b":[1]}}')
    assert parsed.a.b == [1] and parsed.to_py() == {"a": {"b": [1]}}
    assert list(js.Object.keys({"k": 1})) == ["k"]


def test_url_parts():
    url = js.URL.new("https://User:pw@Example.com:443/a/b?x=1&y=2#frag")
    assert url.origin == "https://example.com"
    assert url.pathname == "/a/b" and url.search == "?x=1&y=2"
    assert url.hash == "#frag" and url.port == "" and url.username == "User"
    assert url.searchParams.get("y") == "2"
    other = js.URL.new("http://h:8080")
    assert other.pathname == "/" and other.host == "h:8080"
    assert js.URL.new("/p?q", "https://b.test/x").href == "https://b.test/p?q"
    with pytest.raises(js.JsException):
        js.URL.new("nope")


def test_headers_case_and_set_cookie():
    headers = js.Headers({"Content-Type": "text/plain"})
    headers.append("Set-Cookie", "a=1")
    headers.append("set-cookie", "b=2")
    headers.append("X-Multi", "1")
    headers.append("x-multi", "2")
    assert headers.get("content-type") == "text/plain"
    assert headers.get("x-multi") == "1, 2"
    assert list(headers.getSetCookie()) == ["a=1", "b=2"]
    items = dict((k, v) for k, v in headers.items() if k != "set-cookie")
    assert items == {"content-type": "text/plain", "x-multi": "1, 2"}
    assert ("set-cookie", "a=1") in headers.raw()
    headers.delete("X-MULTI")
    assert not headers.has("x-multi")


def test_response_bodies_clone_and_workers_wrapper():
    async def scenario():
        response = js.Response.new("héllo", {"status": 201, "headers": {
            "x-a": "1"}})
        assert response.headers.get("content-type").startswith("text/plain")
        clone = response.clone()
        assert await clone.text() == "héllo"
        assert bytes((await response.arrayBuffer()).to_py()) == \
            "héllo".encode()

        async def chunks():
            yield b"ab"
            yield b"cd"
        streamed = js.Response.new(js.ReadableStream(chunks()))
        twin = streamed.clone()
        assert await streamed.text() == "abcd"
        assert await twin.text() == "abcd"

        wrapped = workers.Response(js.Response.new("x", {"status": 404}))
        assert wrapped.status == 404 and wrapped.js_object is wrapped
        built = workers.Response("{}", status=200, status_text="OK",
                                 headers=[("content-type", "a/b")])
        assert dict(built.headers.items())["content-type"] == "a/b"
        assert (await built.json()) == {}
        passthrough = js.Response.new(built.body, {"status": 200})
        assert await passthrough.text() == "{}"
        reader = js.Response.new("xyz").body.getReader()
        first = await reader.read()
        assert first.done is False and first.value.to_bytes() == b"xyz"
        assert (await reader.read()).done is True
        with pytest.raises(js.JsException):
            js.Response.new(None, {"status": 101})
        pair = js.WebSocketPair.new()
        client, _server = pair.object_values()
        upgrade = js.Response.new(None, {"status": 101, "webSocket": client})
        assert upgrade.webSocket is client
    run(scenario())


def test_request_new_copies_and_rejects_get_body():
    request = js.Request.new("https://a.test/x", to_js(
        {"method": "POST", "headers": {"a": "1"}, "body": "data"},
        dict_converter=js.Object.fromEntries))
    forwarded = js.Request.new(request)
    forwarded.headers.set("x-forkmesh-internal-ingress", "v1")
    assert request.headers.get("x-forkmesh-internal-ingress") is None
    assert run(forwarded.text()) == "data"
    with pytest.raises(js.JsException):
        js.Request.new("https://a.test/", {"method": "GET", "body": "x"})


def test_cache_api_honours_max_age_and_byte_cap():
    async def scenario():
        cache = js._Cache(max_bytes=4096)
        await cache.put("https://c.test/a", js.Response.new("A", {
            "headers": {"cache-control": "public, max-age=60"}}))
        await cache.put("https://c.test/nostore", js.Response.new("B", {
            "headers": {"cache-control": "no-cache, max-age=0"}}))
        hit = await cache.match("https://c.test/a")
        assert hit is not None and await hit.text() == "A"
        assert await cache.match("https://c.test/nostore") is None
        for i in range(20):
            await cache.put("https://c.test/big%d" % i, js.Response.new(
                "x" * 900, {"headers": {"cache-control": "max-age=60"}}))
        assert cache.size_bytes <= 4096
        assert await cache.match("https://c.test/a") is None  # evicted
        assert await cache.delete("https://c.test/big19") is True
    run(scenario())


def test_abort_signal_timeout_and_controller_bind():
    async def scenario():
        signal = js.AbortSignal.timeout(20)
        assert not signal.aborted
        await asyncio.sleep(0.03)
        assert signal.aborted
        controller = js.AbortController.new()
        timer = js.setTimeout(controller.abort.bind(controller), 10)
        assert isinstance(timer, int)
        await asyncio.sleep(0.03)
        assert controller.signal.aborted
        other = js.AbortController.new()
        cancelled = js.setTimeout(other.abort.bind(other), 10)
        js.clearTimeout(cancelled)
        await js.scheduler.wait(20)
        assert not other.signal.aborted
    run(scenario())


def test_fetch_can_be_disabled(monkeypatch):
    monkeypatch.setenv("FORKMESH_HOST_OFFLINE", "1")
    with pytest.raises(js.JsException):
        run(js.fetch("https://example.invalid/"))


def test_d1_sqlite_shape(tmp_path):
    async def scenario():
        db = D1Sqlite(str(tmp_path / "d1.sqlite"))
        await db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
        ran = await db.prepare("INSERT INTO t (v) VALUES (?)").bind(
            "a").run()
        assert ran.meta.changes == 1 and ran.meta.last_row_id == 1
        await db.prepare("INSERT INTO t (v) VALUES (?)").bind(jsnull).run()
        rows = (await db.prepare("SELECT * FROM t ORDER BY id").all()).results
        assert rows == [{"id": 1, "v": "a"}, {"id": 2, "v": None}]
        assert await db.prepare("SELECT v FROM t WHERE id=?").bind(1).first(
            "v") == "a"
        assert await db.prepare("SELECT v FROM t WHERE id=9").first() is None
        with pytest.raises(Exception, match="D1_ERROR"):
            await db.batch([
                db.prepare("INSERT INTO t (v) VALUES ('b')"),
                db.prepare("INSERT INTO nope VALUES (1)"),
            ])
        count = await db.withSession("first-primary").prepare(
            "SELECT count(*) AS n FROM t").first("n")
        assert count == 2  # the failed batch rolled back
    run(scenario())


def test_all_migrations_apply_to_a_fresh_database():
    from forkmesh_host import paths
    db = D1Sqlite(":memory:")
    applied = apply_migrations(db, paths.APP_DIR / "migrations")
    assert len(applied) >= 190
    assert apply_migrations(db, paths.APP_DIR / "migrations") == []


def test_kv_expiration_and_types(tmp_path):
    async def scenario():
        kv = LocalKV(tmp_path / "kv.sqlite")
        await kv.put("a", json.dumps({"x": 1}))
        assert await kv.get("a") == '{"x": 1}'
        assert (await kv.get("a", "json")).x == 1
        await kv.put("gone", "v", {"expirationTtl": 60})
        kv._conn.execute("UPDATE kv SET expires=1 WHERE k='gone'")
        assert await kv.get("gone") is None
        await kv.delete("a")
        assert await kv.get("a") is None
    run(scenario())


def test_queue_batches_and_retries():
    async def scenario():
        queue = LocalQueue("q", max_batch_size=2, max_batch_timeout=0.01,
                           max_retries=1)
        seen = []

        async def consumer(batch):
            seen.append([m.body for m in batch.messages])
            for message in batch.messages:
                if message.body == "retry-me" and message.attempts == 1:
                    message.retry()
        queue.consumer = consumer
        await queue.send("a")
        await queue.sendBatch([{"body": "b"}, {"body": "retry-me"}])
        await asyncio.sleep(0.05)
        await queue.flush()
        await asyncio.sleep(1.1)
        await queue.flush()
        flat = [body for batch in seen for body in batch]
        assert flat.count("retry-me") == 2 and "a" in flat and "b" in flat
        assert max(len(batch) for batch in seen) <= 2
    run(scenario())


def test_static_assets_html_handling(tmp_path):
    (tmp_path / "index.html").write_text("home")
    (tmp_path / "about.html").write_text("about")
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "index.html").write_text("docs")
    (tmp_path / "404.html").write_text("missing")
    (tmp_path / "app.js").write_text("js")
    assets = StaticAssets(tmp_path, "drop-trailing-slash", "404-page")
    assert assets.resolve("/about") == ("file", tmp_path / "about.html")
    assert assets.resolve("/about.html") == ("redirect", "/about")
    assert assets.resolve("/docs/") == ("redirect", "/docs")
    assert assets.resolve("/docs") == ("file", tmp_path / "docs" /
                                       "index.html")
    assert assets.resolve("/") == ("file", tmp_path / "index.html")
    assert assets.resolve("/../etc/passwd") == (None, None)
    missing = assets.serve("/nope")
    assert missing.status == 404 and missing._body == b"missing"
    # The binding (env.ASSETS.fetch) serves exact documents without the
    # navigation redirects: the app reads "index.html" this way.
    direct = run(assets.fetch("https://x.test/index.html"))
    assert direct.status == 200 and run(direct.text()) == "home"
    assert assets.serve("/app.js").headers.get("content-type").startswith(
        "text/javascript") or assets.serve("/app.js").headers.get(
        "content-type").startswith("application/javascript")


def test_static_assets_apply_headers_file_and_hide_control_files(tmp_path):
    (tmp_path / "forgot-password.html").write_text("form")
    (tmp_path / "about.html").write_text("about")
    (tmp_path / "404.html").write_text("missing")
    (tmp_path / "assets" / "vendor").mkdir(parents=True)
    (tmp_path / "assets" / "vendor" / "lib-1.js").write_text("lib")
    (tmp_path / "_redirects").write_text("/a /b 301")
    (tmp_path / "_headers").write_text(
        "/*\n"
        "  Content-Security-Policy: default-src 'self'\n"
        "  X-Frame-Options: DENY\n"
        "\n"
        "# comment\n"
        "/assets/vendor/*\n"
        "  Cache-Control: public, max-age=31536000, immutable\n"
        "\n"
        "/forgot-password\n"
        "  ! Content-Security-Policy\n"
        "  Content-Security-Policy: default-src 'self'; "
        "script-src 'self' https://challenges.cloudflare.com\n"
        "\n"
        "/:owner/:repo/issues/:number\n"
        "  ! X-Frame-Options\n")
    assets = StaticAssets(tmp_path, "drop-trailing-slash", "404-page")
    about = assets.serve("/about")
    assert about.headers.get("content-security-policy") == \
        "default-src 'self'"
    assert about.headers.get("x-frame-options") == "DENY"
    form = assets.serve("/forgot-password")
    assert "https://challenges.cloudflare.com" in form.headers.get(
        "content-security-policy")
    vendor = assets.serve("/assets/vendor/lib-1.js")
    assert "immutable" in vendor.headers.get("cache-control")
    missing = assets.serve("/acme/widgets/issues/7")
    assert missing.status == 404
    assert missing.headers.get("x-frame-options") is None
    assert missing.headers.get("content-security-policy") == \
        "default-src 'self'"
    for control in ("/_headers", "/_redirects"):
        assert assets.serve(control).status == 404
        assert not assets.exists(control)
    direct = run(assets.fetch("https://x.test/about.html"))
    assert direct.headers.get("content-security-policy") is None


class _Room:
    def __init__(self, ctx, env):
        self.ctx, self.env = ctx, env
        self.messages = []
        self.closed = []
        self.alarms = 0

    async def fetch(self, request):
        client, server = js.WebSocketPair.new().object_values()
        self.ctx.acceptWebSocket(server, to_js(["chat"]))
        server.serializeAttachment(to_js({"id": "s1"},
                                         dict_converter=js.Object.fromEntries))
        server.send("hello")
        return js.Response.new(None, {"status": 101, "webSocket": client})

    async def webSocketMessage(self, ws, message):
        self.messages.append((ws.deserializeAttachment().id, message))
        for peer in self.ctx.getWebSockets("chat"):
            peer.send("echo:" + message)

    async def webSocketClose(self, ws, code, reason, clean):
        self.closed.append((code, reason, clean))

    async def alarm(self, info=None):
        self.alarms += 1
        await self.ctx.storage.put("alarmed", {"n": self.alarms})


def test_durable_object_hibernation_storage_and_alarm():
    async def scenario():
        host = LocalDurableHost(lambda name: _Room, lambda: None)
        namespace = host.namespace("ROOM", "Room")
        object_id = namespace.idFromName("room-1")
        assert namespace.idFromName("room-1") == object_id
        assert object_id.name == "room-1"
        stub = namespace.get(object_id)
        response = await stub.fetch("https://internal/ws")
        client = response.webSocket
        assert (await client._inbox.get()) == ("message", "hello")
        client._client_send("hi")
        assert (await asyncio.wait_for(client._inbox.get(), 1)) == \
            ("message", "echo:hi")
        state = host._states[("Room", object_id.toString())]
        room = state._instance
        assert room.messages == [("s1", "hi")]
        assert len(state.getWebSockets("chat")) == 1
        client._client_closed(1000, "bye", True)
        await asyncio.sleep(0.01)
        assert room.closed == [(1000, "bye", True)]
        assert len(state.getWebSockets("chat")) == 0

        await state.storage.put("k", {"v": [1]})
        stored = await state.storage.get("k")
        stored["v"].append(2)
        assert (await state.storage.get("k")) == {"v": [1]}
        assert list((await state.storage.list({"prefix": "k"})).keys()) == \
            ["k"]
        await state.storage.setAlarm(js.Date.now() + 20)
        assert await state.storage.getAlarm() is not None
        await asyncio.sleep(0.08)
        assert room.alarms == 1 and await state.storage.getAlarm() is None
        assert (await state.storage.get("alarmed")) == {"n": 1}
        order = []

        async def slow():
            order.append("block-start")
            await asyncio.sleep(0.02)
            order.append("block-end")

        async def other():
            await state._gate_wait()
            order.append("other")
        await asyncio.gather(state.blockConcurrencyWhile(slow), other())
        assert order == ["block-start", "block-end", "other"]
    run(scenario())


def test_js_object_is_a_dict_with_attributes():
    value = JsObject(a=1)
    value.b = 2
    assert value.to_py() == {"a": 1, "b": 2}
    with pytest.raises(AttributeError):
        value.missing  # noqa: B018


def test_fetch_streams_redirects_and_times_out(monkeypatch):
    monkeypatch.delenv("FORKMESH_HOST_OFFLINE", raising=False)

    async def handle(reader, writer):
        head = (await reader.readuntil(b"\r\n\r\n")).decode()
        path = head.split(" ")[1]
        if path == "/stream":
            writer.write(b"HTTP/1.1 200 OK\r\ncontent-type: application/x-git"
                         b"\r\ntransfer-encoding: chunked\r\n\r\n")
            for _ in range(3):
                writer.write(b"4\r\npack\r\n")
                await writer.drain()
                await asyncio.sleep(0.01)
            writer.write(b"0\r\n\r\n")
        elif path == "/redirect":
            writer.write(b"HTTP/1.1 302 Found\r\nlocation: /json\r\n"
                         b"content-length: 0\r\n\r\n")
        elif path == "/slow":
            await asyncio.sleep(1)
            writer.write(b"HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n")
        else:
            body = b'{"ok":true}'
            writer.write(b"HTTP/1.1 200 OK\r\ncontent-type: application/json"
                         b"\r\ncontent-length: %d\r\n\r\n%s" % (len(body),
                                                               body))
        await writer.drain()
        writer.close()

    async def scenario():
        server = await asyncio.start_server(handle, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        base = "http://127.0.0.1:%d" % port
        # The egress guard refuses loopback unless allowlisted (egress.py).
        monkeypatch.setenv("FORKMESH_EGRESS_ALLOW", "127.0.0.1:%d" % port)
        try:
            streamed = await js.fetch(base + "/stream")
            assert streamed.status == 200 and streamed.ok
            assert isinstance(streamed.body, js.ReadableStream)
            relayed = js.Response.new(streamed.body, to_js(
                {"status": 200, "headers": {"content-type":
                                            streamed.headers.get(
                                                "content-type")}},
                dict_converter=js.Object.fromEntries))
            assert await relayed.text() == "packpackpack"
            manual = await js.fetch(base + "/redirect", {"redirect": "manual"})
            assert manual.status == 302
            followed = await js.fetch(base + "/redirect")
            assert (await followed.json()).to_py() == {"ok": True}
            assert followed.redirected is True
            with pytest.raises(js.JsException):
                await js.fetch(base + "/redirect", {"redirect": "error"})
            started = asyncio.get_running_loop().time()
            with pytest.raises(js.JsException):
                await js.fetch(base + "/slow", {
                    "signal": js.AbortSignal.timeout(50)})
            assert asyncio.get_running_loop().time() - started < 0.5
        finally:
            server.close()
            await js.close_http_client()
    run(scenario())


def test_d1_caller_names_the_request_and_the_session_it_resolves():
    from forkmesh_host import d1_caller
    request = js.Request.new("https://app.example/api/pulls?x=1", {
        "method": "post",
        "headers": {"cf-connecting-ip": "203.0.113.9"}})
    assert d1_caller.clause() == ""
    d1_caller.note_session("alice", "abcdefghijklmnopqrstuvwx")
    assert d1_caller.clause() == ""
    with d1_caller.request_scope(request):
        assert d1_caller.clause() == "POST /api/pulls, IP 203.0.113.9"
        d1_caller.note_session("alice", "abcdefghijklmnopqrstuvwx")
        assert d1_caller.clause() == (
            "POST /api/pulls, IP 203.0.113.9, user alice, session abcdefgh")
        with d1_caller.request_scope(request):
            assert "user alice" in d1_caller.clause()
        assert d1_caller.header() == [(d1_caller.HEADER, d1_caller.clause())]
    assert d1_caller.clause() == ""
    assert d1_caller.header() == []


def test_d1_caller_fills_an_open_scope_and_strips_parentheses():
    from forkmesh_host import d1_caller
    request = js.Request.new("https://app.example/api/(x)")
    with d1_caller.scope():
        assert d1_caller.clause() == ""
        with d1_caller.request_scope(request):
            assert d1_caller.clause() == "GET /api/ x"
    with d1_caller.scope("cron (* * * * *)"):
        assert d1_caller.clause() == "cron * * * * *"
    with d1_caller.scope(None):
        assert d1_caller.clause() == ""


def test_d1_caller_follows_tasks_but_not_other_requests():
    from forkmesh_host import d1_caller

    async def child():
        return d1_caller.clause()

    async def main():
        with d1_caller.scope("GET /a"):
            task = asyncio.ensure_future(child())
            d1_caller.note_session("bob", "0123456789")
            first = await task
        with d1_caller.scope("GET /b"):
            second = await child()
        return first, second

    assert run(main()) == ("GET /a, user bob, session 01234567", "GET /b")


def test_d1_done_line_keys_the_statement_as_the_entry_logs_it():
    import hashlib
    from forkmesh_host import d1_caller
    sql = "SELECT  id\n FROM users   WHERE name = ?"
    key = hashlib.sha1(b"SELECT id FROM users WHERE name = ?").hexdigest()[:10]
    assert d1_caller.statement_key(sql) == key
    long_sql = "SELECT " + "x, " * 200 + "y FROM t"
    text = " ".join(long_sql.split())
    cut = text[:357] + "..."
    assert d1_caller.statement_key(long_sql, 360) == hashlib.sha1(
        cut.encode()).hexdigest()[:10]
    assert d1_caller.done_line(sql, 3.14159, 2, 0, "local") == (
        "D1-done 3.1 ms, 2 rows, 0 changed, local #" + key)
    assert d1_caller.done_line(sql, 9, failed=True).endswith(
        "0 changed, failed #" + key)


def test_d1_done_prints_only_while_statement_logging_is_on(capsys):
    from forkmesh_host import d1_caller
    d1_caller.disable_done()
    d1_caller.note_done("SELECT 1", 1.0)
    assert capsys.readouterr().out == ""
    d1_caller.enable_done(360)
    try:
        d1_caller.note_done("SELECT 1", 1.0, 1)
    finally:
        d1_caller.disable_done()
    assert capsys.readouterr().out.startswith("D1-done 1.0 ms, 1 rows")


def test_rqlite_statement_reports_time_rows_and_where_it_was_served(capsys):
    from forkmesh_host import d1_caller
    from forkmesh_host.d1_rqlite import (D1Meta, D1PreparedStatement,
                                         D1Result)

    class Db:
        def __init__(self, result):
            self.result = result

        async def _execute_one(self, stmt, session):
            if self.result is None:
                raise RuntimeError("down")
            return self.result

    local = D1Result([{"a": 1}, {"a": 2}],
                     D1Meta(rows_read=2, served_by="rqlite-local"))
    write = D1Result([], D1Meta(changes=3))
    d1_caller.enable_done(360)
    try:
        run(D1PreparedStatement(Db(local), "SELECT a FROM t").all())
        run(D1PreparedStatement(Db(write), "UPDATE t SET a=1").run())
        with pytest.raises(RuntimeError):
            run(D1PreparedStatement(Db(None), "DELETE FROM t").run())
    finally:
        d1_caller.disable_done()
    lines = capsys.readouterr().out.splitlines()
    assert len(lines) == 3
    assert ", 2 rows, 0 changed, local #" in lines[0]
    assert ", 0 rows, 3 changed, http #" in lines[1]
    assert lines[2].endswith(
        ", failed #" + d1_caller.statement_key("DELETE FROM t"))


def test_mesh_forwarding_never_carries_a_client_caller_header():
    from forkmesh_host import cluster, d1_caller
    headers = js.Headers({d1_caller.HEADER: "forged", "accept": "x"})
    assert [name for name, _ in cluster.forward_headers(headers)] == [
        "accept"]
