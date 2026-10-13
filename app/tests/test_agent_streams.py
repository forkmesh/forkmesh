import ast
import asyncio
import base64
import json
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import agent_streams  # noqa: E402
from test_runner_leases import (  # noqa: E402
    FakeRuntime as LeaseRuntime, Key, NOW, call, claim, signed)


class FakeRuntime(LeaseRuntime):
    def __init__(self):
        super().__init__()
        self.db.executescript(
            (ROOT / "migrations" / "0232_agent_session_streams.sql").read_text()
            .split("-- The runtime schema text changed")[0])
        self.rung = []

    async def notify(self, account, topic):
        self.rung.append((account, topic))


def b64(raw):
    return base64.b64encode(raw).decode()


def setup():
    runtime = FakeRuntime()
    key = Key()
    runtime.add_runner("pool-1", key)
    job = runtime.add_job(session="s1")
    lease = claim(runtime, "pool-1", key)["data"]["jobs"][0]["leaseId"]
    return runtime, key, job, lease


def post(runtime, key, job, lease, seq=0, data=None, runner="pool-1"):
    body = {"jobId": job, "leaseId": lease, "seq": seq}
    if data is not None:
        body["data"] = b64(data) if isinstance(data, bytes) else data
    return call(signed(runtime, runner, key, "/api/runners/stream", body),
                "/api/runners/stream")


def view(runtime, after=0, viewer="alice", status="running", session="s1"):
    return asyncio.run(agent_streams.read(
        runtime, session, viewer, str(after), status))


def relayed(result):
    return b"".join(base64.b64decode(c["data"]) for c in result["chunks"])


def test_only_the_lease_holder_may_post():
    runtime, key, job, lease = setup()
    other = Key()
    runtime.add_runner("pool-2", other)
    assert post(runtime, other, job, lease, runner="pool-2")["status"] == 409
    assert post(runtime, key, job, "wrong")["status"] == 409
    assert post(runtime, key, job + 1, lease)["status"] == 409
    unsigned = call(signed(runtime, "pool-1", Key(), "/api/runners/stream",
                           {"jobId": job, "leaseId": lease, "seq": 0}),
                    "/api/runners/stream")
    assert unsigned["status"] == 401
    ok = post(runtime, key, job, lease)
    assert ok["status"] == 200 and ok["data"]["watched"] is False


def test_heartbeat_reports_watched_only_while_a_viewer_reads():
    runtime, key, job, lease = setup()
    assert post(runtime, key, job, lease)["data"]["watched"] is False
    view(runtime)
    assert post(runtime, key, job, lease)["data"]["watched"] is True
    runtime.clock += agent_streams.WATCH_MS - 1
    assert post(runtime, key, job, lease)["data"]["watched"] is True
    runtime.clock += 2
    assert post(runtime, key, job, lease)["data"]["watched"] is False
    view(runtime, status="completed")
    assert post(runtime, key, job, lease)["data"]["watched"] is False


def test_chunks_are_numbered_scrubbed_and_ring_the_watchers():
    runtime, key, job, lease = setup()
    view(runtime, viewer="alice")
    view(runtime, viewer="bob")
    runtime.rung.clear()
    first = post(runtime, key, job, lease, 1, b'{"type":"assistant"}\n')
    assert first["status"] == 200 and first["data"]["seq"] == 1
    assert sorted(runtime.rung) == [("alice", agent_streams.TOPIC),
                                    ("bob", agent_streams.TOPIC)]
    secret = b'{"text":"sk-ant-api03-SECRETSECRET"}\n'  # forkmesh-secret-scan:ignore-line
    assert post(runtime, key, job, lease, 2, secret)["status"] == 200
    assert post(runtime, key, job, lease, 2, secret)["status"] == 200
    assert post(runtime, key, job, lease, 4, b"x")["status"] == 409
    result = view(runtime)
    assert [c["seq"] for c in result["chunks"]] == [1, 2]
    assert {c["job"] for c in result["chunks"]} == {job}
    data = relayed(result)
    assert b"SECRET" not in data and b"[redacted]" in data
    assert result["nextSeq"] == 2 and result["live"] is True
    assert result["truncated"] is False and result["more"] is False
    assert relayed(view(runtime, after=1)) == data[len(b'{"type":"assistant"}\n'):]


