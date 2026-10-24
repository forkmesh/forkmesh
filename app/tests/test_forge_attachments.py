#!/usr/bin/env python3
import base64
import io
import json
from email.message import Message
from pathlib import Path
import sys
from urllib.request import HTTPSHandler
from urllib.response import addinfourl

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import forge_attachments as fa  # noqa: E402

T0 = 1_800_000_000_000
PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 40
JPEG = b"\xff\xd8\xff\xe0" + b"\x01" * 40
SVG = b'<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg"></svg>'
UUID = "46d2bdf0-a7d0-4ae8-bc7b-323f7e73b588"
ASSET = "https://github.com/user-attachments/assets/" + UUID
OLD = "https://user-images.githubusercontent.com/41267675/150181357-9bc11029-61a0-41f2-a2d4-16069bcc6078.png"
FILE = "https://github.com/user-attachments/files/16631182/results_direct.csv"


def jwt(exp):
    part = base64.urlsafe_b64encode(json.dumps({"exp": exp}).encode()).decode().rstrip("=")
    return "eyJhbGciOiJIUzI1NiJ9.%s.c2ln" % part


PRIVATE_PATH = "/41267675/291028936-9139bcdf-4dfd-4954-9b5d-139623fb946d.jpg"
PRIVATE = "https://private-user-images.githubusercontent.com%s?jwt=%s" % (
    PRIVATE_PATH, jwt(1_700_000_000))


class Fetcher:
    def __init__(self, answers=None):
        self.answers = dict(answers or {})
        self.calls = []

    def __call__(self, url):
        self.calls.append(url)
        answer = self.answers.get(url)
        if callable(answer):
            return answer()
        if isinstance(answer, fa.Fetched):
            return answer
        if answer is None:
            return fa.Fetched(status=404, reason="http-404")
        return fa.Fetched(answer, final_url=url)


def records(*bodies):
    return {number: {"number": number, "events": [
        {"type": "open", "id": "o", "title": "t", "body": body}]}
        for number, body in enumerate(bodies, start=1)}


def run(checkout, recs, fetcher, now=T0, **options):
    options.setdefault("sleep", lambda _s: None)
    return fa.sync(checkout, recs, now, fetcher=fetcher, **options)


def index(checkout):
    return json.loads((checkout / fa.INDEX_REL).read_text())


def snapshot(checkout):
    return {str(p.relative_to(checkout)): p.read_bytes()
            for p in sorted(checkout.rglob("*")) if p.is_file()}


def test_finds_every_github_hosted_attachment_spelling_and_nothing_else():
    text = (
        "![image](%s)\n<img width=\"300\" src=\"%s\">\n[results_direct.csv](%s).\n"
        "see %s, and https://github.com/example/forum/files/8027472/flakey.zip!\n"
        "![x](https://ehgt.org/a.png) https://github.com/example/forum/issues/3 "
        "https://raw.githubusercontent.com/o/r/main/x.png "
        "<img src=\"https://objects.githubusercontent.com/github-production-repository-file-5c1aeb/"
        "1/20198649?X-Amz-Date=1&amp;response-content-disposition=attachment%%3Bfilename%%3DChen.pdf\">"
        % (ASSET, OLD, FILE, PRIVATE))
    assert fa.find_urls(text) == [
        ASSET, OLD, FILE, PRIVATE,
        "https://github.com/example/forum/files/8027472/flakey.zip",
        "https://objects.githubusercontent.com/github-production-repository-file-5c1aeb/1/20198649"
        "?X-Amz-Date=1&response-content-disposition=attachment%3Bfilename%3DChen.pdf"]
    assert fa.find_urls(None) == [] and fa.find_urls("no links") == []


def test_only_https_on_the_github_attachment_hosts_is_allowed():
    for url in (ASSET, OLD, FILE, PRIVATE,
                "https://github.com/example/forum/assets/25169478/" + UUID,
                "https://github-production-user-asset-6210df.s3.amazonaws.com/1/2.png?X=1",
                "https://objects.githubusercontent.com/github-production-repository-file-5c1aeb/1/2"):
        assert fa.allowed(url), url
    for url in ("http://github.com/user-attachments/assets/" + UUID,
                "https://github.com/example/forum/issues/1",
                "https://github.com:8443/user-attachments/assets/" + UUID,
                "https://user@user-images.githubusercontent.com/1/a.png",
                "https://avatars.githubusercontent.com/u/1",
                "https://evil-bucket.s3.amazonaws.com/x.png",
                "https://ehgt.org/a.png", "file:///etc/passwd"):
        assert not fa.allowed(url), url
    assert fa.fetch("https://ehgt.org/a.png").reason == "refused"


