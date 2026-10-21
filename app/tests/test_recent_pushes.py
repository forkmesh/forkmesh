import asyncio
import hashlib
import json
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import recent_pushes  # noqa: E402
import schema  # noqa: E402

ZERO = "0" * 40
A = "a" * 40
B = "b" * 40
C = "c" * 40
MINUTE = 60 * 1000


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "recent_pushes" in statement:
                self.db.execute(statement)

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)

    def count(self):
        return self.db.execute("SELECT COUNT(*) FROM recent_pushes").fetchone()[0]


@pytest.fixture
def rt(monkeypatch):
    d1 = FakeD1()
    clock = SimpleNamespace(now=10_000_000_000)
    catalog = {
        "alice/app": {"branch": "main"},
        "upstream/lib": {"branch": "trunk"},
        "bob/lib": {"branch": "main", "forkedFrom": "upstream/lib"},
        "mirror/src": {"branch": "main", "mirrorOf": "https://example.org/x"},
        "bob/src": {"branch": "main", "forkedFrom": "mirror/src"},
    }

    async def blind_index(env, text):
        return hashlib.sha256(str(text).strip().lower().encode()).hexdigest()

    async def encrypt_row(env, obj):
        return json.dumps(obj)

    async def decrypt_row(env, stored):
        return json.loads(stored)

    async def catalog_record(env, owner, repo):
        return catalog.get((owner + "/" + repo).lower())

    async def mirror_fields(env, owner, repo, record=None):
        record = record or {}
        return {"readOnly": bool(record.get("mirrorOf"))}

    async def public_path(env, owner, repo):
        return "/%s/%s" % (owner, repo)

    async def nothing(env):
        return None

    async def body(request):
        return request.body

    async def authed(env, request, data=None):
        return request.account

    def json_response(data, status=200, cache_control=None, **_):
        return SimpleNamespace(status=status, data=data,
                               cache_control=cache_control)

    runtime = {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "_repo_catalog_record": catalog_record,
        "_read_only_mirror_fields": mirror_fields,
        "_repo_public_path": public_path, "ensure_schema": nothing,
        "bounded_json_request": body, "_authed_account_name": authed,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "Date": SimpleNamespace(now=lambda: clock.now),
    }
    for name, value in runtime.items():
        monkeypatch.setattr(recent_pushes, name, value, raising=False)
    return SimpleNamespace(d1=d1, clock=clock, catalog=catalog,
                           blind_index=blind_index)


def run(coro):
    return asyncio.run(coro)


def push(owner, repo, account, *updates, alias=""):
    return run(recent_pushes.remember(
        SimpleNamespace(), owner, repo, account,
        [{"ref": ref, "old": old, "new": new} for ref, old, new in updates],
        alias=alias))


def get(account, env=None):
    request = SimpleNamespace(method="GET", account=account, body=None)
    return run(recent_pushes.handler(env or SimpleNamespace(), request))


def post(account, body):
    request = SimpleNamespace(method="POST", account=account, body=body)
    return run(recent_pushes.handler(SimpleNamespace(), request))


def branches(response):
    return [item["branch"] for item in response.data["pushes"]]


def test_only_the_signed_in_users_own_pushes_are_listed(rt):
    push("alice", "app", "alice", ("refs/heads/feature", ZERO, A))
    push("alice", "app", "carol", ("refs/heads/pr/carol/fix", ZERO, B))
    alice = get("alice")
    assert alice.status == 200
    assert alice.cache_control == "private, no-store"
    assert branches(alice) == ["feature"]
    item = alice.data["pushes"][0]
    assert item["repo"] == "alice/app"
    assert item["head"] == A
    assert item["base"] == "main"
    assert item["fork"] is False
    assert item["target"] == "alice/app"
    assert item["compareUrl"] == "/alice/app/compare/main...feature"
    assert item["branchUrl"] == "/alice/app/tree/feature"
    assert item["pushedAt"] == rt.clock.now
    carol = get("carol")
    assert branches(carol) == ["pr/carol/fix"]
    assert carol.data["pushes"][0]["compareUrl"] == (
        "/alice/app/compare/main...pr/carol/fix")
    assert branches(get("dave")) == []


def test_signed_out_requests_get_an_empty_list(rt):
    push("alice", "app", "alice", ("refs/heads/feature", ZERO, A))
    response = get("")
    assert response.status == 200
    assert response.data == {"ok": True, "signedIn": False, "pushes": []}
    assert post("", {"id": "a" * 64 + ":" + "b" * 64}).status == 401


def test_default_branch_tags_ledger_and_deleted_branches_are_left_out(rt):
    push("alice", "app", "alice",
         ("refs/heads/main", A, B),
         ("refs/tags/v1", ZERO, A),
         ("refs/heads/forkmesh/pulls", A, B),
         ("refs/heads/keep", ZERO, A),
         ("refs/heads/gone", ZERO, B))
    assert sorted(branches(get("alice"))) == ["gone", "keep"]
    push("alice", "app", "alice", ("refs/heads/gone", B, ZERO))
    assert branches(get("alice")) == ["keep"]


