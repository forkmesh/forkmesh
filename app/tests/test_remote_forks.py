import ast
import asyncio
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import remote_forks  # noqa: E402
import schema  # noqa: E402
import urls  # noqa: E402

ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
DASHBOARD = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")


def test_parse_url_accepts_repository_addresses_on_other_hosts():
    parsed = remote_forks.parse_url("https://forkmesh.com/acme/widget")
    assert parsed == {
        "url": "https://forkmesh.com/acme/widget",
        "origin": "https://forkmesh.com",
        "host": "forkmesh.com",
        "owner": "acme",
        "name": "widget",
    }
    assert remote_forks.parse_url("Git.Example.org/acme/widget.git")["url"] == \
        "https://git.example.org/acme/widget"
    assert remote_forks.parse_url(
        "https://git.example.org/acme/widget/pulls/3")["name"] == "widget"
    assert remote_forks.parse_url(
        "https://git.example.org:8443/a/b")["origin"] == "https://git.example.org:8443"
    assert remote_forks.parse_url(
        "https://git.example.org:443/a/b")["origin"] == "https://git.example.org"


@pytest.mark.parametrize("value", [
    "", "http://git.example.org/a/b", "https://user:pw@git.example.org/a/b",
    "https://10.0.0.1/a/b", "https://localhost/a/b", "https://[::1]/a/b",
    "https://git.example.org/a", "https://git.example.org/../b",
    "https://git.example.org/a/.git", "javascript:alert(1)//x.com/a/b",
    "https://git.example.org/a b/c", "ftp://git.example.org/a/b",
    "https://git.example.org/" + "a" * 300 + "/b",
])
def test_parse_url_refuses_anything_but_a_public_https_repository(value):
    assert remote_forks.parse_url(value) is None


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        statement = next(item for item in schema.SCHEMA_STATEMENTS
                         if "repo_remote_forks" in item)
        self.db.execute(statement)

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)


@pytest.fixture
def runtime(monkeypatch):
    d1 = FakeD1()
    private = set()

    async def blind_index(env, text):
        return hashlib.sha256(text.encode()).hexdigest()

    async def body(request):
        if request.body is None:
            raise ValueError("no body")
        return request.body

    async def actor(env, request, data):
        return request.actor

    async def owns(env, actor_name, owner):
        return actor_name == owner

    async def org_owner(env, owner, repo, actor_name):
        return actor_name == "orgadmin"

    async def is_private(env, owner, repo):
        return (owner, repo) in private

    async def ensure_schema(env):
        return None

    def json_response(data, status=200, cache_control=None, extra_headers=None):
        return SimpleNamespace(status=status, data=data)

    for name, value in {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "bounded_json_request": body,
        "_authed_account_name": actor, "_account_owns_node": owns,
        "_org_owner_of_linked_repo": org_owner, "_repo_is_private": is_private,
        "ensure_schema": ensure_schema, "json_response": json_response,
        "method_name": lambda request: request.method,
    }.items():
        monkeypatch.setattr(remote_forks, name, value, raising=False)
    return SimpleNamespace(d1=d1, private=private)


def call(method, actor="", body=None, url="https://code.example.com/api/repo/ada/widget/remote-forks"):
    request = SimpleNamespace(method=method, actor=actor, body=body, url=url)
    env = SimpleNamespace(APP_ORIGIN="https://code.example.com")
    return asyncio.run(remote_forks.handler(env, request, "ada", "widget", "ada"))


def test_owner_saves_lists_and_removes_forks_on_other_instances(runtime):
    listed = call("GET")
    assert listed.status == 200
    assert listed.data["forks"] == [] and listed.data["canManage"] is False
    saved = call("POST", "ada", {"url": "forkmesh.com/forkmesh/widget.git", "label": " Upstream  repo "})
    assert saved.status == 201
    assert saved.data["fork"]["url"] == "https://forkmesh.com/forkmesh/widget"
    call("POST", "ada", {"url": "https://git.example.org/ada/widget"})
    again = call("POST", "ada", {"url": "https://forkmesh.com/forkmesh/widget", "label": "Upstream"})
    assert again.status == 201
    listed = call("GET", "ada")
    assert listed.data["canManage"] is True
    assert [fork["url"] for fork in listed.data["forks"]] == [
        "https://forkmesh.com/forkmesh/widget", "https://git.example.org/ada/widget"]
    assert listed.data["forks"][0]["label"] == "Upstream"
    assert listed.data["forks"][0]["addedBy"] == "ada"
    assert listed.data["forks"][1]["label"] == "git.example.org"
    removed = call("DELETE", "ada", {"url": "https://forkmesh.com/forkmesh/widget"})
    assert [fork["url"] for fork in removed.data["forks"]] == ["https://git.example.org/ada/widget"]
    removed = call("DELETE", "ada", None,
                   "https://code.example.com/api/repo/ada/widget/remote-forks?url=https%3A%2F%2Fgit.example.org%2Fada%2Fwidget")
    assert removed.data["forks"] == []


