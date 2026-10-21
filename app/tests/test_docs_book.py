import asyncio
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

import docs_book  # noqa: E402


GIT_ENV = {
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_AUTHOR_NAME": "seed",
    "GIT_AUTHOR_EMAIL": "seed@example.test",
    "GIT_COMMITTER_NAME": "seed",
    "GIT_COMMITTER_EMAIL": "seed@example.test",
    "PATH": os.environ.get("PATH", ""),
}


def git(cwd, *args, data=None):
    return subprocess.run(
        ["git", *args], cwd=cwd, env=GIT_ENV, input=data,
        capture_output=True, check=True).stdout


@pytest.fixture()
def repo(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    git(work, "init", "-q", "-b", "main")
    (work / "README.md").write_text("# Top\n")
    (work / "a.txt").write_text("a\n")
    (work / "docs" / "app").mkdir(parents=True)
    (work / "docs" / "README.md").write_text("# Docs\n\nIndex.\n")
    (work / "docs" / "PLAN.md").write_text("# Plan\n\nOne.\n\nTwo.\n")
    (work / "docs" / "app" / "notes.md").write_text("# Notes\n")
    (work / "docs" / "app" / "data.json").write_text("{}\n")
    (work / "docs" / "zeta").mkdir()
    (work / "docs" / "zeta" / "z.md").write_text("# Z\n")
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", "seed")
    bare = tmp_path / "repo.git"
    git(tmp_path, "clone", "-q", "--bare", str(work), str(bare))
    return bare


def ls_tree(bare, ref, path):
    spec = ref + (":" + path if path else "")
    out = git(bare, "ls-tree", "-l", "-z", spec)
    entries = []
    for record in out.split(b"\0"):
        if not record:
            continue
        meta, name = record.split(b"\t", 1)
        mode, kind, oid, size = meta.split()
        name = name.decode()
        full = (path + "/" if path else "") + name
        log = git(bare, "log", "-1", "--format=%H%x1f%an%x1f%cI%x1f%s",
                  ref, "--", full).decode().strip().split("\x1f")
        entries.append({
            "name": name, "type": kind.decode(),
            "size": 0 if size == b"-" else int(size),
            "mode": mode.decode(), "oid": oid.decode(),
            "hash": log[0], "author": log[1], "date": log[2],
            "subject": log[3],
        })
    return entries


class FakeResponse:
    def __init__(self, status, data=None, text=""):
        self.status = status
        self._data = data
        self._text = text

    async def json(self):
        return self._data

    async def text(self):
        return self._text


class FakeRequest:
    def __init__(self, method="GET", body=None):
        self.method = method
        self.body = body


def harness(bare, admin=True, gateway_oids=True):
    calls = {"pushes": [], "audits": [], "reads": [], "down": set()}

    async def js_fetch_with_timeout(url, init, timeout):
        parsed = urlparse(url)
        action = parsed.path.rsplit("/", 1)[1]
        query = parse_qs(parsed.query)
        calls["reads"].append((action, query.get("path", [""])))
        if action in calls["down"]:
            return FakeResponse(503)
        ref = query.get("ref", ["main"])[0]
        commit = git(bare, "rev-parse", ref + "^{commit}").decode().strip()
        if action == "tree":
            path = query.get("path", [""])[0]
            entries = ls_tree(bare, commit, path)
            if not gateway_oids:
                for entry in entries:
                    entry.pop("oid")
                    entry.pop("mode")
            return FakeResponse(200, {
                "ok": True, "commit": commit, "entries": entries,
                "latestCommit": {"hash": commit, "date": "", "subject": "x"}})
        if action == "blobs":
            blobs = {}
            for path in query.get("path", []):
                content = git(bare, "show", commit + ":" + path).decode()
                blobs[path] = {"ok": True, "encoding": "utf8",
                               "content": content, "truncated": False}
            return FakeResponse(200, {"ok": True, "commit": commit,
                                      "blobs": blobs})
        return FakeResponse(404)

    async def push_send(env, request, owner, repo, account, method, body,
                        stream, digest, **kwargs):
        calls["pushes"].append((owner, repo, account, kwargs))
        result = subprocess.run(
            ["git", "receive-pack", "--stateless-rpc", str(bare)],
            input=body, env=GIT_ENV, capture_output=True, check=True)
        return FakeResponse(200, text=result.stdout.decode("latin-1")), {
            "node": "n1"}

    async def bounded_json_request(request, max_bytes=None):
        return request.body

    async def authed(env, request, data=None):
        return "jett"

    async def is_admin(env, name):
        return admin

    async def audit(env, actor, action, *args):
        calls["audits"].append((actor, action) + args)

    async def org_node(env, org, name):
        return "fmnext-n1"

    runtime = {
        "json_response": lambda data, status=200, cache_control=None: {
            "status": status, "data": data},
        "method_name": lambda request: request.method,
        "bounded_json_request": bounded_json_request,
        "_authed_account_name": authed,
        "_is_admin": is_admin,
        "_audit_sensitive_action": audit,
        "_org_repo_node": org_node,
        "_core_push_nodes": lambda env: {"fmnext-n1"},
        "_https_mirror_push_send": push_send,
        "js_fetch_with_timeout": js_fetch_with_timeout,
        "_public_base_url": lambda env: "https://app.example.test",
    }
    for name, value in runtime.items():
        setattr(docs_book, name, value)
    docs_book._CACHE.update({"commit": "", "at": 0, "docs": None,
                             "complete": False, "files": [], "trees": {}})
    return calls


def run(path, request):
    return asyncio.run(docs_book.route(object(), request, path))


def test_lists_every_markdown_file_in_order_with_dates(repo):
    harness(repo)
    result = run("/api/docs-book", FakeRequest())
    assert result["status"] == 200
    data = result["data"]
    paths = [doc["path"] for doc in data["docs"]]
    assert paths == ["docs/README.md", "docs/PLAN.md", "docs/app/notes.md",
                     "docs/zeta/z.md"]
    first = data["docs"][0]
    assert first["content"] == "# Docs\n\nIndex.\n"
    assert first["updated"] and first["subject"] == "seed"
    assert len(first["oid"]) == 40
    assert data["canEdit"] is True


def commit_file(bare, tmp_path, path, text):
    work = tmp_path / ("edit-%d" % len(list(tmp_path.iterdir())))
    git(tmp_path, "clone", "-q", str(bare), str(work))
    target = work / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", "edit " + path)
    git(work, "push", "-q", "origin", "main")


def cold_listing(bare):
    harness(bare)
    return run("/api/docs-book", FakeRequest())["data"]


def test_listing_is_kept_until_main_moves(repo):
    calls = harness(repo)
    first = run("/api/docs-book", FakeRequest())["data"]
    calls["reads"].clear()
    docs_book._CACHE["at"] -= docs_book.CACHE_TTL_MS * 10
    again = run("/api/docs-book", FakeRequest())["data"]
    assert calls["reads"] == [("tree", ["docs"])]
    assert again == first


def test_main_moving_outside_docs_reads_nothing_new(repo, tmp_path):
    calls = harness(repo)
    run("/api/docs-book", FakeRequest())
    commit_file(repo, tmp_path, "a.txt", "b\n")
    calls["reads"].clear()
    listed = run("/api/docs-book", FakeRequest())["data"]
    assert calls["reads"] == [("tree", ["docs"])]
    assert listed == cold_listing(repo)


def test_an_edit_rereads_only_its_directory_and_blob(repo, tmp_path):
    calls = harness(repo)
    run("/api/docs-book", FakeRequest())
    commit_file(repo, tmp_path, "docs/app/notes.md", "# Notes\n\nNew.\n")
    calls["reads"].clear()
    listed = run("/api/docs-book", FakeRequest())["data"]
    assert calls["reads"] == [("tree", ["docs"]), ("tree", ["docs/app"]),
                              ("blobs", ["docs/app/notes.md"])]
    assert listed == cold_listing(repo)
    notes = next(doc for doc in listed["docs"]
                 if doc["path"] == "docs/app/notes.md")
    assert notes["content"] == "# Notes\n\nNew.\n"
    assert notes["subject"] == "edit docs/app/notes.md"


def test_failed_reads_are_retried_after_the_window(repo):
    calls = harness(repo)
    calls["down"].update({"blobs", "blob"})
    broken = run("/api/docs-book", FakeRequest())["data"]
    assert all(doc["content"] is None for doc in broken["docs"])
    calls["down"].clear()
    assert run("/api/docs-book", FakeRequest())["data"] == broken
    docs_book._CACHE["at"] -= docs_book.CACHE_TTL_MS + 1
    healed = run("/api/docs-book", FakeRequest())["data"]
    assert healed == cold_listing(repo)
    assert healed["docs"][0]["content"] == "# Docs\n\nIndex.\n"


def test_non_admin_cannot_save(repo):
    harness(repo, admin=False)
    listed = run("/api/docs-book", FakeRequest())["data"]
    assert listed["canEdit"] is False
    result = run("/api/docs-book/save", FakeRequest("POST", {
        "path": "docs/PLAN.md", "content": "x\n"}))
    assert result["status"] == 403


def test_admin_save_commits_on_main_and_git_accepts_it(repo):
    calls = harness(repo)
    listed = run("/api/docs-book", FakeRequest())["data"]
    plan = next(doc for doc in listed["docs"] if doc["path"] == "docs/app/notes.md")
    before = git(repo, "rev-parse", "main").decode().strip()
    result = run("/api/docs-book/save", FakeRequest("POST", {
        "path": "docs/app/notes.md", "oid": plan["oid"],
        "content": "# Notes\n\nEdited on the web.\n",
        "message": "Docs: say hello"}))
    assert result["status"] == 200, result
    after = git(repo, "rev-parse", "main").decode().strip()
    assert result["data"]["commit"] == after != before
    assert git(repo, "rev-parse", "main^").decode().strip() == before
    assert git(repo, "show", "main:docs/app/notes.md").decode() == (
        "# Notes\n\nEdited on the web.\n")
    assert git(repo, "show", "main:docs/PLAN.md").decode().startswith("# Plan")
    assert git(repo, "show", "main:a.txt").decode() == "a\n"
    assert git(repo, "log", "-1", "--format=%an <%ae>|%s", "main").decode(
        ).strip() == "jett <jett@users.noreply.example.test>|Docs: say hello"
    git(repo, "fsck", "--strict", "--no-dangling")
    owner, name, account, options = calls["pushes"][0]
    assert (owner, name, account) == ("fmnext-n1", "forkmesh", "jett")
    assert options["alias_owner"] == "forkmesh"
    assert options["core_held"] is True
    assert calls["audits"][0][1] == "docs.edit"


def test_stale_oid_is_a_conflict_and_changes_nothing(repo):
    harness(repo)
    before = git(repo, "rev-parse", "main").decode().strip()
    result = run("/api/docs-book/save", FakeRequest("POST", {
        "path": "docs/PLAN.md", "oid": "0" * 40, "content": "x\n"}))
    assert result["status"] == 409
    assert git(repo, "rev-parse", "main").decode().strip() == before


def test_unchanged_content_makes_no_commit(repo):
    calls = harness(repo)
    result = run("/api/docs-book/save", FakeRequest("POST", {
        "path": "docs/PLAN.md", "content": "# Plan\n\nOne.\n\nTwo.\n"}))
    assert result["data"]["unchanged"] is True
    assert calls["pushes"] == []


def test_paths_outside_docs_markdown_are_refused(repo):
    harness(repo)
    for path in ("README.md", "docs/app/data.json", "docs/../a.md",
                 "docs/.hidden.md", "docs//x.md"):
        result = run("/api/docs-book/save", FakeRequest("POST", {
            "path": path, "content": "x\n"}))
        assert result["status"] == 400, path


def test_old_gateway_without_object_ids_fails_cleanly(repo):
    harness(repo, gateway_oids=False)
    result = run("/api/docs-book/save", FakeRequest("POST", {
        "path": "docs/PLAN.md", "content": "x\n"}))
    assert result["status"] == 503
    assert result["data"]["error"] == "gateway_outdated"


def test_report_status_parsing():
    ok = docs_book.pkt_line("unpack ok\n") + docs_book.pkt_line(
        "ok refs/heads/main\n") + b"0000"
    assert docs_book.report_status(ok, "refs/heads/main") == (True, "")
    ng = docs_book.pkt_line("unpack ok\n") + docs_book.pkt_line(
        "ng refs/heads/main fetch first\n") + b"0000"
    assert docs_book.report_status(ng, "refs/heads/main") == (
        False, "fetch first")
    assert docs_book.report_status(b"", "refs/heads/main")[0] is False
    assert json.dumps(docs_book.order_key("docs/a/README.md"))
