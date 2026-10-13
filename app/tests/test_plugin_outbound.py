#!/usr/bin/env python3
"""The plugin HTTP capability stays provider-neutral and manifest-bounded."""

import ast
import asyncio
import json
from pathlib import Path
from urllib.parse import urljoin, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"


class _WorldCommunityRuntime:
    def __init__(self, env, request):
        self.env = env
        self.request = request


class _Response:
    def __init__(self, status=200, body='{"accepted": true}', location="", link=""):
        self.status = status
        self._body = body
        self.headers = {key: value for key, value in (("location", location), ("link", link))
                        if value}

    async def text(self):
        return self._body


def _runtime(fetch, origins=("https://api.example.test",)):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    class_node = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "_PluginApiRuntime"
    )
    namespace = {
        "_WorldCommunityRuntime": _WorldCommunityRuntime,
        "CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS": 9,
        "js_fetch_with_timeout": fetch,
        "json": json,
        "urljoin": urljoin,
        "urlparse": urlparse,
    }
    exec(compile(ast.Module(body=[class_node], type_ignores=[]), str(ENTRY), "exec"),
         namespace)
    return namespace["_PluginApiRuntime"]({}, object(), None, origins)


def test_manifest_origin_and_transport_policy_are_applied_before_fetch():
    calls = []

    async def fetch(resource, options, timeout):
        calls.append((resource, options, timeout))
        return _Response()

    runtime = _runtime(fetch)
    result = asyncio.run(runtime.outbound_json(
        "https://api.example.test/v1/import", "POST",
        {"Authorization": "Bearer request-token", "X-Internal": "blocked"},
        '{"limit": 10}',
    ))
    assert result == {"ok": True, "status": 200, "data": {"accepted": True}}
    assert calls == [("https://api.example.test/v1/import", {
        "method": "POST",
        "headers": {"authorization": "Bearer request-token"},
        "redirect": "manual",
        "body": '{"limit": 10}',
    }, 9)]

    for target in (
            "http://api.example.test/v1",
            "https://user@api.example.test/v1",
            "https://api.example.test.evil/v1"):
        denied = asyncio.run(runtime.outbound_json(target))
        assert denied["ok"] is False
    assert len(calls) == 1


def test_manifest_bounded_text_responses_support_package_feed_readers():
    calls = []

    async def fetch(resource, options, timeout):
        calls.append((resource, options, timeout))
        return _Response(200, "<rss><channel><title>Updates</title></channel></rss>")

    runtime = _runtime(fetch)
    result = asyncio.run(runtime.outbound_text(
        "https://api.example.test/feed.xml", headers={"Accept": "application/rss+xml"}))
    assert result == {
        "ok": True,
        "status": 200,
        "data": "<rss><channel><title>Updates</title></channel></rss>",
    }
    assert calls == [("https://api.example.test/feed.xml", {
        "method": "GET",
        "headers": {"accept": "application/rss+xml"},
        "redirect": "manual",
    }, 9)]


def test_redirects_and_oversized_payloads_fail_closed():
    responses = [_Response(302, ""), _Response(200, "x" * (2 * 1024 * 1024 + 1))]

    async def fetch(_resource, _options, _timeout):
        return responses.pop(0)

    runtime = _runtime(fetch)
    redirected = asyncio.run(runtime.outbound_json("https://api.example.test/next"))
    assert redirected == {
        "ok": False, "status": 302, "error": "outbound_redirect_rejected"}
    oversized_response = asyncio.run(
        runtime.outbound_json("https://api.example.test/large"))
    assert oversized_response == {
        "ok": False, "status": 200, "error": "outbound_response_too_large"}
    oversized_request = asyncio.run(runtime.outbound_json(
        "https://api.example.test/upload", "POST", body="x" * (128 * 1024 + 1)))
    assert oversized_request == {
        "ok": False, "error": "outbound_body_too_large"}


def test_a_get_follows_redirects_that_stay_on_allowed_origins():
    responses = {
        "https://api.example.test/blog/": _Response(308, "", "/blog"),
        "https://api.example.test/blog": _Response(
            301, "", "https://feeds.example.test/blog.xml"),
        "https://feeds.example.test/blog.xml": _Response(200, "<rss/>"),
    }
    calls = []

    async def fetch(resource, options, _timeout):
        calls.append((resource, dict(options["headers"])))
        return responses[resource]

    runtime = _runtime(fetch, ("https://api.example.test", "https://feeds.example.test"))
    result = asyncio.run(runtime.outbound_text(
        "https://api.example.test/blog/", headers={"Authorization": "Bearer t"}))
    assert result == {"ok": True, "status": 200, "data": "<rss/>"}
    assert [call[0] for call in calls] == [
        "https://api.example.test/blog/", "https://api.example.test/blog",
        "https://feeds.example.test/blog.xml"]
    assert calls[1][1] == {"authorization": "Bearer t"}
    assert calls[2][1] == {}


def test_a_pagination_link_header_reaches_the_package():
    link = '<https://api.example.test/a?max_id=9>; rel="next"'

    async def fetch(_resource, _options, _timeout):
        return _Response(200, "[]", link=link)

    assert asyncio.run(_runtime(fetch).outbound_text("https://api.example.test/a")) == {
        "ok": True, "status": 200, "data": "[]", "link": link}


def test_redirects_off_the_allowlist_or_past_the_hop_limit_fail_closed():
    async def off_origin(resource, _options, _timeout):
        return _Response(302, "", "https://other.example.test/elsewhere")

    runtime = _runtime(off_origin)
    assert asyncio.run(runtime.outbound_text("https://api.example.test/a")) == {
        "ok": False, "status": 302, "error": "outbound_redirect_rejected"}

    async def downgrade(resource, _options, _timeout):
        return _Response(301, "", "http://api.example.test/a")

    assert asyncio.run(_runtime(downgrade).outbound_text(
        "https://api.example.test/a"))["error"] == "outbound_redirect_rejected"

    loops = []

    async def loop(resource, _options, _timeout):
        loops.append(resource)
        return _Response(307, "", "/again")

    assert asyncio.run(_runtime(loop).outbound_text(
        "https://api.example.test/a"))["error"] == "outbound_redirect_rejected"
    assert len(loops) == 4

    posts = []

    async def post(resource, _options, _timeout):
        posts.append(resource)
        return _Response(308, "", "/other")

    assert asyncio.run(_runtime(post).outbound_json(
        "https://api.example.test/a", "POST", body="{}"))["error"] == \
        "outbound_redirect_rejected"
    assert posts == ["https://api.example.test/a"]
