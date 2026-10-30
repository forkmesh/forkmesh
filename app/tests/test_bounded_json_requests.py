"""Every Worker JSON route uses the common bounded body reader."""

import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import pytest

from hidden_unicode import clean_value


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")
RUNTIME = ENTRY.with_name("app_runtime.py")
RUNTIME_TEXT = RUNTIME.read_text(encoding="utf-8")
BODY_READER = ENTRY.with_name("request_bodies.py")
BODY_TEXT = BODY_READER.read_text(encoding="utf-8")


def _source(name):
    tree = ast.parse(BODY_TEXT)
    for node in tree.body:
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name == name
        ):
            return ast.get_source_segment(BODY_TEXT, node)
    raise AssertionError(name)


def _default_runtime_source():
    for node in ast.parse(RUNTIME_TEXT, filename=str(RUNTIME)).body:
        if isinstance(node, ast.ClassDef) and node.name == "Default":
            return ast.get_source_segment(RUNTIME_TEXT, node)
    raise AssertionError("Default runtime class is missing")


class TooLarge(BaseException):
    pass


def _reader():
    namespace = {
        "RequestBodyTooLarge": TooLarge,
        "JSON_BODY_DEFAULT_MAX_BYTES": 64,
        "JSON_BODY_LARGE_MAX_BYTES": 128,
        "JSON_BODY_LARGE_PATH_PREFIXES": ("/api/security/",),
        "JSON_BODY_LARGE_REPO_COLLECTIONS": ("pulls",),
        "urlparse": urlparse,
        "json": json,
        "clean_value": clean_value,
    }
    exec(_source("_json_request_limit"), namespace)
    exec(_source("bounded_request_text"), namespace)
    exec(_source("bounded_json_request"), namespace)
    return namespace["bounded_json_request"]


def test_reader_rejects_announced_and_streamed_oversize_bodies():
    reader = _reader()
    announced = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/login",
        headers={"content-length": "65"},
        text=lambda: asyncio.sleep(0, result="{}"),
    )
    with pytest.raises(TooLarge):
        asyncio.run(reader(announced))

    streamed = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/login",
        headers={},
        text=lambda: asyncio.sleep(0, result=json.dumps({"x": "y" * 80})),
    )
    with pytest.raises(TooLarge):
        asyncio.run(reader(streamed))

    class ChunkReader:
        def __init__(self):
            self.chunks = [b'{"x":"', b"z" * 80]
            self.cancelled = False

        async def read(self):
            if self.chunks:
                return {"done": False, "value": self.chunks.pop(0)}
            return {"done": True}

        async def cancel(self):
            self.cancelled = True

    chunk_reader = ChunkReader()
    chunked = SimpleNamespace(
        url="https://forkmesh.test/api/accounts/login",
        headers={},
        body=SimpleNamespace(getReader=lambda: chunk_reader),
        text=lambda: (_ for _ in ()).throw(AssertionError("buffered read")),
    )
    with pytest.raises(TooLarge):
        asyncio.run(reader(chunked))
    assert chunk_reader.cancelled


def test_reader_parses_bounded_json_and_has_explicit_large_routes():
    reader = _reader()
    request = SimpleNamespace(
        url="https://forkmesh.test/api/security/scans",
        headers={},
        text=lambda: asyncio.sleep(0, result=json.dumps({"x": "y" * 80})),
    )
    assert asyncio.run(reader(request))["x"].startswith("y")


def test_no_route_bypasses_the_bounded_json_reader():
    worker_source = TEXT + "\n" + RUNTIME_TEXT
    assert "await request.json()" not in worker_source
    assert worker_source.count("await bounded_json_request(request)") >= 60
    runtime = _default_runtime_source()
    assert "except RequestBodyTooLarge:" in runtime
    assert '"request_too_large"' in runtime


def test_pull_submissions_get_the_large_body_allowance():
    namespace = {
        "JSON_BODY_DEFAULT_MAX_BYTES": 64,
        "JSON_BODY_LARGE_MAX_BYTES": 128,
        "JSON_BODY_LARGE_PATH_PREFIXES": ("/api/security/",),
        "JSON_BODY_LARGE_REPO_COLLECTIONS": ("pulls",),
        "urlparse": urlparse,
    }
    exec(_source("_json_request_limit"), namespace)
    limit = namespace["_json_request_limit"]

    def at(path):
        return limit(SimpleNamespace(url="https://forkmesh.test" + path))

    assert at("/api/repo/alice/tools/pulls") == 128
    assert at("/api/repo/alice/tools/pulls/") == 64
    assert at("/api/repo//tools/pulls") == 64
    assert at("/api/repo/alice/tools/issues") == 64
    assert at("/api/repo/alice/tools/pulls/7/events") == 64
    assert at("/api/repo/pulls") == 64
    assert at("/api/security/advisories") == 128
