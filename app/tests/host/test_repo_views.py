"""Pull request, issue and discussion pages from one bundle of reads
(forkmesh_host.repo_views) and their server-rendered first view.

A fake fast path answers gateway reads from an in-memory repository, so the
tests pin the exact read keys the dashboard asks for (06-repo-content.js
repoReadSeedKey), which reads are recorded, and what the page renders.
"""

from __future__ import annotations

import asyncio
import json
import shutil
import subprocess
import urllib.parse
from pathlib import Path

import pytest

from forkmesh_host import repo_ssr, repo_views
from forkmesh_host.repo_fastpath import IMMUTABLE, RepoFastPath, cache_control

MAIN = "a" * 40
LEDGER = "b" * 40
PULL7 = "d" * 40
BASE_OID = "e" * 40
HEAD_OID = "f" * 40
ROOT = Path(__file__).resolve().parents[2]


def pull_md(number, title, status="open", extra=""):
    return ("---\nschema: forkmesh-pull-v1\nnumber: %d\ntitle: %s\nbase: main\n"
            "head: feature-%d\nstatus: %s\nts: %d\nauthorName: alice\n%s---\n\n"
            "Body of pull %d.\n" % (number, title, number, status,
                                    1790000000000 + number, extra, number))


def issue_json(number, title, status="open"):
    return json.dumps({"number": number, "title": title, "status": status,
                       "authorName": "bob", "createdAt": 1789000000000,
                       "labels": ["bug"], "events": [
                           {"type": "open", "id": "o", "author": "k",
                            "body": "Issue %d body" % number},
                           {"type": "comment", "id": "c1", "author": "k2",
                            "authorName": "carol", "body": "A comment"}]})


FILES = {
    MAIN: {
        "README.md": "# hi",
        ".forkmesh/issues/open/12/issue-12.json": issue_json(12, "Open one"),
        ".forkmesh/issues/closed/5/issue-5.json": issue_json(5, "Old", "closed"),
        ".forkmesh/discussions/1/discussion.md":
            "---\ntitle: Hello\ncategory: Q&A\nauthorName: dan\nts: 1\n---\n\nHi",
        ".forkmesh/discussions/1/0001-comment.md":
            "---\nauthorName: erin\nts: 2\n---\n\nReply",
    },
    LEDGER: {
        ".forkmesh/pulls/3/pull.md": pull_md(3, "Merged thing", "merged"),
        ".forkmesh/pulls/4/pull.md": pull_md(4, "Patch pull"),
        ".forkmesh/pulls/4/changes.patch":
            "diff --git a/x.py b/x.py\n--- a/x.py\n+++ b/x.py\n@@ -1 +1 @@\n-old\n+new\n",
        ".forkmesh/pulls/4/commits.mbox": "From abc Mon Sep 17 00:00:00 2001\n",
        ".forkmesh/pulls/4/0001-comment.md":
            "---\ntype: comment\nauthorName: frank\nts: 3\n---\n\nLooks <b>good</b>",
    },
    PULL7: {
        ".forkmesh/pulls/7/pull.md": pull_md(
            7, "Branch only", extra="creationBaseOid: %s\ncreationHeadOid: %s\n"
            % (BASE_OID, HEAD_OID)),
    },
}
REFS = {"main": MAIN, "forkmesh/pulls": LEDGER, "forkmesh/pull/7": PULL7}


