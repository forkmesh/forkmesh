import asyncio
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import pull_attention  # noqa: E402
import schema  # noqa: E402

BASE = "1" * 40
BASE2 = "2" * 40
HEAD = "3" * 40
HEAD2 = "4" * 40
PATCH_HEAD = "5" * 40
BOB_KEY = "bob-key"


def pull_md(number=7, status="open", derive="branch", head="bob/fix",
            draft=""):
    lines = ["---", "number: %d" % number, "title: Fix it", "base: main",
             "head: %s" % head, "status: %s" % status, "author: %s" % BOB_KEY,
             "authorName: bob"]
    if derive == "branch":
        lines.append("derive: branch")
    else:
        lines.append("headOid: %s" % PATCH_HEAD)
    if draft:
        lines.append("draft: %s" % draft)
    return "\n".join(lines + ["---", "", "Body"]) + "\n"


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "pull_merge_watch" in statement or "pull_quick_replies" in statement:
                self.db.execute(statement)

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        cursor = self.db.execute(sql, args)
        return SimpleNamespace(meta=SimpleNamespace(changes=cursor.rowcount))

    def rows(self):
        return [dict(row) for row in self.db.execute(
            "SELECT * FROM pull_merge_watch ORDER BY number").fetchall()]


class Response:
    def __init__(self, status, body):
        self.status = status
        self._body = body

    async def text(self):
        return json.dumps(self._body)


@pytest.fixture
def rt(monkeypatch):
    d1 = FakeD1()
    world = SimpleNamespace(
        now=10_000_000_000,
        records={7: pull_md()},
        tips={"main": BASE, "bob/fix": HEAD},
        preview={"clean": False, "conflicts": ["app.py", "web/index.html"],
                 "behind": 3, "ahead": 1},
        reads=[], pings=[], accounts={BOB_KEY: "bob"}, budget=False,
        private=False,
    )

    async def blind_index(env, text):
        return hashlib.sha256(str(text).strip().lower().encode()).hexdigest()

    async def encrypt_row(env, obj):
        return json.dumps(obj)

    async def decrypt_row(env, stored):
        return json.loads(stored)

    async def routed(env, url, operation, bypass_cache=False):
        query = {key: values[0] for key, values in parse_qs(urlparse(url).query).items()}
        world.reads.append((operation, query))
        if operation == "blob":
            number = int(re.search(r"pulls/(\d+)/pull.md", query["path"]).group(1))
            text = world.records.get(number)
            if text is None or query.get("ref") != "forkmesh/pulls":
                return Response(404, {"ok": False, "error": "not_found"})
            return Response(200, {"ok": True, "content": text})
        if operation == "branches":
            return Response(200, {"ok": True, "defaultBranch": "main", "branches": [
                {"name": name, "commit": commit} for name, commit in world.tips.items()]})
        if operation == "merge-preview":
            return Response(200, {"ok": True, "baseOid": query["base"],
                                  "headOid": query["head"], **world.preview})
        return Response(404, {})

    async def enqueue(env, recipient, kind, title, body="", repo="", href="",
                      actor="", source="", dedupe="", ts=0, meta=None, **_):
        world.pings.append({"recipient": recipient, "kind": kind, "title": title,
                            "body": body, "repo": repo, "href": href,
                            "actor": actor, "source": source, "dedupe": dedupe,
                            "meta": meta})
        return True

    async def account_for_key(env, key):
        return world.accounts.get(key, "")

    async def nothing(env):
        return None

    async def body(request):
        return request.body

    async def authed(env, request, data=None):
        return request.account

    async def private(env, owner, repo):
        return world.private

    def json_response(data, status=200, cache_control=None, **_):
        return SimpleNamespace(status=status, data=data)

    runtime = {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "_routed_repository_read": routed,
        "_forkbot_blob_text": lambda blob: blob.get("content", "") if blob.get("ok") else "",
        "enqueue_notification": enqueue,
        "_git_push_account_for_pubkey": account_for_key,
        "valid_node_name": lambda name: bool(re.fullmatch(r"[a-z][a-z0-9-]{0,62}", str(name or ""))),
        "valid_node_pubkey": lambda key: str(key or "").endswith("-key"),
        "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
        "_public_base_url": lambda env, request=None: "https://forkmesh.test",
        "_cron_budget_exhausted": lambda: world.budget,
        "clean_string": lambda value, limit=200: str(value or "")[:limit],
        "NOTIFICATION_BODY_MAX": 900,
        "NOTIFICATION_ITEM_URL_SEGMENTS": {"pull": "pulls"},
        "REPO_API_PREFIX_RE": re.compile(r"^/api/repo/([^/]+)/([^/]+)(?:/.*)?$"),
        "safe_segment": lambda value: str(value or ""),
        "ensure_schema": nothing, "bounded_json_request": body,
        "_authed_account_name": authed, "_repo_is_private": private,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "Date": SimpleNamespace(now=lambda: world.now),
    }
    for name, value in runtime.items():
        monkeypatch.setattr(pull_attention, name, value, raising=False)
    pull_attention._prune_memo["at"] = 0
    return SimpleNamespace(d1=d1, world=world)


