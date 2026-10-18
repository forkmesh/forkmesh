import asyncio
import base64
import json
import os
import subprocess
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import directory_about  # noqa: E402
import repo_projects  # noqa: E402


GIT_ENV = {
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_AUTHOR_NAME": "seed",
    "GIT_AUTHOR_EMAIL": "seed@example.test",
    "GIT_COMMITTER_NAME": "seed",
    "GIT_COMMITTER_EMAIL": "seed@example.test",
    "PATH": os.environ.get("PATH", ""),
}

PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 64
SVG = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">'
       '<circle cx="32" cy="32" r="30" fill="#dc2626"/></svg>')
LEAF_README = """# Bolt-C - PR Readiness Checker

**Live Site:** [https://leaf.example.net/](https://leaf.example.net/)<br>
**Source Code:** [https://code.example.org/Bolt/Bolt-C](https://code.example.org/Bolt/Bolt-C)

<p align="center"><img src="logo.png" alt="bolt-c logo" width="160"></p>

A simple one-page application to track and monitor **GitHub** Pull Request
readiness status. See [the docs](docs/README.md).

## Quick Deploy
"""
CORE_ABOUT = {
    "name": "Bolt B",
    "about": "Bug Logging Tool: gamified crowd-sourced QA testing.",
    "website": "https://example.net/",
    "links": [
        {"label": "Contributing", "url": "https://code.example.org/c"},
        {"label": "Bad", "url": "javascript:alert(1)"},
        {"label": "Creds", "url": "https://user:pw@example.com/"},
        {"url": "https://docs.example.com/guide"},
        {"label": "Dup", "url": "https://code.example.org/c"},
    ],
    "topics": ["Security", "bug bounty", "bad topic!", "security", 7],
}


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, env=GIT_ENV,
                          capture_output=True, check=True)


@pytest.fixture()
def monorepo(tmp_path):
    work = tmp_path / "work"
    (work / "bolt-c").mkdir(parents=True)
    (work / "bolt-c" / "README.md").write_text(LEAF_README)
    (work / "bolt-c" / "logo.svg").write_text(SVG)
    (work / "bolt-c" / "logo.png").write_bytes(PNG)
    (work / "bolt-b" / ".forkmesh").mkdir(parents=True)
    (work / "bolt-b" / ".forkmesh" / "info.json").write_text(
        json.dumps(CORE_ABOUT))
    (work / "bolt-b" / "logo.png").write_bytes(PNG)
    (work / "bolt-b" / "README.md").write_text("# Core\n\nCore readme.\n")
    (work / "broken" / ".forkmesh").mkdir(parents=True)
    (work / "broken" / ".forkmesh" / "info.json").write_text("{not json")
    (work / "broken" / "README.md").write_text(
        "# Broken\n\nThis folder has a broken about file but a readme.\n")
    (work / "assets").mkdir()
    (work / "assets" / "logo.svg").write_text(SVG)
    (work / "evil").mkdir()
    (work / "evil" / ".forkmesh").mkdir()
    (work / "evil" / ".forkmesh" / "info.json").write_text("{}")
    (work / "evil" / "logo.svg").write_text(
        '<!DOCTYPE svg [<!ENTITY x "y">]><svg></svg>')
    (work / "evil" / "logo.png").write_text("not a png")
    git(work, "init", "-q", "-b", "main")
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", "seed")
    commit = git(work, "rev-parse", "HEAD").stdout.decode().strip()
    return work, commit


class FakeHeaders:
    def __init__(self, values):
        self.values = {k.lower(): v for k, v in values.items()}

    def get(self, name):
        return self.values.get(name.lower())


class FakeRequest:
    def __init__(self, url, method="GET", headers=None):
        self.url = url
        self.method = method
        self.headers = FakeHeaders(headers or {})


class FakeResponse:
    def __init__(self, status, data=None):
        self.status = status
        self._data = data

    async def json(self):
        return self._data


