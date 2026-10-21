import asyncio
import base64
import json
import re
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = Path(__file__).resolve().parent
for path in (SRC, TESTS, ROOT / "host"):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import repo_media_api as media  # noqa: E402
from _dashboard_bundle import assembled_dashboard_js  # noqa: E402
from forkmesh_host import repo_ssr_issue  # noqa: E402


PART = media.MEDIA_PART_BYTES
MP4_HEAD = b"\x00\x00\x00\x18ftypmp42"


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0282_repo_uploads.sql")
            .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
        self.clock = 1_800_000_000_000
        self.ids = 0
        self.public = {"alice/demo"}
        self.accounts = {
            "alice": {"name": "alice", "email_verified": True},
            "bob": {"name": "bob", "email_verified": True},
            "eve": {"name": "eve", "email_verified": False},
        }
        self.use("GET", None)

    def use(self, method, actor, data=None, headers=None):
        self._method = method
        self._actor = actor
        self._headers = {k.lower(): v for k, v in (headers or {}).items()}
        self._data = data
        return self

    def method(self):
        return self._method

    def now(self):
        return self.clock

    def new_id(self):
        self.ids += 1
        return "%032x" % self.ids

    def segment(self, value):
        return value if re.fullmatch(r"[A-Za-z0-9._-]+", value or "") else ""

    def header(self, name):
        return self._headers.get(name.lower(), "")

    async def ensure_schema(self):
        return None

    async def json_body(self, limit):
        raw = json.dumps(self._data or {})
        if len(raw) > limit:
            return None, "payload_too_large"
        return dict(self._data or {}), ""

    async def session(self, data=None):
        record = self.accounts.get(self._actor or "")
        return ("bi:" + self._actor, record) if record else ("", None)

    async def public_repository(self, owner, repo):
        key = (owner + "/" + repo).lower()
        return "repo:" + key if key in self.public else ""

    async def seal(self, value):
        return json.dumps(value)

    async def open(self, value):
        return json.loads(value)

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)

    def response(self, data, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": data, "headers": extra_headers or {}}

    def binary(self, body, status, headers):
        return {"status": status, "body": bytes(body), "headers": headers}


def run(coro):
    return asyncio.run(coro)


def call(runtime, method, actor, path, data=None, headers=None):
    return run(media.handle(runtime.use(method, actor, data, headers), path))


def payload(size):
    body = MP4_HEAD + bytes((index * 7) % 251 for index in range(size - len(MP4_HEAD)))
    return body[:size]


def uploaded(runtime, size=PART * 2 + 1000, actor="alice", mime="video/mp4"):
    body = payload(size)
    created = call(runtime, "POST", actor, "/api/repo/alice/demo/media", {
        "name": "clip [1].mp4", "mime": mime, "size": size,
        "parts": (size + PART - 1) // PART})
    assert created["status"] == 201, created
    mid = created["data"]["media"]["id"]
    for index, start in enumerate(range(0, size, PART)):
        result = call(runtime, "PUT", actor,
                      "/api/repo/alice/demo/media/%s/parts/%d" % (mid, index),
                      {"data": base64.b64encode(body[start:start + PART]).decode()})
        assert result["status"] == 200, result
    finished = call(runtime, "POST", actor, "/api/repo/alice/demo/media/%s/finish" % mid)
    assert finished["status"] == 200, finished
    return mid, body, finished["data"]["media"]


def test_upload_then_serve_whole_file_and_ranges():
    runtime = FakeRuntime()
    mid, body, info = uploaded(runtime)
    assert info["state"] == "ready"
    assert info["url"] == "/api/repo/alice/demo/media/%s.mp4" % mid

    whole = call(runtime, "GET", None, info["url"])
    assert whole["status"] == 200 and whole["body"] == body
    assert whole["headers"]["content-type"] == "video/mp4"
    assert whole["headers"]["accept-ranges"] == "bytes"
    assert whole["headers"]["x-content-type-options"] == "nosniff"
    assert whole["headers"]["content-disposition"].startswith("inline;")

    across = call(runtime, "GET", None, info["url"],
                  headers={"Range": "bytes=%d-%d" % (PART - 10, PART + 9)})
    assert across["status"] == 206
    assert across["body"] == body[PART - 10:PART + 10]
    assert across["headers"]["content-range"] == "bytes %d-%d/%d" % (
        PART - 10, PART + 9, len(body))

    tail = call(runtime, "GET", None, info["url"], headers={"Range": "bytes=-5"})
    assert tail["status"] == 206 and tail["body"] == body[-5:]

    beyond = call(runtime, "GET", None, info["url"],
                  headers={"Range": "bytes=%d-" % len(body)})
    assert beyond["status"] == 416
    assert beyond["headers"]["content-range"] == "bytes */%d" % len(body)

    head = call(runtime, "HEAD", None, info["url"])
    assert head["status"] == 200 and head["body"] == b""
    assert head["headers"]["content-length"] == str(len(body))


def test_open_ended_ranges_are_capped():
    size = media.MEDIA_RANGE_MAX_BYTES + PART
    runtime = FakeRuntime()
    mid, body, info = uploaded(runtime, size=size)
    result = call(runtime, "GET", None, info["url"], headers={"Range": "bytes=0-"})
    assert result["status"] == 206
    assert len(result["body"]) == media.MEDIA_RANGE_MAX_BYTES
    assert result["headers"]["content-range"] == "bytes 0-%d/%d" % (
        media.MEDIA_RANGE_MAX_BYTES - 1, size)


def test_byte_range_parser():
    assert media.byte_range("", 100) == ("full", 0, 99)
    assert media.byte_range("bytes=10-19", 100) == ("partial", 10, 19)
    assert media.byte_range("bytes=90-", 100) == ("partial", 90, 99)
    assert media.byte_range("bytes=-200", 100) == ("partial", 0, 99)
    assert media.byte_range("bytes=150-", 100)[0] == "unsatisfiable"
    assert media.byte_range("bytes=20-10", 100) == ("full", 0, 99)
    assert media.byte_range("items=0-1", 100) == ("full", 0, 99)


def test_first_part_must_match_the_declared_type():
    runtime = FakeRuntime()
    created = call(runtime, "POST", "alice", "/api/repo/alice/demo/media", {
        "mime": "video/webm", "size": 100, "parts": 1})
    mid = created["data"]["media"]["id"]
    result = call(runtime, "PUT", "alice", "/api/repo/alice/demo/media/%s/parts/0" % mid,
                  {"data": base64.b64encode(payload(100)).decode()})
    assert result["status"] == 415
    assert result["data"]["error"] == "media_type_mismatch"
    assert media.sniff_matches("video/webm", b"\x1a\x45\xdf\xa3rest")
    assert media.sniff_matches("video/quicktime", b"\x00\x00\x00\x08wide")
    assert not media.sniff_matches("video/mp4", b"<html><script>")


def test_create_refusals():
    runtime = FakeRuntime()
    path = "/api/repo/alice/demo/media"
    assert call(runtime, "POST", None, path, {"mime": "video/mp4", "size": 1, "parts": 1})["status"] == 401
    assert call(runtime, "POST", "alice", path, {"mime": "image/svg+xml", "size": 1, "parts": 1})["status"] == 415
    too_big = media.MEDIA_MAX_BYTES + 1
    assert call(runtime, "POST", "alice", path, {
        "mime": "video/mp4", "size": too_big, "parts": (too_big + PART - 1) // PART})["status"] == 413
    assert call(runtime, "POST", "alice", path, {"mime": "video/mp4", "size": 10, "parts": 2})["status"] == 400
    assert call(runtime, "POST", "eve", path, {"mime": "video/mp4", "size": 10, "parts": 1})["data"]["error"] == "email_unverified"
    assert call(runtime, "POST", "alice", "/api/repo/alice/secret/media", {
        "mime": "video/mp4", "size": 10, "parts": 1})["data"]["error"] == "repository_unavailable"


def test_daily_quota():
    runtime = FakeRuntime()
    for _ in range(media.MEDIA_DAILY_MAX_COUNT):
        assert call(runtime, "POST", "alice", "/api/repo/alice/demo/media", {
            "mime": "video/mp4", "size": 10, "parts": 1})["status"] == 201
    refused = call(runtime, "POST", "alice", "/api/repo/alice/demo/media", {
        "mime": "video/mp4", "size": 10, "parts": 1})
    assert refused["status"] == 429 and refused["data"]["error"] == "media_quota_count"
    runtime.clock += media.MEDIA_DAY_MS + 1
    assert call(runtime, "POST", "alice", "/api/repo/alice/demo/media", {
        "mime": "video/mp4", "size": 10, "parts": 1})["status"] == 201


def test_only_the_uploader_writes_and_unfinished_media_is_not_served():
    runtime = FakeRuntime()
    created = call(runtime, "POST", "alice", "/api/repo/alice/demo/media", {
        "mime": "video/mp4", "size": 100, "parts": 1})
    mid = created["data"]["media"]["id"]
    part = {"data": base64.b64encode(payload(100)).decode()}
    assert call(runtime, "PUT", "bob", "/api/repo/alice/demo/media/%s/parts/0" % mid, part)["status"] == 403
    assert call(runtime, "GET", None, created["data"]["media"]["url"])["status"] == 404
    assert call(runtime, "POST", "alice", "/api/repo/alice/demo/media/%s/finish" % mid)["data"]["error"] == "upload_incomplete"


def test_media_of_a_repository_made_private_stops_being_served():
    runtime = FakeRuntime()
    _mid, _body, info = uploaded(runtime, size=200)
    runtime.public.clear()
    assert call(runtime, "GET", None, info["url"])["status"] == 404


def test_uploader_removes_media_and_sweep_drops_abandoned_uploads():
    runtime = FakeRuntime()
    mid, _body, info = uploaded(runtime, size=200)
    assert call(runtime, "DELETE", "bob", "/api/repo/alice/demo/media/" + mid)["status"] == 403
    assert call(runtime, "DELETE", "alice", "/api/repo/alice/demo/media/" + mid)["status"] == 200
    assert call(runtime, "GET", None, info["url"])["status"] == 404
    assert runtime.db.execute("SELECT COUNT(*) FROM repo_upload_parts").fetchone()[0] == 0

    created = call(runtime, "POST", "alice", "/api/repo/alice/demo/media", {
        "mime": "video/mp4", "size": PART + 1, "parts": 2})
    stale = created["data"]["media"]["id"]
    call(runtime, "PUT", "alice", "/api/repo/alice/demo/media/%s/parts/0" % stale,
         {"data": base64.b64encode(payload(PART)).decode()})
    runtime.clock += media.MEDIA_UPLOAD_TTL_MS + 1
    assert run(media.sweep(runtime)) == 1
    assert runtime.db.execute(
        "SELECT state FROM repo_uploads WHERE media_id=?", (stale,)).fetchone()[0] == "removed"
    assert runtime.db.execute("SELECT COUNT(*) FROM repo_upload_parts").fetchone()[0] == 0


def test_renderers_turn_uploaded_media_links_into_video():
    url = "/api/repo/alice/demo/media/%032x.mp4" % 7
    html = repo_ssr_issue.image_html("clip", url, None)
    assert html.startswith('<video src="%s" controls preload="metadata"' % url)
    assert repo_ssr_issue.image_html("x", "/api/repo/a/b/media/zz.mp4", None).startswith("<img")
    js = assembled_dashboard_js()
    assert "MD_VIDEO_URL.test(String(url || \"\").trim())" in js
    assert "\\/media\\/[0-9a-f]{32}\\.(?:mp4|m4v|webm|mov)$/i" in js


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_web_renderer_matches_the_server_renderer_for_video():
    js = assembled_dashboard_js()
    constant = re.search(r"\n  const MD_VIDEO_URL =\n.*?;\n  const MD_VIDEO_CLASS = .*?;\n", js, re.S).group(0)
    start = js.index("\n  function mdImageHtml(")
    end = js.index("\n  }\n", start) + 4
    script = constant + js[start:end] + """
const url = "/api/repo/alice/demo/media/%s.webm";
process.stdout.write(mdImageHtml("clip", url, null));
""" % ("%032x" % 9)
    result = subprocess.run(["node"], input=script, capture_output=True, text=True, check=True)
    url = "/api/repo/alice/demo/media/%032x.webm" % 9
    assert result.stdout == repo_ssr_issue.image_html("clip", url, None)


def test_route_is_dispatched_and_swept():
    runtime_src = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    assert "repo_media_api.matches(url.path)" in runtime_src
    assert 'repo_media_api = _LazyModule("repo_media_api")' in entry
    assert "await repo_media_api.sweep(_RepoMediaRuntime(env))" in entry


def test_upload_tables_do_not_reuse_an_existing_table_name():
    import schema
    names = [match.group(1) for statement in schema.SCHEMA_STATEMENTS
             for match in [re.match(r"\s*CREATE TABLE IF NOT EXISTS (\w+)", statement)] if match]
    assert names.count("repo_uploads") == 1 and names.count("repo_upload_parts") == 1
    assert sorted({name for name in names if names.count(name) > 1}) == []