def run(coro):
    return asyncio.run(coro)


ENV = SimpleNamespace()


def check(number=7):
    return run(pull_attention.check(ENV, "alice", "app", number))


def previews(rt):
    return [read for read in rt.world.reads if read[0] == "merge-preview"]


def test_a_conflict_notifies_the_author_once_per_base_and_head(rt):
    result = check()
    assert result["state"] == "conflict" and result["notified"] is True
    assert len(rt.world.pings) == 1
    ping = rt.world.pings[0]
    assert ping["recipient"] == "bob" and ping["kind"] == "pull_attention"
    assert ping["title"] == "Pull request #7 has conflicts with main"
    assert ping["body"].startswith(
        "Your pull request #7 has conflicts with main; please rebase or merge "
        "main and push again.")
    assert "Conflicting files: app.py, web/index.html" in ping["body"]
    assert "git switch bob/fix\ngit merge origin/main" in ping["body"]
    assert ping["href"] == "/alice/app/pulls/7"
    assert ping["source"] == "pull" and ping["actor"] == ""
    assert ping["meta"] == {"number": 7, "source": "pull", "reason": "conflict",
                            "base": "main", "head": "bob/fix",
                            "files": ["app.py", "web/index.html"]}
    assert ping["dedupe"] == "pull-conflict:alice/app#7:%s:%s" % (BASE, HEAD)

    again = check()
    assert again["cached"] is True and again["notified"] is False
    assert len(previews(rt)) == 1
    assert len(rt.world.pings) == 1

    rt.world.tips["main"] = BASE2
    moved = check()
    assert moved["notified"] is True and len(rt.world.pings) == 2
    assert len(previews(rt)) == 2


def test_a_clean_pull_is_recorded_without_a_ping(rt):
    rt.world.preview = {"clean": True, "conflicts": [], "behind": 0, "ahead": 1}
    assert check()["state"] == "clean"
    assert rt.world.pings == []
    row = rt.d1.rows()[0]
    assert row["due_at"] == 0 and row["checked_at"] == rt.world.now
    assert row["trial_bi"] and row["notified_bi"] == ""


def test_drafts_and_closed_pulls_never_ping(rt):
    rt.world.records[7] = pull_md(draft="true")
    assert check()["state"] == "conflict"
    assert rt.world.pings == []
    rt.world.records[7] = pull_md(status="closed")
    assert check()["state"] == "closed"
    assert rt.d1.rows() == []
    rt.world.records[7] = pull_md(status="merged")
    assert check()["state"] == "closed"
    assert rt.world.pings == []


def test_a_patch_backed_pull_is_tried_at_its_head_and_told_to_resubmit(rt):
    rt.world.records[7] = pull_md(derive="patch", head="bob:fix")
    check()
    preview = previews(rt)[0][1]
    assert preview == {"base": BASE, "head": PATCH_HEAD}
    body = rt.world.pings[0]["body"]
    assert "git fetch https://forkmesh.test/alice/app.git main" in body
    assert "git rebase FETCH_HEAD" in body
    assert "submit the pull request again" in body


