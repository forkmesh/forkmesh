import ast
import asyncio
import json
import sys
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import agent_live_relay  # noqa: E402

ROUTES = ROOT / "src" / "route_handlers.py"
URLS = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")


def parse(raw):
    events = []
    for block in raw.decode("utf-8").split("\n\n"):
        if not block:
            continue
        if block.startswith(":"):
            events.append(("keepalive", None))
            continue
        fields = dict(line.split(": ", 1) for line in block.split("\n"))
        if "retry" in fields:
            events.append(("retry", int(fields["retry"])))
            continue
        events.append((fields["event"], json.loads(fields["data"])))
    return events


async def drain(stream, until=None):
    out = []
    async for chunk in stream:
        out.extend(parse(chunk))
        if until and until(out):
            break
    return out


async def next_events(stream, count=1):
    out = []
    while len(out) < count:
        out.extend(parse(await stream.__anext__()))
    return out


def test_a_watcher_gets_the_seed_then_appends_then_the_end():
    async def scenario():
        relay = agent_live_relay.Relay()
        assert relay.publish("s1", 1, "ignored") == 0
        stream = relay.watch("s1", 10, "Reading")
        assert await next_events(stream, 2) == [
            ("retry", agent_live_relay.RETRY_MS),
            ("live", {"at": 10, "text": "Reading"})]
        assert relay.watchers("s1") == 1
        assert relay.publish("s1", 11, "Reading the README") == 1
        assert await next_events(stream) == [
            ("live", {"at": 11, "append": " the README"})]
        relay.publish("s1", 12, "Tail moved on")
        assert await next_events(stream) == [
            ("live", {"at": 12, "text": "Tail moved on"})]
        relay.publish("s1", 13, "", "completed")
        assert await next_events(stream) == [("end", {"status": "completed"})]
        try:
            await stream.__anext__()
        except StopAsyncIteration:
            pass
        else:
            raise AssertionError("the stream kept going after its end")
        assert relay.sessions == {}
    asyncio.run(scenario())


def test_a_newer_seed_wins_and_an_older_publish_is_ignored():
    async def scenario():
        relay = agent_live_relay.Relay()
        first = relay.watch("s1", 5, "five")
        await next_events(first, 2)
        second = relay.watch("s1", 3, "three")
        assert (await next_events(second, 2))[1] == (
            "live", {"at": 5, "text": "five"})
        relay.publish("s1", 4, "four")
        relay.publish("s1", 6, "", "running")
        assert relay.sessions["s1"]["text"] == "five"
        assert relay.sessions["s1"]["status"] == ""
        await first.aclose()
        assert relay.watchers("s1") == 1
        await second.aclose()
        assert relay.sessions == {}
    asyncio.run(scenario())


def test_an_idle_stream_sends_keepalives_and_closes_at_its_lifetime():
    async def scenario():
        relay = agent_live_relay.Relay()
        events = await drain(relay.watch("s1", keepalive=0.01, lifetime=0.05))
        assert events[0][0] == "retry"
        assert ("keepalive", None) in events
        assert all(name in ("retry", "keepalive") for name, _ in events)
        assert relay.sessions == {}
    asyncio.run(scenario())


def test_a_cancelled_watcher_is_forgotten():
    async def scenario():
        relay = agent_live_relay.Relay()

        async def consume():
            await drain(relay.watch("s1", 1, "x"))

        task = asyncio.ensure_future(consume())
        await asyncio.sleep(0.01)
        assert relay.watchers("s1") == 1
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        assert relay.sessions == {}
    asyncio.run(scenario())


def test_watchers_and_sessions_are_capped():
    async def scenario():
        relay = agent_live_relay.Relay()
        streams = []
        for _ in range(agent_live_relay.MAX_WATCHERS):
            assert relay.admits("s1")
            stream = relay.watch("s1")
            await next_events(stream)
            streams.append(stream)
        assert not relay.admits("s1")
        for index in range(agent_live_relay.MAX_SESSIONS - 1):
            stream = relay.watch("other-%d" % index)
            await next_events(stream)
            streams.append(stream)
        assert not relay.admits("one-more")
        for stream in streams:
            await stream.aclose()
        assert relay.sessions == {}
    asyncio.run(scenario())