class FakeJsRequest:
    @staticmethod
    def new(url, init):
        return FakeRequest(url, init["method"], init["headers"])


class FakeJsResponse:
    @staticmethod
    def new(body, init):
        return {"status": init["status"], "body": body,
                "headers": init["headers"]}


class FakeUint8Array:
    @staticmethod
    def new(value):
        return bytes(value)


def blob_payload(work, commit, path):
    result = subprocess.run(["git", "show", commit + ":" + path], cwd=work,
                            env=GIT_ENV, capture_output=True)
    if result.returncode != 0:
        return None
    raw = result.stdout
    try:
        return {"ok": True, "encoding": "utf8", "content": raw.decode("utf-8"),
                "size": len(raw), "truncated": False}
    except UnicodeDecodeError:
        return {"ok": True, "encoding": "base64",
                "content": base64.b64encode(raw).decode(), "size": len(raw),
                "truncated": False}


def harness(monorepo, private=False, hosted_account="", projects=None,
            short=None):
    work, commit = monorepo
    calls = {"proxy": [], "hosted": []}

    async def serve(request, owner, repo, operation):
        parsed = urlparse(request.url)
        assert parsed.path == "/api/repo/%s/%s/blobs" % (
            request.public_owner, repo)
        query = parse_qs(parsed.query)
        ref = query.get("ref", ["main"])[0]
        resolved = subprocess.run(
            ["git", "rev-parse", ref + "^{commit}"], cwd=work, env=GIT_ENV,
            capture_output=True)
        if resolved.returncode != 0:
            return FakeResponse(404, {"ok": False})
        sha = resolved.stdout.decode().strip()
        return FakeResponse(200, {
            "ok": True, "commit": sha,
            "blobs": {path: blob_payload(work, sha, path)
                      for path in query.get("path", [])}})

    async def proxy(env, request, owner, repo, operation):
        calls["proxy"].append((owner, repo, operation, request.url))
        request.public_owner = urlparse(request.url).path.split("/")[3]
        return await serve(request, owner, repo, operation)

    async def hosted(env, request, owner, repo, operation):
        calls["hosted"].append((owner, repo, operation,
                                request.headers.get("cookie")))
        if not hosted_account or request.headers.get("cookie") != (
                "fm_session=" + hosted_account):
            return FakeResponse(404, {"error": "not_found"})
        request.public_owner = urlparse(request.url).path.split("/")[3]
        return await serve(request, owner, repo, operation)

    async def is_private(env, owner, repo):
        return private

    runtime = {
        "json_response": lambda data, status=200, cache_control=None: {
            "status": status, "data": data, "cache": cache_control},
        "method_name": lambda request: request.method,
        "_repo_is_private": is_private,
        "_https_mirror_proxy": proxy,
        "hosted_private_read": hosted,
        "JsRequest": FakeJsRequest,
        "JsResponse": FakeJsResponse,
        "Uint8Array": FakeUint8Array,
        "_to_js": lambda value: value,
        "to_js": lambda value: value,
    }
    for name, value in runtime.items():
        setattr(directory_about, name, value)
    directory_about.clear_cache()
    calls["commit"] = commit
    calls["projects"] = projects
    calls["short"] = short
    return calls


def call(calls, path, action="dir-about", ref="main", owner="acme-bolt",
         route_owner="acme-bolt", headers=None, method="GET"):
    url = "https://git.example.test/api/repo/%s/Bolt/%s?path=%s&ref=%s" % (
        route_owner, action, path, ref)
    request = FakeRequest(url, method, headers)

    async def projects():
        return calls["projects"] or {}

    async def short():
        return calls["short"] or {}

    return asyncio.run(directory_about.route(
        object(), request, owner, "Bolt", action,
        projects=projects, short=short))