def test_bounds_on_chunk_size_and_body():
    runtime, key, job, lease = setup()
    assert post(runtime, key, job, lease, 1,
                b"x" * (agent_streams.MAX_CHUNK_BYTES + 1))["status"] == 400
    assert post(runtime, key, job, lease, 1, "not base64!")["status"] == 400
    assert post(runtime, key, job, lease, 1, b"")["status"] == 400
    assert post(runtime, key, job, lease, -1)["status"] == 400
    assert post(runtime, key, job, lease, 1,
                b"x" * agent_streams.MAX_CHUNK_BYTES)["status"] == 200


def test_the_ring_keeps_only_the_newest_chunks_and_says_so():
    runtime, key, job, lease = setup()
    view(runtime)
    total = agent_streams.RING_CHUNKS + 10
    for seq in range(1, total + 1):
        assert post(runtime, key, job, lease, seq,
                    b"%d\n" % seq)["status"] == 200
    count = runtime.db.execute(
        "SELECT COUNT(*) FROM agent_stream_chunks WHERE session_id='s1'"
    ).fetchone()[0]
    assert count == agent_streams.RING_CHUNKS
    result = view(runtime)
    assert result["truncated"] is True and result["more"] is True
    assert result["chunks"][0]["seq"] == 11
    assert len(result["chunks"]) == agent_streams.READ_PAGE
    tail = view(runtime, after=total - 2)
    assert [c["seq"] for c in tail["chunks"]] == [total - 1, total]
    assert tail["truncated"] is False


def test_a_cursor_past_the_stream_starts_over():
    runtime, key, job, lease = setup()
    view(runtime)
    post(runtime, key, job, lease, 1, b"a\n")
    result = view(runtime, after=50)
    assert result["reset"] is True and [c["seq"] for c in result["chunks"]] == [1]


def test_the_run_ending_hides_and_later_deletes_its_chunks():
    runtime, key, job, lease = setup()
    view(runtime)
    post(runtime, key, job, lease, 1, b"a\n")
    asyncio.run(agent_streams.end(runtime, "s1", runtime.clock))
    ended = view(runtime, status="completed")
    assert ended["chunks"] == [] and ended["live"] is False
    assert post(runtime, key, job, lease, 2, b"b\n")["status"] == 409
    runtime.clock += agent_streams.ENDED_KEEP_MS + 1
    view(runtime, status="completed")
    for table in ("agent_stream_chunks", "agent_stream_state",
                  "agent_stream_watchers"):
        assert runtime.db.execute(
            "SELECT COUNT(*) FROM %s" % table).fetchone()[0] == 0


def test_a_follow_up_job_replaces_the_previous_runs_chunks():
    runtime, key, job, lease = setup()
    view(runtime)
    post(runtime, key, job, lease, 1, b"old\n")
    asyncio.run(agent_streams.end(runtime, "s1", runtime.clock))
    queued = view(runtime, status="queued")
    assert queued["chunks"] == [] and queued["live"] is False
    status, body = asyncio.run(agent_streams.accept(
        runtime, "s1", job + 5, {"seq": 1, "data": b64(b"new\n")}))
    assert status == 200 and body["watched"] is True
    result = view(runtime, after=0)
    assert relayed(result) == b"new\n" and result["job"] == job + 5
    assert result["chunks"][0]["seq"] == 2 and result["truncated"] is False
    status, _ = asyncio.run(agent_streams.accept(
        runtime, "s1", job, {"seq": 0}))
    assert status == 409


def test_an_abandoned_stream_is_swept():
    runtime, key, job, lease = setup()
    view(runtime)
    post(runtime, key, job, lease, 1, b"a\n")
    runtime.clock += agent_streams.STALE_MS + 1
    asyncio.run(agent_streams.sweep(runtime, runtime.clock))
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM agent_stream_chunks").fetchone()[0] == 0


def test_forget_drops_everything_of_a_deleted_session():
    runtime, key, job, lease = setup()
    view(runtime)
    post(runtime, key, job, lease, 1, b"a\n")
    asyncio.run(agent_streams.forget(runtime, "s1"))
    for table in ("agent_stream_chunks", "agent_stream_state",
                  "agent_stream_watchers"):
        assert runtime.db.execute(
            "SELECT COUNT(*) FROM %s" % table).fetchone()[0] == 0