class FakeFastPath:
    """RepoFastPath's read surface over FILES."""

    def __init__(self, public=True):
        self.public = public
        self.urls = []

    gateway_url = staticmethod(RepoFastPath.gateway_url)

    async def node_name(self):
        return "fmnext-n9"

    async def default_branch(self, owner, repo, commit):
        return "main" if commit == MAIN else ""

    async def read(self, url, immutable=False):
        self.urls.append((url, immutable))
        parts = urllib.parse.urlsplit(url)
        op = parts.path.rsplit("/", 1)[1]
        query = urllib.parse.parse_qs(parts.query, keep_blank_values=True)
        if not self.public or "/forkmesh/forkmesh/" not in parts.path:
            return self._answer(404, {"ok": False, "error": "not_found"})
        ref = (query.get("ref") or ["main"])[0]
        commit = REFS.get(ref, ref)
        files = FILES.get(commit, {})
        if op == "tree":
            path = (query.get("path") or [""])[0].strip("/")
            prefix = path + "/" if path else ""
            names = {}
            for name in files:
                if name.startswith(prefix):
                    rest = name[len(prefix):].split("/")
                    names[rest[0]] = "tree" if len(rest) > 1 else "blob"
            if not names:
                return self._answer(404, {"ok": False, "error": "not_found"})
            data = {"ok": True, "commit": commit, "entries": [
                {"name": n, "type": t} for n, t in sorted(names.items())]}
            if path == ".forkmesh/pulls":
                data["pullRefs"] = {"7": PULL7}
            return self._answer(200, data)
        if op == "blob":
            path = query["path"][0]
            if path not in files:
                return self._answer(503, {"ok": False,
                                          "error": "mirror_unavailable"})
            return self._answer(200, {"ok": True, "commit": commit,
                                      "content": files[path]})
        if op == "blobs":
            return self._answer(200, {"blobs": {
                p: ({"content": files[p]} if p in files else None)
                for p in query.get("path", [])}})
        if op == "branches":
            return self._answer(200, {"branches": [
                {"name": name, "commit": oid} for name, oid in REFS.items()]})
        if op == "compare":
            return self._answer(200, {"ok": True, "patch": "diff --git a/y b/y\n"})
        return self._answer(404, {"ok": False, "error": "not_found"})

    @staticmethod
    def _answer(status, data):
        body = json.dumps(data).encode()
        return status, body, data


@pytest.fixture(autouse=True)
def fresh_bundles():
    repo_views._bundles.clear()
    repo_views._latest.clear()
    repo_views._building.clear()
    repo_views._public.clear()
    yield


def build(view, number=0, fast=None):
    fast = fast or FakeFastPath()
    return asyncio.run(repo_views.build(fast, "forkmesh", "forkmesh", view,
                                        number)), fast


def reads(bundle):
    return json.loads(bundle.body)["reads"]


def test_read_key_is_the_dashboards():
    assert repo_views.read_key("blobs", [
        ("path", "b"), ("ref", "main"), ("path", "a"), ("fmv", "9")]) == \
        "blobs?path=a&path=b&ref=main"
    assert repo_views.read_key("branches", []) == "branches?"


@pytest.mark.skipif(shutil.which("node") is None, reason="node not installed")
def test_dashboard_builds_the_same_keys():
    source = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()
    start = source.index("  function repoReadSeedKey(path) {")
    end = source.index("\n  }\n", start) + 4
    script = (
        "const location = { origin: 'https://app.test' };\n" + source[start:end]
        + "\nconsole.log(JSON.stringify(["
        "repoReadSeedKey('/api/repo/ForkMesh/forkmesh/blobs?path=b&path=a&ref=main&fmv=1'),"
        "repoReadSeedKey('/api/repo/forkmesh/forkmesh/tree?path=.forkmesh%2Fpulls&ref=forkmesh%2Fpulls'),"
        "repoReadSeedKey('/api/repo/forkmesh/forkmesh/branches'),"
        "repoReadSeedKey('/api/repo/forkmesh/forkmesh/tree?path=&viewer=x'),"
        "repoReadSeedKey('https://other.test/api/repo/forkmesh/forkmesh/tree?path=')]));")
    out = json.loads(subprocess.run(["node", "-e", script], check=True,
                                    capture_output=True, text=True).stdout)
    base = "/api/repo/forkmesh/forkmesh|"
    assert out == [
        base + repo_views.read_key("blobs", [("path", "b"), ("path", "a"),
                                             ("ref", "main")]),
        base + repo_views.read_key("tree", [("path", ".forkmesh/pulls"),
                                            ("ref", "forkmesh/pulls")]),
        base + "branches?", None, None]