def test_parse_about_cleans_and_validates_fields():
    parsed = directory_about.parse_about(json.dumps(CORE_ABOUT))
    assert parsed["name"] == "Bolt B"
    assert parsed["description"].startswith("Bug Logging Tool")
    assert parsed["website"] == "https://example.net/"
    assert parsed["links"] == [
        {"label": "Contributing", "url": "https://code.example.org/c"},
        {"label": "docs.example.com",
         "url": "https://docs.example.com/guide"},
    ]
    assert parsed["topics"] == ["security", "bug-bounty"]


def test_parse_about_caps_lengths_and_strips_control_characters():
    parsed = directory_about.parse_about(json.dumps({
        "name": "x" * 500,
        "description": "line\none\x00two " + "y" * 600,
        "homepage": "http://example.com/" + "a" * 300,
        "links": [{"label": "L%d" % i, "url": "https://e.test/%d" % i}
                  for i in range(30)],
        "tags": ["t%d" % i for i in range(40)],
    }))
    assert len(parsed["name"]) == directory_about.MAX_NAME
    assert "\n" not in parsed["description"]
    assert "\x00" not in parsed["description"]
    assert len(parsed["description"]) <= directory_about.MAX_DESCRIPTION
    assert parsed["website"] == ""
    assert len(parsed["links"]) == directory_about.MAX_LINKS
    assert len(parsed["topics"]) == directory_about.MAX_TOPICS


@pytest.mark.parametrize("url", [
    "javascript:alert(1)", "data:text/html,hi", "ftp://example.com",
    "https://", "https://a b.com", "//example.com", "https://u:p@e.com",
    "https://example.com\\@evil.com", "vbscript:x", 42, None,
])
def test_clean_url_rejects_unsafe_values(url):
    assert directory_about.clean_url(url, 400) == ""


def test_parse_about_rejects_non_objects_and_oversized_files():
    assert directory_about.parse_about("[]") is None
    assert directory_about.parse_about("{bad") is None
    big = json.dumps({"about": "x" * (directory_about.MAX_ABOUT_BYTES + 1)})
    assert directory_about.parse_about(big) is None


@pytest.mark.parametrize("path,expected", [
    ("bolt-c", "bolt-c"), ("/bolt-c/", "bolt-c"),
    ("./a//b", "a/b"), ("", ""), ("a/../b", None), (".git", None),
    ("a\\b", None), ("a\x00b", None), ("x" * 2000, None),
])
def test_clean_path(path, expected):
    assert directory_about.clean_path(path) == expected


def test_readme_summary_skips_headings_labels_html_and_badges():
    summary = directory_about.readme_summary(LEAF_README)
    assert summary["website"] == "https://leaf.example.net/"
    assert summary["description"] == (
        "A simple one-page application to track and monitor GitHub Pull "
        "Request readiness status. See the docs.")
    flutter = ("# ACME Bolt\n\n`Report Bugs!`\n\n"
               "[![Build](https://x/badge.svg)](https://x)\n\n"
               "```\ncode block that is long enough to count\n```\n\n"
               "Title\n=====\n\n- a list item that is long enough\n\n"
               "The real first paragraph of this project.\n")
    assert directory_about.readme_summary(flutter)["description"] == (
        "The real first paragraph of this project.")


def test_readme_summary_truncates_on_a_word():
    text = "Intro " + "word " * 200
    description = directory_about.readme_summary(text)["description"]
    assert len(description) <= directory_about.MAX_DESCRIPTION
    assert description.endswith("…")


def test_logo_type_checks_content():
    assert directory_about.logo_type("logo.svg", SVG.encode()) == (
        "image/svg+xml")
    assert directory_about.logo_type(
        "logo.svg", b'<?xml version="1.0"?>\n<!-- x -->' + SVG.encode()) == (
        "image/svg+xml")
    assert directory_about.logo_type("logo.png", PNG) == "image/png"
    assert directory_about.logo_type("logo.png", b"GIF89a") == ""
    assert directory_about.logo_type("logo.svg", b"<html><svg></svg>") == ""
    assert directory_about.logo_type(
        "logo.svg", b'<!DOCTYPE svg [<!ENTITY x "y">]><svg/>') == ""
    assert directory_about.logo_type(
        "logo.svg", b"<svg>" + b" " * directory_about.MAX_LOGO_BYTES) == ""


