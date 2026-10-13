import asyncio
import os
import subprocess
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import repository_file_edits  # noqa: E402


GIT_ENV = {
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_AUTHOR_NAME": "seed",
    "GIT_AUTHOR_EMAIL": "seed@example.test",
    "GIT_COMMITTER_NAME": "seed",
    "GIT_COMMITTER_EMAIL": "seed@example.test",
    "PATH": os.environ.get("PATH", ""),
}


def git(cwd, *args, data=None, check=True):
    return subprocess.run(
        ["git", *args], cwd=cwd, env=GIT_ENV, input=data,
        capture_output=True, check=check)


def git_out(cwd, *args):
    return git(cwd, *args).stdout.decode()


@pytest.fixture()
def bare_repo(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    git(work, "init", "-q", "-b", "main")
    (work / "README.md").write_text("# Top\n")
    (work / ".gitignore").write_text("build\n")
    (work / "docs").mkdir()
    (work / "docs" / "guide.md").write_text("# Guide\n")
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", "seed")
    bare = tmp_path / "repo.git"
    git(tmp_path, "clone", "-q", "--bare", str(work), str(bare))
    return bare


def tree_entries(bare, ref, path):
    spec = ref + (":" + path if path else "")
    result = git(bare, "ls-tree", "-l", "-z", spec, check=False)
    if result.returncode != 0:
        return None
    entries = []
    for record in result.stdout.split(b"\0"):
        if not record:
            continue
        meta, name = record.split(b"\t", 1)
        mode, kind, oid, size = meta.split()
        entries.append({
            "name": name.decode(),
            "type": kind.decode(),
            "mode": mode.decode(),
            "oid": oid.decode(),
            "size": 0 if size == b"-" else int(size),
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
    def __init__(self, method="POST", body=None):
        self.method = method
        self.body = body or {}


def harness(bare, owner_write=True, org_write=False, access=True):
    calls = {"pushes": [], "audits": []}

    async def js_fetch_with_timeout(url, init, timeout):
        parsed = urlparse(url)
        action = parsed.path.rsplit("/", 1)[1]
        query = parse_qs(parsed.query)
        ref = query.get("ref", ["main"])[0]
        commit = git(bare, "rev-parse", ref + "^{commit}", check=False)
        if commit.returncode != 0:
            return FakeResponse(404, {"ok": False})
        commit_id = commit.stdout.decode().strip()
        if action == "tree":
            path = query.get("path", [""])[0]
            entries = tree_entries(bare, commit_id, path)
            if entries is None:
                return FakeResponse(404, {"ok": False})
            return FakeResponse(200, {
                "ok": True, "commit": commit_id, "entries": entries})
        if action == "blob":
            path = query["path"][0]
            content = git(bare, "show", commit_id + ":" + path,
                          check=False)
            if content.returncode != 0:
                return FakeResponse(404, {"ok": False})
            return FakeResponse(200, {
                "ok": True, "content": content.stdout.decode()})
        return FakeResponse(404, {"ok": False})

    async def push_send(env, request, owner, repo, account, method, body,
                        stream, digest, **kwargs):
        calls["pushes"].append((owner, repo, account, kwargs))
        result = subprocess.run(
            ["git", "receive-pack", "--stateless-rpc", str(bare)],
            input=body, env=GIT_ENV, capture_output=True, check=True)
        return FakeResponse(200, text=result.stdout.decode("latin-1")), {
            "node": "host"}

    async def bounded_json_request(request, max_bytes=None):
        return request.body

    async def authed(env, request, data=None):
        return "jett"

    async def repository_access(env, request, owner, repo, viewer=None):
        return {"owner": owner, "repo": repo} if access else None

    async def account_owns_node(env, account, owner):
        return owner_write

    async def org_write_allowed(env, owner, repo, actor):
        return org_write

    async def audit(env, actor, action, *args):
        calls["audits"].append((actor, action) + args)

    runtime = {
        "json_response": lambda data, status=200, cache_control=None: {
            "status": status, "data": data},
        "method_name": lambda request: request.method,
        "bounded_json_request": bounded_json_request,
        "_authed_account_name": authed,
        "_repository_access_context": repository_access,
        "_account_owns_node": account_owns_node,
        "_org_write_allowed": org_write_allowed,
        "_audit_sensitive_action": audit,
        "_core_push_nodes": lambda env: {"alice"},
        "_https_mirror_push_send": push_send,
        "js_fetch_with_timeout": js_fetch_with_timeout,
        "_public_base_url": lambda env: "https://app.example.test",
    }
    for name, value in runtime.items():
        setattr(repository_file_edits, name, value)
    return calls


def run(owner, repo, request):
    return asyncio.run(
        repository_file_edits.route(object(), request, owner, repo,
                                    "file-edit"))


def blob_oid(bare, ref, path):
    return git_out(bare, "rev-parse", ref + ":" + path).strip()


def test_owner_save_commits_text_file_on_branch(bare_repo):
    calls = harness(bare_repo)
    before = git_out(bare_repo, "rev-parse", "main").strip()
    result = run("alice", "project", FakeRequest(body={
        "path": "README.md",
        "branch": "main",
        "oid": blob_oid(bare_repo, "main", "README.md"),
        "content": "# Top\n\nEdited on the web.\n",
        "message": "Edit README in browser",
    }))
    assert result["status"] == 200, result
    after = git_out(bare_repo, "rev-parse", "main").strip()
    assert result["data"]["commit"] == after != before
    assert git_out(bare_repo, "rev-parse", "main^").strip() == before
    assert git_out(bare_repo, "show", "main:README.md") == (
        "# Top\n\nEdited on the web.\n")
    assert git_out(bare_repo, "show", "main:docs/guide.md") == "# Guide\n"
    assert git_out(
        bare_repo, "log", "-1", "--format=%an <%ae>|%s", "main").strip() == (
            "jett <jett@users.noreply.example.test>|Edit README in browser")
    git(bare_repo, "fsck", "--strict", "--no-dangling")
    owner, repo, account, options = calls["pushes"][0]
    assert (owner, repo, account) == ("alice", "project", "jett")
    assert options["credential"] == "web-file-edit"
    assert options["core_held"] is True
    assert calls["audits"][0][1] == "repository.file_edit"


def test_save_can_create_branch_from_current_branch(bare_repo):
    harness(bare_repo)
    main_before = git_out(bare_repo, "rev-parse", "main").strip()
    result = run("alice", "project", FakeRequest(body={
        "path": "README.md",
        "branch": "web/readme",
        "baseRef": "main",
        "oid": blob_oid(bare_repo, "main", "README.md"),
        "content": "# Top\n\nBranch edit.\n",
    }))
    assert result["status"] == 200, result
    assert result["data"]["createdBranch"] is True
    assert git_out(bare_repo, "rev-parse", "main").strip() == main_before
    assert git_out(bare_repo, "show", "web/readme:README.md") == (
        "# Top\n\nBranch edit.\n")
    assert git_out(bare_repo, "rev-parse", "web/readme^").strip() == main_before


def test_org_writer_can_save_when_owner_check_fails(bare_repo):
    harness(bare_repo, owner_write=False, org_write=True)
    result = run("acme", "project", FakeRequest(body={
        "path": ".gitignore",
        "branch": "main",
        "oid": blob_oid(bare_repo, "main", ".gitignore"),
        "content": "build\nnode_modules\n",
    }))
    assert result["status"] == 200, result
    assert git_out(bare_repo, "show", "main:.gitignore") == (
        "build\nnode_modules\n")


def test_writer_without_access_cannot_discover_private_repo(bare_repo):
    harness(bare_repo, access=False)
    result = run("alice", "project", FakeRequest(body={
        "path": "README.md",
        "branch": "main",
        "content": "x\n",
    }))
    assert result["status"] == 404
    assert result["data"]["error"] == "not_found"


def test_private_org_writer_can_save_after_read_gate_fails(bare_repo):
    harness(bare_repo, owner_write=False, org_write=True, access=False)
    result = run("node-owner", "project", FakeRequest(body={
        "path": "README.md",
        "branch": "main",
        "oid": blob_oid(bare_repo, "main", "README.md"),
        "content": "# Top\n\nPrivate org edit.\n",
    }))
    assert result["status"] == 200, result
    assert git_out(bare_repo, "show", "main:README.md") == (
        "# Top\n\nPrivate org edit.\n")


def test_read_only_viewer_cannot_save(bare_repo):
    harness(bare_repo, owner_write=False, org_write=False)
    result = run("alice", "project", FakeRequest(body={
        "path": "README.md",
        "branch": "main",
        "content": "x\n",
    }))
    assert result["status"] == 403
    assert result["data"]["error"] == "forbidden"


def test_stale_blob_oid_is_conflict(bare_repo):
    harness(bare_repo)
    before = git_out(bare_repo, "rev-parse", "main").strip()
    result = run("alice", "project", FakeRequest(body={
        "path": "README.md",
        "branch": "main",
        "oid": "1" * 40,
        "content": "# Top\n\nStale.\n",
    }))
    assert result["status"] == 409
    assert result["data"]["error"] == "conflict"
    assert git_out(bare_repo, "rev-parse", "main").strip() == before


def test_save_can_create_a_file_and_its_folders(bare_repo):
    harness(bare_repo)
    content = '{"schema": "forkmesh-pull-rules-v1", "rules": []}\n'
    result = run("alice", "project", FakeRequest(body={
        "path": ".forkmesh/pull-rules.json",
        "branch": "main",
        "baseRef": "main",
        "create": True,
        "content": content,
        "message": "Add pull request rules",
    }))
    assert result["status"] == 200, result
    assert git_out(bare_repo, "show", "main:.forkmesh/pull-rules.json") == content
    assert git_out(bare_repo, "show", "main:docs/guide.md") == "# Guide\n"
    git(bare_repo, "fsck", "--strict", "--no-dangling")

    nested = run("alice", "project", FakeRequest(body={
        "path": "docs/a-new.md", "branch": "main", "create": True,
        "content": "# New\n",
    }))
    assert nested["status"] == 200, nested
    assert git_out(bare_repo, "show", "main:docs/a-new.md") == "# New\n"
    git(bare_repo, "fsck", "--strict", "--no-dangling")


def test_create_refuses_an_existing_file_and_edit_refuses_a_missing_one(bare_repo):
    harness(bare_repo)
    exists = run("alice", "project", FakeRequest(body={
        "path": "README.md", "branch": "main", "create": True,
        "content": "replaced\n",
    }))
    assert exists["status"] == 409, exists
    missing = run("alice", "project", FakeRequest(body={
        "path": "missing.md", "branch": "main", "content": "x\n",
    }))
    assert missing["status"] == 404, missing