def test_frames_keep_newlines_inside_one_data_line():
    raw = agent_live_relay.frame("live", {"text": "a\nb\r\nc — d"})
    assert raw.count(b"\n") == 3
    assert parse(raw) == [("live", {"text": "a\nb\r\nc — d"})]


def test_an_ended_stream_says_so_at_once():
    events = asyncio.run(drain(agent_live_relay.ended_stream("failed")))
    assert events == [("retry", agent_live_relay.RETRY_MS),
                      ("end", {"status": "failed"})]


class _Body:
    def __init__(self, data):
        self.data = data

    async def text(self):
        return json.dumps(self.data)


class _Target:
    pass


def _bound():
    calls = {}

    def json_response(body, status=200, **_kwargs):
        return {"status": status, "body": body}

    def response(body, headers=None):
        calls["headers"] = headers
        return {"status": 200, "stream": body}

    namespace = vars(agent_live_relay)
    saved = {name: namespace.get(name)
             for name in ("json_response", "Response", "ReadableStream")}
    namespace.update(json_response=json_response, Response=response,
                     ReadableStream=lambda source: source)
    return calls, saved


def _unbind(saved):
    namespace = vars(agent_live_relay)
    for name, value in saved.items():
        if value is None:
            namespace.pop(name, None)
        else:
            namespace[name] = value


def test_the_object_routes_publish_and_watch_one_relay_per_owner():
    calls, saved = _bound()
    try:
        async def scenario():
            target = _Target()
            handle = agent_live_relay.handle
            bad = await handle(target, _Body({"session": ""}),
                               agent_live_relay.PUBLISH_PATH)
            assert bad["status"] == 400
            watched = await handle(
                target, _Body({"session": "s1", "at": 3, "text": "hi"}),
                agent_live_relay.WATCH_PATH)
            assert calls["headers"]["content-type"].startswith(
                "text/event-stream")
            stream = watched["stream"]
            assert (await next_events(stream, 2))[1] == (
                "live", {"at": 3, "text": "hi"})
            published = await handle(
                target, _Body({"session": "s1", "at": 4, "text": "hi there"}),
                agent_live_relay.PUBLISH_PATH)
            assert published["body"] == {"ok": True, "watchers": 1}
            assert await next_events(stream) == [
                ("live", {"at": 4, "append": " there"})]
            await stream.aclose()
            published = await handle(
                target, _Body({"session": "s1", "at": 5, "text": "x"}),
                agent_live_relay.PUBLISH_PATH)
            assert published["body"] == {"ok": True, "watchers": 0}
        asyncio.run(scenario())
    finally:
        _unbind(saved)


def _load(names, extra):
    tree = ast.parse(ROUTES.read_text(encoding="utf-8"))
    nodes = [n for n in tree.body
             if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
             and n.name in names]
    assert len(nodes) == len(names)
    namespace = dict(extra)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(ROUTES), "exec"),
        namespace)
    return namespace


class _Response:
    def __init__(self, status, body=b""):
        self.status = status
        self.body = body

    async def text(self):
        return self.body


class _Namespace:
    def __init__(self, reply):
        self.reply = reply
        self.calls = []

    def idFromName(self, name):
        return name

    def get(self, object_id):
        namespace = self

        class _Stub:
            async def fetch(self, request):
                namespace.calls.append((object_id, request))
                return namespace.reply
        return _Stub()


class _Env:
    def __init__(self, reply):
        self.FORKMESH_NODES = _Namespace(reply)