def test_project_name_needs_matching_repo_and_owner():
    projects = repo_projects.parse(json.dumps({
        "repo": "Bolt", "table": "bolt-c\tBolt-C\t\n"}))
    short = {"short": True, "owner": "acme-bolt"}
    assert directory_about.project_name(
        projects, short, ("node1", "acme-bolt"), "bolt", "bolt-c") == (
        "Bolt-C")
    assert directory_about.project_name(
        projects, short, ("other",), "Bolt", "bolt-c") == ""
    assert directory_about.project_name(
        projects, short, ("acme-bolt",), "Other", "bolt-c") == ""
    assert directory_about.project_name(
        projects, short, ("acme-bolt",), "Bolt", "bolt-b") == ""


def test_directory_with_about_file(monorepo):
    calls = harness(monorepo)
    result = call(calls, "bolt-b")
    assert result["status"] == 200
    about = result["data"]["about"]
    assert result["data"]["commit"] == calls["commit"]
    assert about["source"] == "file"
    assert about["name"] == "Bolt B"
    assert about["descriptionSource"] == "about"
    assert about["website"] == "https://example.net/"
    assert about["topics"] == ["security", "bug-bounty"]
    assert about["logo"] == {
        "path": "bolt-b/logo.png", "type": "image/png",
        "url": "/api/repo/acme-bolt/Bolt/dir-logo?path=bolt-b&ref="
               + calls["commit"]}
    assert about["readme"] == "bolt-b/README.md"
    assert result["cache"] == "no-cache"
    owner, repo, operation, url = calls["proxy"][0]
    assert operation == "blobs"
    query = parse_qs(urlparse(url).query)
    assert "bolt-b/.forkmesh/info.json" in query["path"]
    assert "bolt-b/logo.svg" in query["path"]


def test_directory_without_about_file_is_unchanged(monorepo):
    calls = harness(monorepo)
    result = call(calls, "assets")
    assert result["status"] == 200
    assert result["data"]["about"] is None


def test_project_folder_gets_about_from_readme_and_logo(monorepo):
    calls = harness(
        monorepo,
        projects=repo_projects.parse(json.dumps({
            "repo": "Bolt", "table": "bolt-c\tBolt-C\t\n"})),
        short={"short": True, "owner": "acme-bolt"})
    about = call(calls, "bolt-c")["data"]["about"]
    assert about["source"] == "project"
    assert about["name"] == "Bolt-C"
    assert about["descriptionSource"] == "readme"
    assert about["description"].startswith("A simple one-page application")
    assert about["website"] == "https://leaf.example.net/"
    assert about["websiteSource"] == "readme"
    assert about["logo"]["type"] == "image/svg+xml"
    assert about["logo"]["path"] == "bolt-c/logo.svg"


def test_invalid_about_file_falls_back_to_readme(monorepo):
    calls = harness(monorepo)
    about = call(calls, "broken")["data"]["about"]
    assert about["invalid"] is True
    assert about["name"] == "broken"
    assert about["descriptionSource"] == "readme"


def test_unsafe_logos_are_not_offered(monorepo):
    calls = harness(monorepo)
    assert call(calls, "evil")["data"]["about"]["logo"] is None
    assert call(calls, "evil", action="dir-logo")["status"] == 404


def test_bad_path_and_ref_are_rejected(monorepo):
    calls = harness(monorepo)
    assert call(calls, "a/../b")["data"]["error"] == "invalid_path"
    assert call(calls, "bolt-c", ref="-x")["data"]["error"] == "invalid_ref"
    assert call(calls, "bolt-c", ref="a..b")["status"] == 400
    assert call(calls, "bolt-c", method="POST")["status"] == 405
    assert not calls["proxy"]