def test_pull_list_reads_the_ledger_and_branch_only_pulls():
    bundle, _fast = build("pulls")
    keys = set(reads(bundle))
    assert "tree?path=.forkmesh/pulls&ref=forkmesh/pulls" in keys
    assert "tree?path=.forkmesh/pulls&ref=%s" % LEDGER in keys
    assert ("blobs?path=.forkmesh/pulls/3/pull.md&path=.forkmesh/pulls/4/"
            "pull.md&ref=%s" % LEDGER) in keys
    # Pull 7 lives only on forkmesh/pull/7: read at that branch's commit.
    assert "blob?path=.forkmesh/pulls/7/pull.md&ref=%s" % PULL7 in keys
    assert "tree?path=&ref=main" in keys
    rows = bundle.model["rows"]
    assert [r["number"] for r in rows] == [7, 4, 3]
    assert rows[0]["title"] == "Branch only"
    html = repo_views.render(bundle)
    assert "2 Open" in html and "1 Closed" in html
    assert 'data-ssr-record="pulls"' in html and "Merged thing" not in html
    assert 'opened by <a href="/@alice">alice</a>' in html


def test_pull_list_leaves_out_a_pull_its_author_deleted(monkeypatch):
    ledger = dict(FILES[LEDGER])
    ledger[".forkmesh/pulls/5/pull.md"] = pull_md(5, "Withdrawn", "deleted")
    monkeypatch.setitem(FILES, LEDGER, ledger)
    bundle, _fast = build("pulls")
    assert [r["number"] for r in bundle.model["rows"]] == [7, 4, 3]
    assert "Withdrawn" not in repo_views.render(bundle)


def test_pull_detail_reads_patch_and_conversation():
    bundle, _fast = build("pull", 4)
    keys = set(reads(bundle))
    for name in ("pull.md", "changes.patch", "commits.mbox"):
        assert "blob?path=.forkmesh/pulls/4/%s&ref=%s" % (name, LEDGER) in keys
    assert "tree?path=.forkmesh/pulls/4&ref=%s" % LEDGER in keys
    assert "blobs?path=.forkmesh/pulls/4/0001-comment.md&ref=%s" % LEDGER \
        in keys
    # The branch listing is left to the dashboard (costly on the gateway).
    assert "branches?" not in keys
    html = repo_views.render(bundle, "files")
    assert "x.py" in html and 'class="a">+new' in html
    page = repo_views.render(bundle)
    assert "Body of pull 4." in page and "frank" in page
    assert "<b>good</b>" not in page  # markup in a comment stays text
    assert '<b><a href="/@alice">alice</a></b> wants to merge' in page
    assert '<b><a href="/@frank">frank</a></b> commented' in page
    assert '<h1 class="fm-ssr-title"><a href="/@forkmesh">forkmesh</a> / ' in page


def test_branch_only_pull_detail_seeds_the_ledger_miss():
    bundle, _fast = build("pull", 7)
    data = reads(bundle)
    miss = data["blob?path=.forkmesh/pulls/7/pull.md&ref=%s" % LEDGER]
    assert miss == {"__seedError": {"status": 404,
                                    "error": "mirror_unavailable"}}
    assert "tree?path=.forkmesh/pulls/7&ref=forkmesh/pull/7" in data
    assert "blob?path=.forkmesh/pulls/7/pull.md&ref=%s" % PULL7 in data
    # No changes.patch on the branch: the review diff is the compare.
    assert "compare?base=%s&head=%s" % (BASE_OID, HEAD_OID) in data