def test_an_unreadable_record_backs_off_then_gives_up(rt):
    rt.world.records = {}
    now = rt.world.now
    run(pull_attention.watch(ENV, "alice", "app", 7, base="main", head="bob/fix",
                             derive="branch", now=now))
    for minutes in (10, 30, 120, 360):
        assert check()["state"] == "unknown"
        assert rt.d1.rows()[0]["due_at"] == now + minutes * 60 * 1000
    assert check()["state"] == "unknown"
    assert rt.d1.rows()[0]["due_at"] == 0


def test_pushes_mark_pulls_on_the_moved_base_or_head_due(rt):
    for number, base, head in ((7, "main", "bob/fix"), (8, "release", "carol/x"),
                               (9, "main", "dave:y")):
        run(pull_attention.watch(ENV, "alice", "app", number, base=base,
                                 head=head, derive="patch" if ":" in head else "branch",
                                 due=False))
    assert [row["due_at"] for row in rt.d1.rows()] == [0, 0, 0]
    run(pull_attention.pushed(ENV, "alice", "app", [
        {"ref": "refs/heads/main", "old": BASE, "new": BASE2},
        {"ref": "refs/tags/v1", "old": "0" * 40, "new": HEAD}]))
    assert [row["due_at"] > 0 for row in rt.d1.rows()] == [True, False, True]
    rt.world.now += 1
    run(pull_attention.moved(ENV, "alice", "app", ["carol/x"]))
    assert rt.d1.rows()[1]["due_at"] == rt.world.now
    run(pull_attention.moved(ENV, "alice", "other", ["release"]))
    assert rt.d1.rows()[1]["due_at"] == rt.world.now


def test_a_merge_drops_the_pull_and_rechecks_its_base(rt):
    run(pull_attention.watch(ENV, "alice", "app", 7, base="main", head="bob/fix",
                             derive="branch", due=False))
    run(pull_attention.watch(ENV, "alice", "app", 8, base="main", head="carol/x",
                             derive="branch", due=False))
    run(pull_attention.watch(ENV, "alice", "app", 9, base="release", head="d/y",
                             derive="branch", due=False))
    run(pull_attention.merged(ENV, "alice", "app", 7))
    rows = rt.d1.rows()
    assert [row["number"] for row in rows] == [8, 9]
    assert rows[0]["due_at"] > 0 and rows[1]["due_at"] == 0


def test_the_cron_is_bounded_and_stops_when_the_budget_runs_out(rt):
    for number in (7, 8, 9):
        rt.world.records[number] = pull_md(number=number)
        run(pull_attention.watch(ENV, "alice", "app", number, base="main",
                                 head="bob/fix", derive="branch"))
    result = run(pull_attention.cron(ENV))
    assert result["checked"] == pull_attention.CRON_BATCH == 2
    assert sum(1 for row in rt.d1.rows() if row["due_at"]) == 1
    rt.world.budget = True
    assert run(pull_attention.cron(ENV))["checked"] == 0
    rt.world.budget = False
    assert run(pull_attention.cron(ENV))["checked"] == 1
    assert {ping["meta"]["number"] for ping in rt.world.pings} == {7, 8, 9}
    assert run(pull_attention.cron(ENV))["checked"] == 0
    rt.world.now += pull_attention.RECHECK_MS + 1
    assert run(pull_attention.cron(ENV))["checked"] == 2
    assert len(rt.world.pings) == 3


def test_a_quick_reply_is_claimed_once_per_reviewer_and_head(rt):
    claim = lambda reviewer, quick: run(pull_attention.claim_quick_reply(
        ENV, "repo", 7, reviewer, quick))
    assert claim("carol", {"id": "tests", "head": HEAD}) is True
    assert claim("carol", {"id": "tests", "head": HEAD}) is False
    assert claim("carol", {"id": "tests", "head": HEAD2}) is True
    assert claim("dave", {"id": "tests", "head": HEAD}) is True
    assert claim("carol", {"id": "split", "head": HEAD}) is True
    assert claim("carol", {"id": "Bad Id", "head": HEAD}) is None
    assert claim("carol", "tests") is None
    assert sorted(run(pull_attention.sent_quick_replies(
        ENV, "repo", 7, "carol", HEAD))) == ["split", "tests"]