ROUTES = ROOT / "src" / "route_handlers.py"


def _load_handler(extra):
    tree = ast.parse(ROUTES.read_text(encoding="utf-8"))
    node = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef)
                and n.name == "org_agent_bot_stream_handler")
    namespace = dict(extra)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[])), str(ROUTES), "exec"),
        namespace)
    return namespace["org_agent_bot_stream_handler"]


class _Request:
    def __init__(self, method, url):
        self.method = method
        self.url = url


def _handler_harness(member_error=None, row=None, signed=False):
    calls = {}
    runtime = FakeRuntime()

    async def ensure_schema(_env):
        pass

    async def signed_session(_env, _request, _data):
        calls["signed"] = True
        return ("bi:alice", {"name": "alice"})

    async def member_context(_env, _request, org, repo, _data,
                             owner_scope=False, signed_account=None):
        calls["context"] = (org, repo, owner_scope, signed_account)
        if member_error:
            return None, member_error
        return {"actor": "alice", "org": org, "repo": repo}, None

    async def session_row(_env, _context, session_id):
        calls["session"] = session_id
        return (row, {}) if row else (None, None)

    class _Streams:
        WorkerStore = staticmethod(lambda _env: runtime)
        read = staticmethod(agent_streams.read)

    handler = _load_handler({
        "ensure_schema": ensure_schema,
        "method_name": lambda request: request.method,
        "_org_agent_error": lambda code, message, status, **_: {
            "status": status, "error": code},
        "_owner_agent_signed_triple": lambda _request: signed,
        "_owner_agent_signed_session": signed_session,
        "_org_agent_member_context": member_context,
        "_org_agent_session_row": session_row,
        "_agent_streams": lambda: _Streams,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "json_response": lambda body, status=200, **_: {
            "status": status, "body": json.loads(json.dumps(body))},
    })
    return handler, calls, runtime


URL = "https://app.forkmesh.com/api/repo/alice/proj/agent-sessions/s1/stream"


def _serve(handler, method="GET", url=URL + "?after=0", owner_scope=True):
    return asyncio.run(handler(object(), _Request(method, url), "alice",
                               "proj", "s1", owner_scope=owner_scope))


def test_the_viewer_route_is_authorized_like_the_session_read():
    handler, calls, _ = _handler_harness(
        member_error={"status": 401, "error": "unauthorized"})
    assert _serve(handler)["status"] == 401
    assert "session" not in calls
    handler, _, _ = _handler_harness()
    assert _serve(handler)["status"] == 404
    handler, _, _ = _handler_harness(row={"session_id": "s1",
                                          "status": "running"})
    assert _serve(handler, method="POST")["status"] == 405
    handler, calls, _ = _handler_harness(
        row={"session_id": "s1", "status": "running"}, signed=True)
    _serve(handler, owner_scope=False)
    assert calls["signed"] is True
    assert calls["context"] == ("alice", "proj", False, ("bi:alice", {"name": "alice"}))


def test_the_viewer_route_marks_the_session_watched_and_returns_chunks():
    handler, _, runtime = _handler_harness(
        row={"session_id": "s1", "status": "running"})
    served = _serve(handler)
    assert served["status"] == 200 and served["body"]["status"] == "running"
    watcher = runtime.db.execute(
        "SELECT account,until FROM agent_stream_watchers").fetchall()
    assert [tuple(w) for w in watcher] == [("alice", NOW + agent_streams.WATCH_MS)]
    runtime.db.execute("UPDATE agent_stream_state SET job_id=3")
    runtime.db.commit()
    asyncio.run(agent_streams.accept(runtime, "s1", 3,
                                     {"seq": 1, "data": b64(b"hi\n")}))
    served = _serve(handler)
    assert [c["seq"] for c in served["body"]["chunks"]] == [1]
    handler, _, runtime = _handler_harness(
        row={"session_id": "s1", "status": "completed"})
    _serve(handler)
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM agent_stream_watchers").fetchone()[0] == 0
