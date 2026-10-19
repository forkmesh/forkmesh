import asyncio
import json
import sys
from pathlib import Path

import httpx
import pytest

APP = Path(__file__).resolve().parents[1]
if str(APP / "host") not in sys.path:
    sys.path.insert(0, str(APP / "host"))

from forkmesh_host.repo_fastpath import RepoFastPath  # noqa: E402
from test_mirror_gateway import (  # noqa: E402,F401
    AcceptVerifier,
    FakeArchiveMaterializer,
    HealthSigner,
    NOW,
    gateway,
    make_bare_repository,
    write_config,
)


@pytest.fixture
def gateway_app(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    config = gateway.load_config(write_config(tmp_path, bare))
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    yield app
    app.close()


def _transport(app, seen):
    def handler(request):
        target = request.url.raw_path.decode("ascii")
        seen.append(target)
        response = app.dispatch("GET", target, {}, b"")
        return httpx.Response(
            response.status,
            headers={"content-type": response.content_type},
            content=response.body,
        )
    return httpx.MockTransport(handler)


def _history(app, query, seen):
    async def main():
        base = "http://gateway.test"
        fast = RepoFastPath(base, client=httpx.AsyncClient(
            base_url=base, transport=_transport(app, seen)))
        sent = []

        async def send(message):
            sent.append(message)
        path = "/api/repo/alice/project/history"
        try:
            handled = await fast.handle({
                "type": "http", "method": "GET", "path": path,
                "raw_path": path.encode(), "query_string": query.encode(),
                "headers": []}, send)
        finally:
            await fast.aclose()
        return handled, sent
    handled, sent = asyncio.run(main())
    assert handled
    assert sent[0]["status"] == 200
    body = b"".join(message.get("body", b"") for message in sent[1:])
    return [commit["subject"] for commit in json.loads(body)["commits"]]


def test_history_with_a_path_lists_only_commits_touching_it(gateway_app):
    seen = []
    everything = _history(gateway_app, "ref=main&limit=5", seen)
    assert everything == ["Improve greeting", "Initial files"]
    readme = _history(gateway_app, "path=README.md&ref=main&limit=5", seen)
    assert readme == ["Initial files"]
    folder = _history(gateway_app, "path=./src/&ref=main", seen)
    assert folder == ["Improve greeting", "Initial files"]
    history_reads = [target for target in seen if "/history" in target]
    assert history_reads[1].endswith("history?path=README.md&ref=main")
    assert history_reads[2].endswith("history?path=src&ref=main")