def test_a_review_pings_the_pull_author_with_what_was_asked(rt):
    event = {"type": "review", "state": "changes_requested", "id": "review-web-1",
             "body": "Please add tests for this change."}
    recipient = run(pull_attention.review_posted(
        ENV, "alice", "app", 7, event, "carol", public_owner="acme",
        quick={"id": "tests", "head": HEAD}))
    assert recipient == "bob"
    ping = rt.world.pings[0]
    assert ping["title"] == "carol requested changes on your pull request #7"
    assert ping["body"] == "Please add tests for this change."
    assert ping["href"] == "/acme/app/pulls/7" and ping["repo"] == "acme/app"
    assert ping["actor"] == "carol" and ping["kind"] == "pull_attention"
    assert ping["meta"] == {"number": 7, "source": "pull", "reason": "review",
                            "state": "changes_requested", "quickReply": "tests"}
    assert ping["dedupe"] == "pull-review:alice/app#7:review-web-1"
    assert rt.d1.rows()[0]["due_at"] > 0
    blob_reads = sum(1 for read in rt.world.reads if read[0] == "blob")

    approved = dict(event, state="approved", id="review-web-2", body="Thanks! Looks good.")
    run(pull_attention.review_posted(ENV, "alice", "app", 7, approved, "carol"))
    assert rt.world.pings[1]["title"] == "carol approved your pull request #7"
    assert sum(1 for read in rt.world.reads if read[0] == "blob") == blob_reads

    assert run(pull_attention.review_posted(
        ENV, "alice", "app", 7, dict(event, id="x"), "bob")) == ""
    assert run(pull_attention.review_posted(
        ENV, "alice", "app", 7, {"type": "comment", "body": "hi"}, "carol")) == ""
    comment = run(pull_attention.review_posted(
        ENV, "alice", "app", 7, {"type": "comment", "id": "c1", "body": "Thanks"},
        "carol", quick={"id": "thanks-note", "head": HEAD}))
    assert comment == "bob"
    assert rt.world.pings[-1]["title"] == "carol commented on your pull request #7"
    assert len(rt.world.pings) == 3


def test_a_closed_pull_gets_no_review_ping(rt):
    rt.world.records[7] = pull_md(status="closed")
    event = {"type": "review", "state": "approved", "id": "r", "body": "ok"}
    assert run(pull_attention.review_posted(ENV, "alice", "app", 7, event, "carol")) == ""
    assert rt.world.pings == []


def request(method, account="carol", body=None, url="https://forkmesh.test/api/repo/alice/app/pulls/7/attention"):
    return SimpleNamespace(method=method, account=account, body=body, url=url)


def test_the_page_report_checks_a_reported_conflict_at_once(rt):
    handler = lambda req: run(pull_attention.handler(ENV, req, "alice", "app", 7))
    assert handler(request("POST", account="", body={})).status == 401
    assert handler(request("POST", body={"baseOid": "x"})).status == 400
    rt.world.private = True
    assert handler(request("POST", body={"baseOid": BASE, "headOid": HEAD})).status == 404
    rt.world.private = False

    report = {"baseOid": BASE, "headOid": HEAD, "clean": False, "base": "main",
              "head": "bob/fix", "derive": "branch"}
    first = handler(request("POST", body=report))
    assert first.data["state"] == "conflict" and first.data["notified"] is True
    assert len(rt.world.pings) == 1
    reads = len(rt.world.reads)
    second = handler(request("POST", body=report))
    assert second.data == {"ok": True, "state": "conflict", "cached": True}
    assert len(rt.world.reads) == reads

    rt.world.tips["bob/fix"] = HEAD2
    third = handler(request("POST", body=dict(report, headOid=HEAD2)))
    assert third.data["state"] == "queued"
    assert rt.d1.rows()[0]["due_at"] == rt.world.now


