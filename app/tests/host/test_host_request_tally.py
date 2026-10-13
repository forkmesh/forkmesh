from __future__ import annotations

import asyncio
import json

from forkmesh_host import js as _js
from forkmesh_host import request_tally
from forkmesh_host.asgi import HostASGI, WorkerFirstRules


class _Clock:
    def __init__(self, now):
        self.now = now

    def __call__(self):
        return self.now


def _group(path):
    parts = [p for p in path.split("/") if p][1:]
    return parts[0] if len(parts) == 1 else parts[0] + "/*"


def _tally(clock, lines):
    return request_tally.RequestTally(group=_group, clock=clock,
                                      emit=lines.append)


def _decode(line):
    assert line.startswith(request_tally.PREFIX)
    return json.loads(line[len(request_tally.PREFIX):])


def test_status_class_folds_statuses():
    assert request_tally.status_class(200) == "2xx"
    assert request_tally.status_class(101) == "2xx"
    assert request_tally.status_class(304) == "3xx"
    assert request_tally.status_class(404) == "4xx"
    assert request_tally.status_class(503) == "5xx"
    assert request_tally.status_class(0) == "5xx"


def test_record_aggregates_one_line_per_window():
    clock = _Clock(1_000_003.5)
    lines = []
    tally = _tally(clock, lines)
    assert tally.record("/api/accounts/alice", 200, 12.4)
    assert tally.record("/api/accounts/bob", 200, 30)
    assert tally.record("/api/accounts/bob", 404, 5)
    assert tally.record("/api/version", 503, 7)
    assert not tally.record("/assets/app.js", 200, 1)
    assert not tally.record("/", 200, 1)
    assert lines == []
    clock.now += 10
    tally.record("/api/version", 200, 3)
    assert len(lines) == 1
    payload = _decode(lines[0])
    assert payload["t"] == 1_000_000_000
    assert payload["s"] == request_tally.WINDOW_S
    assert payload["r"] == [["accounts/*", "2xx", 2, 42, 30],
                            ["accounts/*", "4xx", 1, 5, 5],
                            ["version", "5xx", 1, 7, 7]]
    assert "alice" not in lines[0] and "bob" not in lines[0]
    assert _decode(tally.flush())["r"] == [["version", "2xx", 1, 3, 3]]
    assert tally.flush() is None


def test_group_cap_folds_new_labels_into_other():
    lines = []
    tally = request_tally.RequestTally(group=lambda path: path, emit=lines.append,
                                       clock=_Clock(0))
    for index in range(request_tally.MAX_GROUPS + 5):
        tally.record("/api/g%d" % index, 200, 1)
    tally.record("/api/" + request_tally.FLAGSHIP_GROUP, 200, 1)
    rows = _decode(tally.flush())["r"]
    groups = {row[0] for row in rows}
    assert "other" in groups
    assert len(groups) == request_tally.MAX_GROUPS + 1


def test_group_failure_counts_as_other():
    def broken(path):
        raise ImportError("api_metrics")

    tally = request_tally.RequestTally(group=broken, emit=lambda line: None,
                                       clock=_Clock(0))
    tally.record("/api/x", 200, 1)
    assert _decode(tally.line())["r"] == [["other", "2xx", 1, 1, 1]]


def test_due_after_window_boundary():
    clock = _Clock(100.0)
    tally = _tally(clock, [])
    assert not tally.due()
    tally.record("/api/x", 200, 1)
    assert not tally.due()
    clock.now = 110.0
    assert tally.due()


class _Env:
    FORKMESH_PLATFORM = "vps"
    APP_ORIGIN = "https://app.next.forkmesh.com"
    WORLD_ORIGIN = "https://world.next.forkmesh.com"


class _Runtime:
    def __init__(self, status=200, fail=False):
        self.env = _Env()
        self.sealed = False
        self.durable = None
        self.status = status
        self.fail = fail

    async def fetch(self, request):
        if self.fail:
            raise RuntimeError("boom")
        return _js.Response(b"ok", self.status, "",
                            _js.Headers({"content-type": "text/plain"}))


def _serve(runtime, path, monkeypatch):
    monkeypatch.delenv("FORKMESH_PUBLIC_HOSTS", raising=False)
    app = HostASGI(runtime, serve_assets=False,
                   worker_first=WorkerFirstRules([]))
    lines = []
    app.request_tally = _tally(_Clock(50.0), lines)
    scope = {"type": "http", "method": "GET", "path": path,
             "raw_path": path.encode(), "query_string": b"",
             "headers": [(b"host", b"app.next.forkmesh.com")],
             "client": ("127.0.0.1", 50000), "scheme": "http",
             "http_version": "1.1"}
    sent = []

    async def receive():
        return {"type": "http.request", "body": b"", "more_body": False}

    async def send(message):
        sent.append(message)

    asyncio.run(app(scope, receive, send))
    return app, sent


def test_asgi_counts_api_requests(monkeypatch):
    app, sent = _serve(_Runtime(status=404), "/api/accounts/alice",
                       monkeypatch)
    assert sent[0]["status"] == 404
    assert _decode(app.request_tally.line())["r"][0][:3] == [
        "accounts/*", "4xx", 1]


def test_asgi_counts_worker_failures_as_5xx(monkeypatch):
    app, sent = _serve(_Runtime(fail=True), "/api/version", monkeypatch)
    assert sent[0]["status"] == 500
    assert _decode(app.request_tally.line())["r"][0][:3] == [
        "version", "5xx", 1]


def test_asgi_skips_non_api_paths(monkeypatch):
    app, sent = _serve(_Runtime(), "/alice/repo", monkeypatch)
    assert sent[0]["status"] == 200
    assert app.request_tally.line() is None