def _relay_namespace():
    return _load(
        ["_org_agent_live_relay_object", "_org_agent_live_relay_call",
         "_publish_org_agent_live", "_org_agent_live_stream",
         "_org_agent_live_projection"],
        {
            "_agent_live_relay": agent_live_relay,
            "clean_string": lambda value, limit=240: (
                value.strip()[:limit] if isinstance(value, str) else ""),
            "MAX_NODE_NAME": 64,
            "MAX_AGENT_TRANSCRIPT": 16000,
            "_node_events_do_name": lambda owner: "node-events-v1:" + owner,
            "JsRequest": type("JsRequest", (), {
                "new": staticmethod(lambda url, init: (url, init))}),
            "to_js": lambda value: value,
            "json": json,
            "Response": lambda body, headers=None: {
                "status": 200, "stream": body, "headers": headers},
            "ReadableStream": lambda source: source,
            "_org_agent_error": lambda code, message, status, **_: {
                "status": status, "error": code},
        })


def test_publishing_goes_to_the_session_creators_object():
    ns = _relay_namespace()
    env = _Env(_Response(200, json.dumps({"ok": True, "watchers": 2})))
    watchers = asyncio.run(ns["_publish_org_agent_live"](
        env, {"createdBy": "Alice"}, "s1", {"at": 7, "text": "hi"}))
    assert watchers == 2
    (object_id, (url, init)), = env.FORKMESH_NODES.calls
    assert object_id == "node-events-v1:alice"
    assert url == "https://forkmesh.internal" + agent_live_relay.PUBLISH_PATH
    assert json.loads(init["body"]) == {"at": 7, "text": "hi", "session": "s1"}
    assert asyncio.run(ns["_publish_org_agent_live"](
        env, {}, "s1", {"at": 7})) == 0


def test_the_live_route_seeds_the_watch_from_the_stored_projection():
    ns = _relay_namespace()
    reply = _Response(200)
    env = _Env(reply)
    record = {"createdBy": "alice", "live": {"text": "Step one", "at": 9}}
    served = asyncio.run(ns["_org_agent_live_stream"](
        env, {"session_id": "s1", "status": "running"}, record))
    assert served is reply
    (_, (url, init)), = env.FORKMESH_NODES.calls
    assert url.endswith(agent_live_relay.WATCH_PATH)
    assert json.loads(init["body"]) == {
        "session": "s1", "at": 9, "text": "Step one"}
    env = _Env(_Response(429))
    served = asyncio.run(ns["_org_agent_live_stream"](
        env, {"session_id": "s1", "status": "queued"}, record))
    assert served == {"status": 503, "error": "live_unavailable"}


def test_the_live_route_ends_at_once_for_a_finished_session():
    ns = _relay_namespace()
    env = _Env(_Response(200))
    served = asyncio.run(ns["_org_agent_live_stream"](
        env, {"session_id": "s1", "status": "completed"}, {}))
    assert env.FORKMESH_NODES.calls == []
    events = asyncio.run(drain(served["stream"]))
    assert events[-1] == ("end", {"status": "completed"})


def test_the_live_route_shares_the_session_read_route_and_proof():
    from urls import ORG_AGENT_BOT_STREAM_RE, REPO_OWNER_AGENT_SESSION_STREAM_RE
    assert REPO_OWNER_AGENT_SESSION_STREAM_RE.match(
        "/api/repo/alice/proj/agent-sessions/s1/live")
    assert ORG_AGENT_BOT_STREAM_RE.match(
        "/api/orgs/acme/repos/proj/agent-bots/s1/live")
    assert not ORG_AGENT_BOT_STREAM_RE.match(
        "/api/orgs/acme/repos/proj/agent-bots/s1/livez")
    source = ROUTES.read_text(encoding="utf-8")
    handler = source[source.index("async def org_agent_bot_stream_handler("):]
    handler = handler[:handler.index("\nasync def ", 1)]
    assert handler.index("_org_agent_member_context(") < handler.index(
        "_org_agent_live_stream(")
    assert 'urlparse(request.url).path.endswith("/live")' in handler
    assert "(?:stream|live)$" in URLS
    assert "_agent_live_relay.handle(self, request, path)" in ENTRY
    assert urlparse("https://x/api/nodes/events").path not in (
        agent_live_relay.PUBLISH_PATH, agent_live_relay.WATCH_PATH)
