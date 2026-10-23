from __future__ import annotations

import asyncio

import httpx

from forkmesh_host.repo_fastpath import RepoFastPath, StaleCache

from test_repo_fastpath import COMMIT, StubGateway, gateway, scope  # noqa: F401

PULLS = scope("/api/repo/forkmesh/forkmesh/tree", "path=.forkmesh/pulls")
RECORD = scope("/api/repo/forkmesh/forkmesh/blob", "path=.forkmesh/pulls/3/pull.md")


def run(base, steps):
    async def main():
        fast = RepoFastPath(base, client=httpx.AsyncClient(
            base_url=base, timeout=httpx.Timeout(20.0, connect=10.0),
            trust_env=False))
        try:
            return await steps(fast)
        finally:
            await fast.aclose()
    return asyncio.run(main())


async def handled(fast, sc):
    sent = []

    async def send(message):
        sent.append(message)
    return await fast.handle(sc, send), sent


async def stale(fast, sc, status):
    answer = fast.stale_answer(sc, status)
    if answer is None:
        return None
    sent = []

    async def send(message):
        sent.append(message)
    await fast.send_stale(sc, send, answer)
    headers = {k.decode(): v.decode() for k, v in sent[0]["headers"]}
    return sent[0]["status"], headers, b"".join(m.get("body", b"") for m in sent[1:])


def test_outage_answers_the_last_good_pull_folder_and_record_marked_stale(gateway):  # noqa: F811
    async def steps(fast):
        fresh = {}
        for sc in (PULLS, RECORD):
            ok, sent = await handled(fast, sc)
            assert ok
            fresh[sc["query_string"]] = b"".join(m.get("body", b"") for m in sent[1:])
        StubGateway.status = 503
        out = {}
        for sc in (PULLS, RECORD):
            ok, _sent = await handled(fast, sc)
            assert not ok
            out[sc["query_string"]] = await stale(fast, sc, 503)
        return fresh, out, await stale(fast, PULLS, 404), await stale(fast, PULLS, 200)
    fresh, out, not_found, fine = run(gateway, steps)
    for key, answer in out.items():
        status, headers, body = answer
        assert status == 200
        assert headers["x-forkmesh-stale"] == "1"
        assert headers["cache-control"] == "no-store"
        assert body == fresh[key]
    assert not_found is None and fine is None


def test_a_real_404_forgets_the_stale_answer(gateway):  # noqa: F811
    async def steps(fast):
        assert (await handled(fast, PULLS))[0]
        StubGateway.status = 404
        assert not (await handled(fast, PULLS))[0]
        return fast.stale_answer(PULLS, 503)
    assert run(gateway, steps) is None


def test_private_views_pinned_reads_and_other_operations_are_never_stale(gateway):  # noqa: F811
    private = scope("/api/repo/forkmesh/forkmesh/tree",
                    "path=.forkmesh/pulls&viewer=a&ts=1&sig=b")
    pinned = scope("/api/repo/forkmesh/forkmesh/tree", "path=&ref=" + COMMIT)
    search = scope("/api/repo/forkmesh/forkmesh/search", "q=x")

    async def steps(fast):
        for sc in (private, pinned, search):
            await handled(fast, sc)
        return [fast.stale_answer(sc, 503) for sc in (private, pinned, search)], len(fast.stale)
    answers, entries = run(gateway, steps)
    assert answers == [None, None, None]
    assert entries == 0


def test_stale_cache_is_bounded_by_entries_bytes_size_and_age():
    now = [0.0]
    cache = StaleCache(max_entries=3, max_bytes=10, entry_max=6, max_age=60,
                       clock=lambda: now[0])
    cache.put("big", "t", b"1234567")
    assert cache.get("big") is None
    for key in ("a", "b", "c", "d"):
        cache.put(key, "t", b"12")
    assert len(cache) == 3 and cache.get("a") is None
    cache.put("e", "t", b"123456")
    assert cache.bytes <= 10
    assert cache.get("b") is None and cache.get("e") == ("t", b"123456")
    now[0] = 61.0
    assert cache.get("e") is None
    assert cache.bytes == sum(len(v) for v in (b"12",) * len(cache))


def test_the_host_answers_stale_only_after_the_worker_reports_an_outage(monkeypatch):
    from forkmesh_host import js as _js
    from test_host_request_guard import _Runtime, _app, _call, _scope

    class Outage(_Runtime):
        status = 503

        async def fetch(self, request):
            self.seen.append(request)
            return _js.Response(b'{"ok":false,"error":"mirror_unavailable"}', self.status, "",
                                _js.Headers({"content-type": "application/json"}))

    fast = RepoFastPath("http://127.0.0.1:9")

    async def gateway_down(scope, send):
        return False
    fast.handle = gateway_down
    fast.stale.put("/v1/public/repositories/acme/site/tree?path=.forkmesh%2Fpulls",
                   "application/json", b'{"ok":true,"entries":[]}')

    runtime = Outage()
    app = _app(runtime, monkeypatch)
    app.repo_fastpath = fast
    request = _scope("/api/repo/acme/site/tree")
    request["query_string"] = b"path=.forkmesh/pulls"
    status, body = _call(app, request)
    assert status == 200 and body == b'{"ok":true,"entries":[]}'
    assert len(runtime.seen) == 1
    runtime.status = 404
    status, _body = _call(app, request)
    assert status == 404
