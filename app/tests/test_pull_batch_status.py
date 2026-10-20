import asyncio
import hashlib
import json
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import pull_batch  # noqa: E402
import remote_forks  # noqa: E402
import schema  # noqa: E402
import urls  # noqa: E402

TARGET = "https://forkmesh.example/acme/widget"
HEAD_A = "a" * 40
HEAD_B = "b" * 40
HEAD_C = "c" * 40
HEAD_D = "d" * 40
UPSTREAM = "e" * 40
SHARED = "f" * 40


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "repo_remote_forks" in statement or "pull_batch_sent" in statement:
                self.db.execute(statement)

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)


class FakeResponse:
    def __init__(self, status, value):
        self.status = status
        self._text = json.dumps(value)

    async def text(self):
        return self._text


@pytest.fixture
def runtime(monkeypatch):
    d1 = FakeD1()
    state = SimpleNamespace(
        d1=d1,
        tips={"main": "1" * 40, "fm/merged": HEAD_A, "fm/pending": HEAD_B,
              "fm/sent": HEAD_C, "fm/child": HEAD_D},
        upstream_tips={"main": UPSTREAM},
        merged_heads={HEAD_A},
        reads=[],
        mirror_of={("forkmesh-node", "forkmesh"): "https://app.forkmesh.example/acme/widget"},
    )

    async def blind_index(env, text):
        return hashlib.sha256(text.encode()).hexdigest()

    async def body(request):
        return request.body

    async def actor(env, request, data):
        return request.actor

    async def owns(env, actor_name, owner):
        return actor_name == owner

    async def no(*_args):
        return False

    async def nothing(env):
        return None

    async def mirror_fields(env, owner, name):
        page = state.mirror_of.get((owner, name), "")
        return {"mirrorOf": page, "readOnly": bool(page)}

    async def routed(env, url, operation, bypass_cache=False):
        parsed = urlparse(url)
        parts = parsed.path.split("/")
        owner, repo = parts[3], parts[4]
        query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
        state.reads.append((owner, repo, operation, query))
        if operation == "branches":
            tips = state.upstream_tips if (owner, repo) == ("forkmesh-node", "forkmesh") else state.tips
            return FakeResponse(200, {
                "defaultBranch": "main",
                "branches": [{"name": name, "commit": commit} for name, commit in tips.items()],
            })
        assert operation == "compare" and query["cherry"] == "forkmesh-node/forkmesh"
        head = query["head"]
        return FakeResponse(200, {"ok": True, "commits": [
            {"commit": head, "upstream": head in state.merged_heads}]})

    def json_response(data, status=200, cache_control=None, extra_headers=None):
        return SimpleNamespace(status=status, data=data)

    shared = {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "bounded_json_request": body,
        "_authed_account_name": actor, "_account_owns_node": owns,
        "_org_owner_of_linked_repo": no, "_repo_is_private": no,
        "ensure_schema": nothing, "json_response": json_response,
        "method_name": lambda request: request.method,
        "_routed_repository_read": routed,
        "_read_only_mirror_fields": mirror_fields,
        "_instance_solo_flagship_node": lambda env: "forkmesh-node",
    }
    for module in (remote_forks, pull_batch):
        for name, value in shared.items():
            monkeypatch.setattr(module, name, value, raising=False)
    pull_batch._cherry_cache.clear()
    asyncio.run(d1.run(None, "INSERT INTO repo_remote_forks VALUES (?,?,?,?)",
                       hashlib.sha256(b"remote-forks:ada/widget").hexdigest(),
                       "u", 1, json.dumps({"url": TARGET})))
    return state


def call(body, actor="ada", method="POST"):
    request = SimpleNamespace(method=method, actor=actor, body=body,
                              url="https://code.example.com/api/repo/ada/widget/pull-batch")
    return asyncio.run(pull_batch.handler(SimpleNamespace(), request, "ada", "widget", "ada"))


def rows(*names, **compare_bases):
    return [{"branch": name, "compareBase": compare_bases.get(name.replace("/", "_"), SHARED)}
            for name in names]


def test_the_route_is_registered():
    assert urls.REPO_PULL_BATCH_RE.match("/api/repo/ada/widget/pull-batch").groups() == ("ada", "widget")


def test_only_the_owner_reads_or_records(runtime):
    body = {"action": "status", "target": TARGET, "rows": rows("fm/pending")}
    assert call(body, actor="").status == 403
    assert call(body, actor="mallory").status == 403
    assert call({"action": "record", "target": TARGET, "sent": [
        {"branch": "fm/pending", "head": HEAD_B, "number": 9}]}, actor="mallory").status == 403
    assert runtime.d1.db.execute("SELECT COUNT(*) FROM pull_batch_sent").fetchone()[0] == 0
    assert call(body, method="GET").status == 405
    assert call({**body, "target": "https://elsewhere.example/a/b"}).status == 400


def test_merged_sent_missing_and_pending_rows_are_told_apart(runtime):
    recorded = call({"action": "record", "target": TARGET, "sent": [
        {"branch": "fm/sent", "head": HEAD_C, "number": 41},
        {"branch": "fm/child", "head": "9" * 40, "number": 12},
        {"branch": "../bad", "head": HEAD_C, "number": 1},
    ]})
    assert recorded.status == 200 and recorded.data["saved"] == 2
    out = call({"action": "status", "target": TARGET, "rows": rows(
        "fm/merged", "fm/pending", "fm/sent", "fm/gone", "fm/child")})
    assert out.status == 200
    assert out.data["upstream"] == {"repo": "forkmesh-node/forkmesh", "branch": "main", "commit": UPSTREAM}
    states = {name: row["state"] for name, row in out.data["rows"].items()}
    assert states == {"fm/merged": "merged", "fm/pending": "pending", "fm/sent": "sent",
                      "fm/gone": "missing", "fm/child": "pending"}
    assert out.data["rows"]["fm/sent"]["sent"]["number"] == 41
    assert out.data["rows"]["fm/child"]["sentBefore"]["number"] == 12
    assert out.data["rows"]["fm/pending"]["head"] == HEAD_B


def test_comparisons_use_the_row_compare_base_and_are_cached_by_sha(runtime):
    body = {"action": "status", "target": TARGET,
            "rows": rows("fm/pending", "fm/child", fm_child="fm/pending")}
    call(body)
    compares = [query for *_rest, operation, query in runtime.reads if operation == "compare"]
    assert [(q["head"], q["base"], q.get("limit")) for q in compares] == [
        (HEAD_B, UPSTREAM, SHARED), (HEAD_D, UPSTREAM, HEAD_B)]
    runtime.reads.clear()
    call(body)
    assert not [read for read in runtime.reads if read[2] == "compare"]
    runtime.upstream_tips["main"] = "2" * 40
    call(body)
    assert len([read for read in runtime.reads if read[2] == "compare"]) == 2


def test_without_a_local_mirror_of_the_target_nothing_is_hidden_as_merged(runtime):
    runtime.mirror_of.clear()
    out = call({"action": "status", "target": TARGET, "rows": rows("fm/merged")})
    assert out.data["upstream"] is None
    assert out.data["rows"]["fm/merged"]["state"] == "pending"
    assert out.data["rows"]["fm/merged"]["upstream"] == "unchecked"