def test_a_clean_report_only_queues_a_background_check(rt):
    handler = lambda req: run(pull_attention.handler(ENV, req, "alice", "app", 7))
    result = handler(request("POST", body={"baseOid": BASE, "headOid": HEAD,
                                           "clean": True, "base": "main",
                                           "head": "bob/fix", "derive": "branch"},
                             url="https://forkmesh.test/api/repo/acme/app/pulls/7/attention"))
    assert result.data["state"] == "queued"
    assert rt.world.reads == []
    row = rt.d1.rows()[0]
    assert row["due_at"] == rt.world.now
    assert json.loads(row["data"])["publicOwner"] == "acme"


def test_the_page_reads_back_its_sent_quick_replies(rt):
    run(pull_attention.claim_quick_reply(
        ENV, run(pull_attention.repo_key(ENV, "alice", "app")), 7, "carol",
        {"id": "tests", "head": HEAD}))
    result = run(pull_attention.handler(
        ENV, request("GET", url="https://forkmesh.test/api/repo/alice/app/pulls/7/attention?head=" + HEAD),
        "alice", "app", 7))
    assert result.data == {"ok": True, "head": HEAD, "sent": ["tests"]}


ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
CORE_PUSH = (ROOT / "src" / "core_push.py").read_text(encoding="utf-8")
CATALOG = (ROOT / "src" / "catalog_routes.py").read_text(encoding="utf-8")
ROUTES = (ROOT / "src" / "route_handlers.py").read_text(encoding="utf-8")
URLS = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")


def test_the_relay_wires_every_trigger():
    handler = ENTRY[ENTRY.index("async def pulls_handler("):]
    handler = handler[:handler.index('\n    if method == "GET":')]
    claim = handler.index("pull_attention_claim_quick_reply(")
    assert handler.index("_inbox_author_over_quota(", handler.index("if isinstance(event, dict):\n            try:")) < claim
    assert claim < handler.index('"INSERT INTO pull_inbox (repo_bi, data, submitter_bi) "', claim)
    assert '{"error": "quick_reply_sent"}, status=409' in handler
    assert "pull_attention_review_posted(" in handler
    assert "skip=(pull_author,) if pull_author else ()" in handler
    assert "pull_attention_forget(" in handler
    assert "pull_attention_watch(" in handler
    assert "or name in skip):" in ENTRY
    assert 'phases.append("pull-attention")' in ENTRY
    assert 'if phase == "pull-attention":\n        return await pull_attention_cron(env)' in ENTRY
    assert '("pull_merge_watch", "repo_bi"),' in ENTRY
    assert '("pull_quick_replies", "repo_bi"),' in ENTRY
    assert "pulls/([1-9][0-9]{0,8})/attention$" in URLS
    assert "REPO_PULL_ATTENTION_RE.match(url.path)" in APP_RUNTIME
    assert "await pull_attention_pushed(env, owner, repo, updates)" in CORE_PUSH
    assert "await pull_attention_moved(" in CATALOG
    assert "await pull_attention_merged(" in ROUTES


def _entry_constant(name):
    import ast
    for node in ast.parse(ENTRY).body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and getattr(node.targets[0], "id", "") == name):
            value = node.value
            if isinstance(value, ast.Call):
                value = value.args[0]
            return ast.literal_eval(value)
    raise AssertionError(name)


def test_the_ping_kind_is_immediate_mailed_and_settled_on_close():
    for name in ("NOTIFICATION_KINDS", "NOTIFICATION_EMAIL_KINDS",
                 "NOTIFICATION_IMMEDIATE_KINDS", "NOTIFICATION_PULL_STATE_KINDS"):
        assert "pull_attention" in _entry_constant(name), name
    assert _entry_constant("NOTIFICATION_EMAIL_DEFAULTS")["pull_attention"] is True


def test_conflict_push_icon_differs_from_review():
    import push_icons
    assert push_icons.alert_icon("pull_attention", {"reason": "conflict"}) == "⚠️"
    assert push_icons.alert_icon("pull_attention", {"reason": "review"}) == "\U0001F4DD"