def test_issue_list_and_detail():
    bundle, _fast = build("issues")
    keys = set(reads(bundle))
    assert "tree?path=.forkmesh/issues&ref=main" in keys
    assert "tree?path=.forkmesh/issues/open&ref=%s" % MAIN in keys
    assert "tree?path=.forkmesh/issues/closed&ref=%s" % MAIN in keys
    assert "blobs?path=.forkmesh/issues/open/12/issue-12.json&ref=%s" % MAIN \
        in keys
    assert bundle.model["open"] == 1 and bundle.model["closed"] == 1
    assert "Open one" in repo_views.render(bundle)

    closed, _fast = build("issue", 5)
    data = reads(closed)
    batch = data["blobs?path=.forkmesh/issues/5/issue-5.json"
                 "&path=.forkmesh/issues/closed/5/issue-5.json"
                 "&path=.forkmesh/issues/open/5/issue-5.json&ref=main"]
    assert batch["blobs"][".forkmesh/issues/closed/5/issue-5.json"]
    assert not any(key.startswith("blob?") for key in data)
    html = repo_views.render(closed)
    assert "Issue 5 body" in html and "A comment" in html
    assert '<b><a href="/@bob">bob</a></b> opened this issue' in html
    assert '<b><a href="/@carol">carol</a></b> commented' in html


SEED = repo_views.SEED_ERROR


def test_discussions():
    bundle, _fast = build("discussions")
    assert "blobs?path=.forkmesh/discussions/1/discussion.md&ref=main" \
        in reads(bundle)
    detail, _fast = build("discussion", 1)
    html = repo_views.render(detail)
    assert "Hello" in html and "Reply" in html
    assert '<b><a href="/@dan">dan</a></b> started this discussion' in html
    assert '<b><a href="/@erin">erin</a></b> commented' in html


def test_person_links_accounts_and_leaves_bots_and_git_names_plain():
    assert repo_views._person("@Alice") == '<a href="/@alice">@Alice</a>'
    assert repo_views._person("tasks[bot]") == "tasks[bot]"
    assert repo_views._person("dependabot") == "dependabot"
    assert repo_views._person("Jane Doe") == "Jane Doe"
    assert repo_views._person("<x>") == "&lt;x&gt;"
    assert repo_views._person(None) == ""


def test_unverified_author_shows_the_claimed_name_marked():
    author = repo_views._record_author
    assert author({"authorName": "bob", "authorClaimedName": "eve"}) == "bob"
    assert author({"author": "KEY", "authorClaimedName": "Eve"}) == "eve (unverified)"
    assert author({"author": "KEY", "authorClaimedName": "not a name"}) == "KEY"
    assert author({}) == "unknown"
    assert repo_views._person("eve (unverified)") == "eve (unverified)"


def test_commit_pinned_reads_are_marked_immutable():
    _bundle, fast = build("pull", 4)
    flags = {url: immutable for url, immutable in fast.urls}
    assert any(immutable for url, immutable in flags.items()
               if "ref=" + LEDGER in url)
    assert not any(immutable for url, immutable in flags.items()
                   if "ref=forkmesh%2Fpulls" in url or "ref=main" in url)


def test_private_or_unknown_repository_has_no_bundle():
    bundle, _fast = build("pulls", fast=FakeFastPath(public=False))
    assert bundle is None


def call(fast, query, owner="forkmesh", headers=()):
    sent = []

    async def send(message):
        sent.append(message)
    scope = {"type": "http", "method": "GET", "query_string": query.encode(),
             "headers": list(headers)}
    handled = asyncio.run(repo_views.handle(fast, scope, send, owner,
                                            "forkmesh"))
    if not handled:
        return None
    head = dict((k.decode(), v.decode()) for k, v in sent[0]["headers"])
    return sent[0]["status"], head, b"".join(m.get("body", b"")
                                             for m in sent[1:])


