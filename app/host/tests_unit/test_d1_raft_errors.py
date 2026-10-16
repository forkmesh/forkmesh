import asyncio
import sys
from pathlib import Path

import httpx
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from forkmesh_host import d1_rqlite  # noqa: E402


def _run_with_rqlite_error(message, sql="INSERT INTO t (x) VALUES (?)"):
    db = d1_rqlite.make_d1({"url": "http://rqlite.test"})

    def answer(request):
        return httpx.Response(200, json={"results": [{"error": message}]})

    async def go():
        db._client = httpx.AsyncClient(transport=httpx.MockTransport(answer))
        db._client_loop = asyncio.get_running_loop()
        try:
            await db.prepare(sql).bind(1).run()
        finally:
            await db._client.aclose()

    with pytest.raises(d1_rqlite.D1Error) as caught:
        asyncio.run(go())
    return str(caught.value)


@pytest.mark.parametrize("message", [
    "timed out enqueuing operation",
    "leader not found",
    "leadership lost while committing log",
    "raft is already shutdown",
    "context deadline exceeded",
    "index 3589896: timeout waiting for fsm",
    "store not open",
])
def test_raft_backpressure_is_overload_not_sqlite_error(message):
    text = _run_with_rqlite_error(message)
    assert text.startswith("D1_ERROR: D1 DB is overloaded"), text
    assert "SQLITE_ERROR" not in text


@pytest.mark.parametrize("message, code", [
    ("no such table: t", "SQLITE_ERROR"),
    ("UNIQUE constraint failed: t.x", "SQLITE_CONSTRAINT"),
])
def test_statement_errors_stay_sqlite_errors(message, code):
    text = _run_with_rqlite_error(message)
    assert text == "D1_ERROR: %s: %s" % (message, code)
    assert "overload" not in text.lower()