def test_a_branch_deleted_from_the_web_is_forgotten(rt):
    push("alice", "app", "alice", ("refs/heads/feature", ZERO, A))
    repo_bi = run(rt.blind_index(None, "alice/app"))
    run(recent_pushes.forget_branches(SimpleNamespace(), repo_bi, ["feature"]))
    assert branches(get("alice")) == []


def test_the_time_window_and_newest_first_order(rt):
    push("alice", "app", "alice", ("refs/heads/old", ZERO, A))
    rt.clock.now += 30 * MINUTE
    push("alice", "app", "alice", ("refs/heads/new", ZERO, B))
    assert branches(get("alice")) == ["new", "old"]
    rt.clock.now += 31 * MINUTE
    assert branches(get("alice")) == ["new"]
    assert get("alice").data["windowMinutes"] == 60
    wide = SimpleNamespace(RECENT_PUSH_WINDOW_MINUTES="120")
    assert branches(get("alice", wide)) == ["new", "old"]
    rt.clock.now += 60 * MINUTE
    assert branches(get("alice")) == []


def test_a_new_push_moves_the_row_and_old_rows_are_pruned_after_a_day(rt):
    push("alice", "app", "alice", ("refs/heads/feature", ZERO, A))
    rt.clock.now += 10 * MINUTE
    push("alice", "app", "alice", ("refs/heads/feature", A, B))
    response = get("alice")
    assert branches(response) == ["feature"]
    assert response.data["pushes"][0]["head"] == B
    assert rt.d1.count() == 1
    rt.clock.now += 25 * 60 * MINUTE
    push("alice", "app", "alice", ("refs/heads/other", ZERO, C))
    assert rt.d1.count() == 1


def test_a_fork_branch_compares_back_to_the_parent(rt):
    push("bob", "lib", "bob", ("refs/heads/fix", ZERO, A))
    item = get("bob").data["pushes"][0]
    assert item["fork"] is True
    assert item["repo"] == "bob/lib"
    assert item["target"] == "upstream/lib"
    assert item["targetUrl"] == "/upstream/lib"
    assert item["base"] == "trunk"
    assert item["compareUrl"] == "/bob/lib/compare/trunk...fix"
    push("bob", "lib", "bob", ("refs/heads/main", A, B))
    assert branches(get("bob")) == ["fix"]


def test_a_fork_of_a_read_only_mirror_opens_its_pull_on_the_fork(rt):
    push("bob", "src", "bob", ("refs/heads/fix", ZERO, A))
    item = get("bob").data["pushes"][0]
    assert item["fork"] is False
    assert item["target"] == "bob/src"
    assert item["compareUrl"] == "/bob/src/compare/main...fix"


def test_branches_with_an_open_pull_request_are_left_out(rt):
    push("alice", "app", "alice",
         ("refs/heads/one", ZERO, A), ("refs/heads/two", ZERO, B))
    push("bob", "lib", "bob", ("refs/heads/fix", ZERO, C))
    app_bi = run(rt.blind_index(None, "alice/app"))
    lib_bi = run(rt.blind_index(None, "upstream/lib"))
    run(recent_pushes.mark_pulled(SimpleNamespace(), app_bi, "one"))
    run(recent_pushes.mark_pulled(SimpleNamespace(), lib_bi, "bob:fix"))
    assert branches(get("alice")) == ["two"]
    assert branches(get("bob")) == []
    rt.clock.now += MINUTE
    push("alice", "app", "alice", ("refs/heads/one", A, C))
    assert branches(get("alice")) == ["two"]


def test_the_owner_marks_a_branch_pulled_by_its_id(rt):
    push("alice", "app", "alice", ("refs/heads/feature", ZERO, A))
    push("alice", "app", "carol", ("refs/heads/feature", ZERO, A))
    ident = get("alice").data["pushes"][0]["id"]
    assert post("alice", {"id": "nope"}).status == 400
    assert post("carol", {"id": ident}).status == 200
    assert branches(get("alice")) == ["feature"]
    assert post("alice", {"id": ident}).status == 200
    assert branches(get("alice")) == []
    assert branches(get("carol")) == []


def test_alias_owner_names_the_public_repository(rt):
    push("node7", "app", "alice", ("refs/heads/feature", ZERO, A),
         alias="acme")
    item = get("alice").data["pushes"][0]
    assert item["repo"] == "acme/app"
    assert item["compareUrl"] == "/acme/app/compare/main...feature"


def test_branch_names_are_validated_and_escaped_in_urls(rt):
    push("alice", "app", "alice",
         ("refs/heads/feat/a+b", ZERO, A),
         ("refs/heads/../x", ZERO, A),
         ("refs/heads/-x", ZERO, A),
         ("refs/heads/<script>", ZERO, A))
    response = get("alice")
    assert branches(response) == ["feat/a+b"]
    assert response.data["pushes"][0]["compareUrl"] == (
        "/alice/app/compare/main...feat/a%2Bb")


def test_unsupported_methods_are_refused(rt):
    request = SimpleNamespace(method="DELETE", account="alice", body=None)
    assert run(recent_pushes.handler(SimpleNamespace(), request)).status == 405