def test_page_reads_endpoint():
    fast = FakeFastPath()
    status, headers, body = call(fast, "view=pulls")
    assert status == 200 and headers["cache-control"] == "no-cache"
    data = json.loads(body)
    assert data["base"] == "/api/repo/forkmesh/forkmesh"
    version = headers["etag"].split("-")[-1].strip('"')
    status, headers, again = call(fast, "view=pulls&v=" + version)
    assert headers["cache-control"] == IMMUTABLE and again == body
    status, _headers, _body = call(fast, "view=pulls", headers=[
        (b"if-none-match", ('"fm-view-%s"' % version).encode())])
    assert status == 304
    for bad in ("view=nope", "view=pull", "view=pull&number=x",
                "view=pulls&sig=1"):
        assert call(fast, bad) is None
    # The repository went private: its remembered bundle is not served
    # once the public check (kept a few seconds) is renewed.
    repo_views._public.clear()
    assert call(FakeFastPath(public=False), "view=pulls") is None
    assert call(FakeFastPath(public=False), "view=pulls&v=" + version) is None


def test_ssr_injects_the_view_and_its_bundle_address():
    page = ('<html><head><meta charset="UTF-8" /></head><body data-page="repo">'
            + repo_ssr.PLACEHOLDER + "</body></html>")
    bundle, _fast = build("pull", 4)
    html = repo_ssr.inject_view(page, bundle, "")
    assert "window.__forkmeshView=" in html and "data-repo-ssr" in html
    meta = json.loads(html.split("window.__forkmeshView=", 1)[1].split(
        ";window.", 1)[0])
    assert meta["url"] == bundle.url() and meta["view"] == "pull"
    assert repo_ssr.inject_view("<html></html>", bundle) is None


@pytest.mark.parametrize("path,expected", [
    ("/forkmesh/forkmesh/pulls", ("forkmesh", "forkmesh", "pulls", 0, "")),
    ("/ForkMesh/forkmesh/pulls/12/files",
     ("forkmesh", "forkmesh", "pull", 12, "files")),
    ("/forkmesh/forkmesh/issues/5", ("forkmesh", "forkmesh", "issue", 5, "")),
    ("/forkmesh/forkmesh/discussions", ("forkmesh", "forkmesh",
                                        "discussions", 0, "")),
    ("/forkmesh/forkmesh/tree/main", None),
    ("/api/forkmesh/pulls", None),
    ("/forkmesh/forkmesh/pulls/12/nope", None),
])
def test_view_target(path, expected):
    scope = {"method": "GET", "path": path, "raw_path": path.encode(),
             "query_string": b""}
    assert repo_views.view_target(scope) == expected


def test_compare_between_commits_is_immutable():
    assert cache_control("compare", [("base", BASE_OID),
                                     ("head", HEAD_OID)]) == IMMUTABLE
    assert cache_control("compare", [("base", "main"),
                                     ("head", HEAD_OID)]) != IMMUTABLE


def test_bundle_is_the_gateway_bytes_and_parses():
    bundle, _fast = build("pulls")
    data = json.loads(bundle.body)
    assert data["view"] == "pulls" and data["servedBy"] == "fmnext-n9"
    assert list(data["reads"]) == sorted(data["reads"])


def test_stale_bundle_is_served_while_one_refresh_runs(monkeypatch):
    async def main():
        fast = FakeFastPath()
        first = await repo_views.build(fast, "forkmesh", "forkmesh", "pulls")
        reads = len(fast.urls)
        # Fresh: no gateway read at all.
        assert repo_views.cached(fast, "forkmesh", "forkmesh", "pulls") is first
        assert len(fast.urls) == reads
        first.built -= repo_views.BUNDLE_FRESH_SECONDS + 1
        again = repo_views.cached(fast, "forkmesh", "forkmesh", "pulls")
        assert again is first  # served stale ...
        assert ("forkmesh", "forkmesh", "pulls", 0) in repo_views._building
        await asyncio.sleep(0.05)  # ... while the refresh runs
        assert len(fast.urls) > reads
        # Unchanged content: the same bundle, now fresh again.
        assert repo_views.cached(fast, "forkmesh", "forkmesh", "pulls") is first
        first.built -= repo_views.BUNDLE_STALE_SECONDS
        assert repo_views.cached(fast, "forkmesh", "forkmesh", "pulls") is None
    asyncio.run(main())