def test_an_expired_private_link_falls_back_to_a_fresh_public_source():
    assert fa.candidates(PRIVATE, T0) == [
        "https://user-images.githubusercontent.com" + PRIVATE_PATH,
        "https://github.com/user-attachments/assets/9139bcdf-4dfd-4954-9b5d-139623fb946d"]
    live = PRIVATE.split("?jwt=")[0] + "?jwt=" + jwt(T0 // 1000 + 3600)
    assert fa.candidates(live, T0)[0] == live
    expired = ("https://objects.githubusercontent.com/github-production-repository-file-5c1aeb/"
               "111/20198649?X-Amz-Expires=300&response-content-disposition="
               "attachment%3Bfilename%3DChen2004a.en.pdf")
    assert fa.candidates(expired, T0) == [
        "https://github.com/user-attachments/files/20198649/Chen2004a.en.pdf"]
    assert fa.candidates(expired.split("?")[0], T0) == []


def test_content_is_checked_by_its_magic_bytes():
    assert fa.sniff(PNG) == ("image/png", "png")
    assert fa.sniff(JPEG) == ("image/jpeg", "jpg")
    assert fa.sniff(b"GIF89a....") == ("image/gif", "gif")
    assert fa.sniff(b"RIFF\x00\x00\x00\x00WEBPVP8 ") == ("image/webp", "webp")
    assert fa.sniff(b"%PDF-1.7 ...") == ("application/pdf", "pdf")
    assert fa.sniff(b"PK\x03\x04rest") == ("application/zip", "zip")
    assert fa.sniff(b"\x00\x00\x00\x18ftypmp42....") == ("video/mp4", "mp4")
    assert fa.sniff(SVG) == ("image/svg+xml", "svg")
    assert fa.sniff(b"<html><script>alert(1)</script>") is None
    assert fa.sniff(b"a,b\n1,2\n", FILE) == ("text/plain", "txt")
    assert fa.sniff(b"a,b\n1,2\n", ASSET) is None
    assert fa.sniff(b"a\x00b", FILE) is None
    assert fa.sniff(b"") is None


def test_a_pass_stores_files_by_content_and_writes_a_sorted_index(tmp_path):
    recs = records("![a](%s) ![b](%s)" % (ASSET, OLD), "[csv](%s) ![b](%s)" % (FILE, OLD))
    before = json.dumps(recs, sort_keys=True)
    fetcher = Fetcher({ASSET: PNG, OLD: PNG, FILE: b"a,b\n"})
    summary = run(tmp_path, recs, fetcher)
    assert json.dumps(recs, sort_keys=True) == before
    assert summary["stored"] == 3 and summary["unavailable"] == 0
    digest = __import__("hashlib").sha256(PNG).hexdigest()
    png = {"path": ".forkmesh/attachments/%s.png" % digest, "type": "image/png", "bytes": len(PNG)}
    doc = index(tmp_path)
    assert doc["schema"] == fa.SCHEMA and doc["unavailable"] == {}
    assert doc["files"][ASSET] == png and doc["files"][OLD] == png
    assert doc["files"][FILE]["type"] == "text/plain"
    assert list(doc["files"]) == sorted(doc["files"])
    assert (tmp_path / png["path"]).read_bytes() == PNG
    assert sorted(p.name for p in (tmp_path / fa.DIR_REL).iterdir() if p.is_file()) == sorted(
        [digest + ".png", __import__("hashlib").sha256(b"a,b\n").hexdigest() + ".txt", "index.json"])
    shard = json.loads((tmp_path / fa.ISSUES_REL / "2.json").read_text())
    assert shard == {"schema": fa.ISSUE_SCHEMA, "files": {FILE: doc["files"][FILE], OLD: png}}
    settled = snapshot(tmp_path)
    again = Fetcher()
    run(tmp_path, recs, again, now=T0 + 10 ** 9)
    assert again.calls == [] and snapshot(tmp_path) == settled


def test_the_newest_issues_go_first_and_the_budget_leaves_the_rest_pending(tmp_path):
    other = "https://github.com/user-attachments/assets/" + UUID.replace("4", "5")
    recs = records("![a](%s)" % ASSET, "![b](%s)" % other)
    fetcher = Fetcher({ASSET: PNG, other: JPEG})
    summary = run(tmp_path, recs, fetcher, max_files=1)
    assert fetcher.calls == [other]
    assert summary["pending"] == 1 and summary["stopped"] == "files"
    run(tmp_path, recs, fetcher)
    assert fetcher.calls == [other, ASSET]


def test_unavailable_files_are_recorded_and_retried_only_occasionally(tmp_path):
    recs = records("![a](%s)" % ASSET)
    fetcher = Fetcher({ASSET: b"<html>login</html>"})
    run(tmp_path, recs, fetcher)
    entry = index(tmp_path)["unavailable"][ASSET]
    assert entry == {"reason": "type", "tries": 1, "nextTry": T0 + fa.RETRY_MAX_MS}
    assert not (tmp_path / fa.ISSUES_REL / "1.json").exists()
    run(tmp_path, recs, fetcher, now=T0 + fa.RETRY_MAX_MS - 1)
    assert len(fetcher.calls) == 1
    fetcher.answers[ASSET] = PNG
    run(tmp_path, recs, fetcher, now=T0 + fa.RETRY_MAX_MS)
    doc = index(tmp_path)
    assert ASSET in doc["files"] and doc["unavailable"] == {}
    missing = records("![a](%s)" % OLD)
    run(tmp_path / "b", missing, Fetcher())
    assert index(tmp_path / "b")["unavailable"][OLD] == {
        "reason": "http-404", "tries": 1, "nextTry": T0 + fa.RETRY_MS}
    run(tmp_path / "b", missing, Fetcher(), now=T0 + fa.RETRY_MS)
    assert index(tmp_path / "b")["unavailable"][OLD]["nextTry"] == T0 + 3 * fa.RETRY_MS


def test_a_rate_limit_waits_once_then_stops_without_blaming_the_file(tmp_path):
    recs = records("![a](%s)" % ASSET, "![b](%s)" % OLD)
    waits = []
    answers = iter([fa.Fetched(status=429, retry_after=5), fa.Fetched(PNG)])
    fetcher = Fetcher({OLD: lambda: next(answers),
                       ASSET: fa.Fetched(status=403, retry_after=3600)})
    summary = run(tmp_path, recs, fetcher, sleep=waits.append)
    assert waits == [5, fa.PACE_SECONDS, fa.PACE_SECONDS]
    assert summary["stopped"] == "rate-limited" and summary["pending"] == 1
    doc = index(tmp_path)
    assert OLD in doc["files"] and doc["unavailable"] == {}


def test_private_links_are_mirrored_through_their_public_copy(tmp_path):
    public = "https://user-images.githubusercontent.com" + PRIVATE_PATH
    fetcher = Fetcher({public: JPEG})
    run(tmp_path, records("![p](%s)" % PRIVATE), fetcher)
    assert fetcher.calls == [public]
    assert index(tmp_path)["files"][PRIVATE]["type"] == "image/jpeg"


class FakeHTTPS(HTTPSHandler):
    def __init__(self, routes):
        super().__init__()
        self.routes = routes
        self.seen = []

    def https_open(self, request):
        url = request.full_url
        self.seen.append(url)
        status, headers, body = self.routes.get(url, (404, {}, b""))
        message = Message()
        for key, value in headers.items():
            message[key] = value
        response = addinfourl(io.BytesIO(body), message, url, status)
        response.msg = "OK"
        return response


def test_redirects_are_followed_only_inside_the_allowlist(monkeypatch):
    bucket = "https://github-production-user-asset-6210df.s3.amazonaws.com/1/a.png?sig=1"
    handler = FakeHTTPS({ASSET: (302, {"Location": bucket}, b""),
                         bucket: (200, {"Content-Length": str(len(PNG))}, PNG)})
    monkeypatch.setattr(fa, "HANDLERS", (handler,))
    got = fa.fetch(ASSET)
    assert got.data == PNG and got.final_url == bucket
    handler.routes[ASSET] = (302, {"Location": "https://evil.example/a.png"}, b"")
    got = fa.fetch(ASSET)
    assert got.data is None and got.reason == "redirect"
    assert "https://evil.example/a.png" not in handler.seen


def test_the_size_cap_is_enforced_by_header_and_by_body(monkeypatch):
    handler = FakeHTTPS({OLD: (200, {"Content-Length": str(fa.MAX_BYTES + 1)}, b"")})
    monkeypatch.setattr(fa, "HANDLERS", (handler,))
    assert fa.fetch(OLD).reason == "too-large"
    monkeypatch.setattr(fa, "MAX_BYTES", 8)
    handler.routes[OLD] = (200, {}, PNG)
    assert fa.fetch(OLD).reason == "too-large"
    handler.routes[OLD] = (429, {"Retry-After": "7"}, b"")
    got = fa.fetch(OLD)
    assert got.rate_limited and got.retry_after == 7


def test_a_damaged_index_is_rebuilt_and_bad_rows_are_dropped(tmp_path):
    (tmp_path / fa.DIR_REL).mkdir(parents=True)
    (tmp_path / fa.INDEX_REL).write_text(json.dumps({"schema": fa.SCHEMA, "files": {
        ASSET: {"path": "../../etc/passwd", "type": "image/png", "bytes": 1}}}))
    fetcher = Fetcher({ASSET: PNG})
    run(tmp_path, records("![a](%s)" % ASSET), fetcher)
    assert fetcher.calls == [ASSET]
    assert index(tmp_path)["files"][ASSET]["path"].startswith(".forkmesh/attachments/")


@pytest.mark.parametrize("name", ["forge_attachments.py"])
def test_the_module_carries_no_comments_or_docstrings(name):
    source = (ROOT / "tools" / name).read_text()
    lines = [line.strip() for line in source.splitlines()[1:]]
    assert not [line for line in lines if line.startswith("#")]
    assert '"""' not in source