def test_missing_ref_or_directory_is_not_found(monorepo):
    calls = harness(monorepo)
    assert call(calls, "bolt-c", ref="nope")["status"] == 404


def test_full_commit_reads_are_cached_for_public_repos(monorepo):
    calls = harness(monorepo)
    first = call(calls, "bolt-b", ref=calls["commit"])
    second = call(calls, "bolt-b", ref=calls["commit"])
    assert first["data"] == second["data"]
    assert first["cache"] == "public, max-age=300"
    assert len(calls["proxy"]) == 1


def test_private_repo_needs_a_reader_session(monorepo):
    calls = harness(monorepo, private=True, hosted_account="alice")
    denied = call(calls, "bolt-b", ref=calls["commit"])
    assert denied["status"] == 404
    assert not calls["proxy"]
    allowed = call(calls, "bolt-b", ref=calls["commit"],
                   headers={"cookie": "fm_session=alice"})
    assert allowed["status"] == 200
    assert allowed["data"]["about"]["name"] == "Bolt B"
    assert allowed["cache"].startswith("private, no-store")
    again = call(calls, "bolt-b", ref=calls["commit"])
    assert again["status"] == 404
    assert [entry[3] for entry in calls["hosted"]] == [
        None, "fm_session=alice", None]


def test_private_repo_without_hosted_read_is_not_found(monorepo):
    calls = harness(monorepo, private=True)
    directory_about.hosted_private_read = None
    assert call(calls, "bolt-b")["status"] == 404
    assert not calls["proxy"]


def test_logo_is_served_as_a_sandboxed_image(monorepo):
    calls = harness(monorepo)
    result = call(calls, "bolt-c", action="dir-logo", ref=calls["commit"])
    assert result["status"] == 200
    assert result["body"] == SVG.encode()
    headers = result["headers"]
    assert headers["content-type"] == "image/svg+xml"
    assert headers["x-content-type-options"] == "nosniff"
    assert "sandbox" in headers["content-security-policy"]
    assert "default-src 'none'" in headers["content-security-policy"]
    assert headers["content-disposition"] == 'inline; filename="logo.svg"'
    assert headers["cache-control"] == "public, max-age=300"
    png = call(calls, "bolt-b", action="dir-logo")
    assert png["headers"]["content-type"] == "image/png"
    assert png["body"] == PNG
    call(calls, "bolt-c", action="dir-logo", ref=calls["commit"])
    assert len(calls["proxy"]) == 2


def test_logo_for_private_repo_follows_permissions(monorepo):
    calls = harness(monorepo, private=True, hosted_account="alice")
    assert call(calls, "bolt-c", action="dir-logo")["status"] == 404
    ok = call(calls, "bolt-c", action="dir-logo",
              headers={"cookie": "fm_session=alice"})
    assert ok["status"] == 200
    assert ok["headers"]["cache-control"].startswith("private, no-store")


def test_organization_route_owner_is_kept_for_reads_and_logo_url(monorepo):
    calls = harness(monorepo)
    result = call(calls, "bolt-b", owner="node1", route_owner="acme-bolt")
    assert result["data"]["about"]["logo"]["url"].startswith(
        "/api/repo/acme-bolt/Bolt/dir-logo?")
    owner, _repo, _op, url = calls["proxy"][0]
    assert owner == "node1"
    assert urlparse(url).path == "/api/repo/acme-bolt/Bolt/blobs"


def test_app_runtime_routes_directory_about_actions():
    source = (ROOT / "src" / "app_runtime.py").read_text()
    assert 'if action in ("dir-about", "dir-logo"):' in source
    assert "directory_about.route(" in source
    assert "projects=self._repo_projects_setting" in source
