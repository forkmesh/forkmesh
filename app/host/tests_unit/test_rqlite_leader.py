import asyncio
import os
import sys

import httpx

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from forkmesh_host.cluster import Rqlite  # noqa: E402


def _client(handler, seen):
    def record(request):
        seen.append(request.url.path)
        return handler(request)
    return httpx.AsyncClient(transport=httpx.MockTransport(record))


def _leader(handler):
    seen = []
    rqlite = Rqlite("http://rqlite.test")

    async def run():
        await rqlite._client.aclose()
        rqlite._client = _client(handler, seen)
        first = await rqlite.leader()
        second = await rqlite.leader()
        await rqlite.aclose()
        return first, second

    first, second = asyncio.run(run())
    return first, second, seen


def test_leader_uses_leader_route_not_status():
    def handler(request):
        if request.url.path == "/leader":
            return httpx.Response(200, json={"id": "n2", "leader": False})
        return httpx.Response(500)

    first, second, seen = _leader(handler)
    assert (first, second) == ("n2", "n2")
    assert seen == ["/leader", "/leader"]


def test_leader_without_leader_is_empty():
    first, _, seen = _leader(lambda request: httpx.Response(503))
    assert first == ""
    assert "/status" not in seen


def test_leader_falls_back_to_status_once_on_old_rqlite():
    def handler(request):
        if request.url.path == "/leader":
            return httpx.Response(404)
        return httpx.Response(
            200, json={"store": {"leader": {"node_id": "n3"}}})

    first, second, seen = _leader(handler)
    assert (first, second) == ("n3", "n3")
    assert seen == ["/leader", "/status", "/status"]
