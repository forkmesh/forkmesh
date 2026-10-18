from __future__ import annotations

import asyncio
import importlib
import json

import pytest


class _Headers:
    def __init__(self, values):
        self._values = {key.lower(): value for key, value in values.items()}

    def get(self, name):
        return self._values.get(name.lower())


class _Request:
    def __init__(self, method, body, url="https://app.forkmesh.test/api/calendar"):
        self.method = method
        self.url = url
        self._body = body
        self.headers = _Headers({"content-length": str(len(body.encode()))})

    async def text(self):
        return self._body


@pytest.fixture(scope="module")
def entry(staged_src):
    del staged_src
    return importlib.import_module("entry")


@pytest.mark.parametrize("method,body,expected", [
    ("POST", json.dumps({"title": "Standup", "startMs": 1, "endMs": 2}),
     {"title": "Standup", "startMs": 1, "endMs": 2}),
    ("PATCH", json.dumps({"title": "Moved"}), {"title": "Moved"}),
    ("DELETE", "{}", {}),
    ("DELETE", "", {}),
])
def test_calendar_runtime_reads_write_bodies(entry, method, body, expected):
    runtime = entry._CalendarRuntime(None, _Request(method, body))
    assert asyncio.run(runtime.json()) == expected
    assert len(runtime._calendar_body_digest) == 64


def test_calendar_runtime_refuses_an_oversized_body(entry):
    runtime = entry._CalendarRuntime(
        None, _Request("POST", "x" * (entry._json_request_limit(_Request("POST", "")) + 1)))
    with pytest.raises(entry.RequestBodyTooLarge):
        asyncio.run(runtime.json())