def test_only_the_owner_or_an_organization_owner_changes_the_list(runtime):
    assert call("POST", "", {"url": "https://forkmesh.com/a/b"}).status == 403
    assert call("POST", "mallory", {"url": "https://forkmesh.com/a/b"}).status == 403
    assert call("DELETE", "mallory", {"url": "https://forkmesh.com/a/b"}).status == 403
    assert call("POST", "orgadmin", {"url": "https://forkmesh.com/a/b"}).status == 201
    assert call("PUT", "ada", {}).status == 405


def test_bad_addresses_this_instance_and_too_many_forks_are_refused(runtime):
    assert call("POST", "ada", {"url": "http://forkmesh.com/a/b"}).data["error"] == "invalid_url"
    same = call("POST", "ada", {"url": "https://code.example.com/bob/widget"})
    assert same.status == 400 and same.data["error"] == "same_instance"
    for index in range(remote_forks.MAX_FORKS):
        assert call("POST", "ada", {"url": "https://h%d.example.org/a/b" % index}).status == 201
    full = call("POST", "ada", {"url": "https://more.example.org/a/b"})
    assert full.status == 409 and full.data["error"] == "too_many_forks"
    assert call("POST", "ada", {"url": "https://h0.example.org/a/b"}).status == 201


def test_a_private_repository_has_no_public_fork_list(runtime):
    runtime.private.add(("ada", "widget"))
    assert call("GET", "ada").status == 404


def test_route_schema_and_migration_are_wired():
    assert urls.REPO_REMOTE_FORKS_RE.match("/api/repo/ada/widget/remote-forks")
    assert not urls.REPO_REMOTE_FORKS_RE.match("/api/repo/ada/widget/remote-forks/x")
    assert '"REPO_FORKS_RE", "REPO_REMOTE_FORKS_RE",' in ENTRY
    assert "remote_forks_match = REPO_REMOTE_FORKS_RE.match(url.path)" in APP_RUNTIME
    assert "remote_forks._bind_runtime(globals())" in APP_RUNTIME
    migration = (ROOT / "migrations" / "0252_repo_remote_forks.sql").read_text()
    assert "CREATE TABLE IF NOT EXISTS repo_remote_forks" in migration
    down = (ROOT / "migrations" / "down" / "0252_repo_remote_forks.sql").read_text()
    assert "DROP TABLE IF EXISTS repo_remote_forks" in down


def _open_pull_cors_headers():
    tree = ast.parse(ENTRY)
    node = next(item for item in tree.body
                if isinstance(item, ast.FunctionDef)
                and item.name == "_open_pull_cors_headers")
    namespace = {
        "REPO_PULLS_RE": urls.REPO_PULLS_RE,
        "_request_origin": lambda request: request.headers.get("origin", ""),
    }
    exec(compile(ast.Module(body=[node], type_ignores=[]), "entry", "exec"), namespace)
    return namespace["_open_pull_cors_headers"]


def test_any_origin_may_post_a_signed_pull_without_credentials():
    headers_for = _open_pull_cors_headers()
    request = SimpleNamespace(headers={
        "origin": "https://git.example.org",
        "access-control-request-method": "POST"})
    path = "/api/repo/ada/widget/pulls"
    plain = headers_for(request, path)
    assert plain["access-control-allow-origin"] == "*"
    assert "access-control-allow-credentials" not in plain
    preflight = headers_for(request, path, preflight=True)
    assert preflight["access-control-allow-methods"] == "POST, OPTIONS"
    assert preflight["access-control-allow-headers"] == "accept, content-type"
    assert headers_for(request, "/api/repo/ada/widget/pulls/3/merge") == {}
    assert headers_for(request, "/api/repo/ada/widget/issues") == {}
    deleting = SimpleNamespace(headers={
        "origin": "https://git.example.org",
        "access-control-request-method": "DELETE"})
    assert headers_for(deleting, path, preflight=True) == {}
    assert headers_for(SimpleNamespace(headers={}), path) == {}
    assert "_open_pull_cors_headers(request, path, preflight=True)" in ENTRY
    assert re.search(r"_open_pull_cors_headers\(request, path\)\s+if method_name\(request\) == \"POST\"", ENTRY)


def test_dashboard_sends_a_pull_to_a_saved_fork_on_another_instance():
    assert "async function submitRemotePullOpen(repo, remote, title, body, base, head, compareBase = \"\", aiModel = \"\")" in DASHBOARD
    assert 'credentials: "omit"' in DASHBOARD.split("async function submitRemotePullOpen", 1)[1].split("async function signWebPull", 1)[0]
    assert "const signedHead = `${location.host}/${repoDisplayKey(repo)}:${head}`;" in DASHBOARD
    assert 'webEventSigningPrefix("pull-event", signingRepo)' in DASHBOARD
    assert "data-repo-pull-target" in DASHBOARD
    assert "data-repo-remote-fork-form" in DASHBOARD
    assert "`${repoApiBase(repo)}/remote-forks`" in DASHBOARD
    assert 'headOwner.includes("/") ? headOwner' in DASHBOARD
