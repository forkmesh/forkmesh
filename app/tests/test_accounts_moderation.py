import ast
import asyncio
import hashlib
import importlib.util
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = SRC / "entry.py"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import account_policy  # noqa: E402
import random_handles  # noqa: E402

NODE_NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
DAY = 24 * 60 * 60 * 1000
NOW = 1_800_000_000_000


def run(coro):
    return asyncio.run(coro)


def bi(text):
    return hashlib.sha256(str(text).strip().lower().encode()).hexdigest()


def fresh(name):
    spec = importlib.util.spec_from_file_location(
        name + "_under_test", SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


schema = fresh("schema")


class Response:
    def __init__(self, payload, status=200, headers=None):
        self.payload = payload
        self.status = status
        self.headers = headers or {}

    async def text(self):
        return json.dumps(self.payload)


def json_response(payload, status=200, cache_control=None, extra_headers=None,
                  cache_seconds=None):
    headers = dict(extra_headers or {})
    if cache_control:
        headers["cache-control"] = cache_control
    return Response(payload, status, headers)


class Request:
    def __init__(self, account="", method="GET", url="https://forum.test/",
                 body=None):
        self.account = account
        self.method = method
        self.url = url
        self.body = body or {}
        self.headers = {}


class World:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "community_ad" in statement:
                continue
            if re.search(r"\b(moderation_[a-z_]+|instance_settings)\b",
                         statement):
                self.db.execute(statement)
        self.now = NOW
        self.accounts = {}
        self.admins = set()
        self.orgs = {}
        self.org_repos = {}
        self.audit = []
        self.commits = []
        self.commit_status = 201

    def add_account(self, name, age_days=30, **extra):
        rec = {"name": name, "status": "active", "kind": "user",
               "created_at": self.now - age_days * DAY}
        rec.update(extra)
        self.accounts[name] = rec
        return rec

    def add_org(self, org, members=None, repos=()):
        self.orgs[org] = dict(members or {})
        for repo in repos:
            self.org_repos.setdefault(repo, set()).add(org)

    def rows(self, sql, *args):
        return [dict(r) for r in self.db.execute(sql, args).fetchall()]

    def runtime(self):
        world = self

        async def d1_all(env, sql, *args):
            return world.rows(sql, *args)

        async def d1_first(env, sql, *args):
            if sql.startswith("SELECT org_bi FROM org_repos"):
                return None
            rows = world.rows(sql, *args)
            world.db.commit()
            return rows[0] if rows else None

        async def d1_all_routed(env, sql, *args):
            if sql.startswith("SELECT org_bi FROM org_repos"):
                owner, repo = args
                return [{"org_bi": bi("org:" + org)}
                        for org in sorted(world.org_repos.get(
                            owner + "/" + repo, ()))]
            if sql.startswith("SELECT name FROM orgs"):
                return []
            return await d1_all(env, sql, *args)

        async def d1_first_routed(env, sql, *args):
            if sql.startswith("SELECT name FROM orgs WHERE org_bi=?"):
                for org in world.orgs:
                    if bi("org:" + org) == args[0]:
                        return {"name": org}
                return None
            return await d1_first(env, sql, *args)

        async def d1_run(env, sql, *args):
            world.db.execute(sql, args)
            world.db.commit()

        async def blind_index(env, text):
            return bi(text)

        async def encrypt_row(env, obj):
            return "enc:" + json.dumps(obj)

        async def decrypt_row(env, stored):
            if not stored:
                return None
            assert str(stored).startswith("enc:")
            return json.loads(stored[4:])

        async def _org_row(env, org):
            org = str(org or "").lower()
            if org in world.orgs:
                return bi("org:" + org), {"name": org}
            return bi("org:" + org), None

        async def _org_role(env, org_bi, account):
            for org, members in world.orgs.items():
                if bi("org:" + org) == org_bi:
                    return members.get(account, "")
            return ""

        async def _is_admin(env, name):
            return name in world.admins

        async def _account_owns_node(env, actor, node):
            return actor == node

        async def _account_row(env, name):
            return bi(name), world.accounts.get(name)

        async def _save_account(env, name_bi, rec):
            world.accounts[rec["name"]] = rec

        async def _account_session_record(env, request, data=None,
                                          allow_unverified=False):
            name = getattr(request, "account", "")
            if not name or name not in world.accounts:
                return "", None
            return bi(name), world.accounts[name]

        async def _repository_access_context(env, request, owner, repo,
                                             viewer=""):
            return {"repoBi": bi(owner + "/" + repo), "visibility": "public",
                    "viewer": viewer or getattr(request, "account", "")}

        async def bounded_json_request(request):
            return request.body

        async def _audit_sensitive_action(env, actor, action, *args):
            world.audit.append((actor, action))

        async def _moderation_commit_issue(env, request, held):
            world.commits.append(held)
            return json_response({"ok": True, "number": 42},
                                 status=world.commit_status)

        def clean_string(value, limit):
            return str(value or "")[:limit]

        return {
            "d1_all": d1_all_routed, "d1_first": d1_first_routed,
            "d1_run": d1_run, "blind_index": blind_index,
            "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
            "_org_row": _org_row, "_org_role": _org_role,
            "_is_admin": _is_admin, "_account_owns_node": _account_owns_node,
            "_account_row": _account_row, "_save_account": _save_account,
            "_account_session_record": _account_session_record,
            "_repository_access_context": _repository_access_context,
            "bounded_json_request": bounded_json_request,
            "_audit_sensitive_action": _audit_sensitive_action,
            "_moderation_commit_issue": _moderation_commit_issue,
            "json_response": json_response, "clean_string": clean_string,
            "valid_node_name": lambda v: bool(NODE_NAME_RE.match(v or "")),
            "method_name": lambda request: request.method,
            "Date": SimpleNamespace(now=lambda: world.now),
        }


@pytest.fixture
def world():
    account_policy.forget()
    yield World()
    account_policy.forget()


@pytest.fixture
def mod(world):
    module = fresh("moderation")
    module._bind_runtime(world.runtime())
    return module


def set_policy(mod, **values):
    setting = dict(account_policy.DEFAULTS, **values)
    run(mod.save_policy(None, setting))
    return setting


def event(kind="comment", author="", body="hi", event_id="c1"):
    return {"type": kind, "authorName": author, "body": body, "id": event_id,
            "author": "pk-" + (author or "anon")}


def gate(mod, account, kind="comment", number=7, owner="node", repo="forum",
         surface="issue"):
    request = Request(account=account, method="POST")
    return run(mod.submission_gate(
        None, request, {"number": number}, owner, repo,
        event(kind, account), surface))


# --- random handles ------------------------------------------------------


def test_suggested_handles_are_valid_random_and_recognised():
    seen = set()
    for _ in range(200):
        handle = random_handles.suggest()
        assert NODE_NAME_RE.match(handle) and len(handle) <= 63
        assert random_handles.is_random_handle(handle)
        seen.add(handle)
    assert len(seen) > 190
    assert random_handles.combinations() > 10 ** 10


def test_word_lists_hold_only_plain_lowercase_words():
    adjectives, animals = random_handles.words()
    assert len(adjectives) >= 100 and len(animals) >= 100
    for word in adjectives + animals:
        assert re.fullmatch(r"[a-z]{2,12}", word), word
    blocked = {"ass", "cock", "tit", "booby", "dick", "slut", "fat", "dumb",
               "crazy", "gay", "black", "white", "pig", "rat", "kill", "nazi"}
    assert not blocked & set(adjectives + animals)


def test_random_handle_check_rejects_chosen_names_and_odd_suffixes():
    assert not random_handles.is_random_handle("alice")
    assert not random_handles.is_random_handle("quiet-otter")
    assert not random_handles.is_random_handle("quiet-otter-7f3k-x")
    assert not random_handles.is_random_handle("evil-otter-7f3k")
    assert not random_handles.is_random_handle("quiet-otter-0o1l")
    assert random_handles.is_random_handle("quiet-otter-7f3k")


def test_suggest_skips_rejected_names_and_uses_the_given_randomness():
    class Fixed:
        def __init__(self):
            self.calls = 0

        def choice(self, items):
            self.calls += 1
            return items[0] if self.calls <= 6 else items[-1]

    rejected = []

    def reject(handle):
        rejected.append(handle)
        return len(rejected) == 1

    handle = random_handles.suggest(reject=reject, rng=Fixed())
    assert len(rejected) == 2 and handle == rejected[1]
    assert random_handles.suggest(reject=lambda h: True, attempts=3) == ""


# --- policy ----------------------------------------------------------------


def test_policy_defaults_keep_todays_behaviour_and_clean_bad_values():
    setting = account_policy.parse("")
    assert setting["signup"] == "required"
    assert setting["handles"] == "off"
    assert not setting["accountRequired"] and not setting["firstPostApproval"]
    odd = account_policy.parse(json.dumps({
        "signup": "anyone", "handles": "x", "newThreadsPerDay": "-4",
        "newCommentsPerDay": 99999999, "accountRequired": "yes"}))
    assert odd["signup"] == "required" and odd["handles"] == "off"
    assert odd["newThreadsPerDay"] == 0 and odd["newCommentsPerDay"] == 10000
    assert odd["accountRequired"] is False
    assert account_policy.parse("x" * 5000) == account_policy.DEFAULTS


def test_turning_approval_on_stamps_the_time_existing_accounts_keep_posting():
    form = {"signup": ["optional"], "handles": ["suggest"],
            "firstPostApproval": ["1"], "newAccountDays": ["7"],
            "newThreadsPerDay": ["3"], "newCommentsPerDay": ["20"]}
    after = account_policy.from_form(form, account_policy.DEFAULTS, NOW)
    assert after["approvalSince"] == NOW and after["signup"] == "optional"
    again = account_policy.from_form(form, after, NOW + DAY)
    assert again["approvalSince"] == NOW
    old = {"created_at": NOW - DAY}
    new = {"created_at": NOW + 1}
    assert not account_policy.needs_approval(after, old)
    assert account_policy.needs_approval(after, new)
    assert not account_policy.needs_approval(
        after, dict(new, moderation_trusted_at=NOW + 2))
    off = account_policy.from_form(
        {"signup": ["optional"]}, after, NOW + 2 * DAY)
    assert off["approvalSince"] == 0 and not off["firstPostApproval"]
    assert account_policy.email_optional({"signup": "invite"})
    assert not account_policy.email_optional({"signup": "required"})


def test_new_account_period_and_limits():
    setting = dict(account_policy.DEFAULTS, newAccountDays=7,
                   newThreadsPerDay=3, newCommentsPerDay=20)
    assert account_policy.is_new_account(setting, {"created_at": NOW - DAY}, NOW)
    assert not account_policy.is_new_account(
        setting, {"created_at": NOW - 8 * DAY}, NOW)
    assert not account_policy.is_new_account(
        dict(setting, newAccountDays=0), {"created_at": NOW}, NOW)
    assert account_policy.daily_limit(setting, "thread") == 3
    assert account_policy.daily_limit(setting, "comment") == 20


# --- rendering hidden posts --------------------------------------------------


RECORD = {
    "number": 7, "title": "t", "body": "opening",
    "events": [
        {"type": "open", "id": "o1", "body": "opening", "authorName": "ann",
         "attachments": ["a.png"]},
        {"type": "comment", "id": "c1", "body": "spam", "authorName": "troll"},
        {"type": "comment", "id": "c2", "body": "fine", "authorName": "bob"},
        {"type": "edit", "target": "c2", "body": "fine, edited",
         "authorName": "bob"},
        {"type": "edit", "target": "o1", "body": "opening v2",
         "authorName": "ann"},
        {"type": "comment", "id": "c3", "body": "more spam",
         "authorName": "Troll"},
    ],
}


def _bodies(record):
    return {e.get("id") or "edit:" + e.get("target", ""): e["body"]
            for e in record["events"]}


def test_hidden_posts_keep_their_place_but_lose_their_text(mod):
    out = mod.apply_to_record(RECORD, hidden=["c2"], hidden_authors=["troll"])
    bodies = _bodies(out)
    assert bodies["c1"] == bodies["c3"] == bodies["c2"] == mod.HIDDEN_TEXT
    assert bodies["edit:c2"] == mod.HIDDEN_TEXT
    assert bodies["o1"] == "opening" and bodies["edit:o1"] == "opening v2"
    assert [e["type"] for e in out["events"]] == [
        e["type"] for e in RECORD["events"]]
    assert RECORD["events"][1]["body"] == "spam"


def test_hiding_the_opening_post_covers_its_edits_and_attachments(mod):
    out = mod.apply_to_record(RECORD, hidden=["body"])
    bodies = _bodies(out)
    assert bodies["o1"] == bodies["edit:o1"] == out["body"] == mod.HIDDEN_TEXT
    assert out["events"][0]["attachments"] == []
    assert bodies["c1"] == "spam"
    assert mod.apply_to_record(RECORD) is RECORD
    text = json.dumps(RECORD)
    assert mod.apply_to_text(text) == text
    assert json.loads(mod.apply_to_text(text, ["c1"]))["events"][1][
        "moderationHidden"] is True


def test_the_host_and_dashboard_apply_the_same_rule():
    host = (ROOT / "host/forkmesh_host/repo_moderation.py").read_text("utf-8")
    js = (ROOT / "public/dashboard/js/09-moderation.js").read_text("utf-8")
    server = (SRC / "moderation.py").read_text("utf-8")
    text = re.search(r'HIDDEN_TEXT = "([^"]+)"', server).group(1)
    assert 'HIDDEN_TEXT = "%s"' % text in host
    assert 'MODERATION_HIDDEN_TEXT = "%s"' % text in js


# --- the submission gate ------------------------------------------------------


def test_anonymous_posts_are_refused_when_accounts_are_required(world, mod):
    set_policy(mod, accountRequired=True)
    refusal, _ = gate(mod, "")
    assert refusal.status == 401 and refusal.payload["error"] == "account_required"
    set_policy(mod, accountRequired=False)
    refusal, context = gate(mod, "")
    assert refusal is None and not context["hold"]


def test_blocked_accounts_cannot_post_comment_vote_or_open(world, mod):
    world.add_account("troll")
    world.add_account("mod1")
    world.add_org("example", {"lead": "owner"}, repos=["node/forum"])
    world.add_account("lead")
    org_bi = bi("org:example")
    refused = run(mod.apply_action(
        None, Request("lead"), org_bi, "org:example", "lead", True,
        {"action": "block", "name": "troll", "hideContent": True,
         "reason": "spam"}))
    assert refused is None
    for kind in ("comment", "open", "vote", "claim", "reaction"):
        refusal, _ = gate(mod, "troll", kind)
        assert refusal.status == 403
        assert refusal.payload["error"] == "account_blocked"
    refusal, _ = gate(mod, "troll", "comment", surface="discussion")
    assert refusal.payload["error"] == "account_blocked"
    refusal, _ = gate(mod, "troll", "review", surface="pull")
    assert refusal.payload["error"] == "account_blocked"
    refusal, _ = gate(mod, "troll", "comment", owner="other", repo="x")
    assert refusal is None
    assert run(mod.hidden_authors(None, {org_bi: "example"})) == ["troll"]
    run(mod.apply_action(None, Request("lead"), org_bi, "org:example",
                         "lead", True, {"action": "unblock", "name": "troll"}))
    assert gate(mod, "troll")[0] is None


def test_an_instance_block_covers_every_repository(world, mod):
    world.add_account("troll")
    scope = run(mod.instance_scope(None))
    run(mod.apply_action(None, Request("root"), scope, mod.INSTANCE_SCOPE,
                         "root", True, {"action": "block", "name": "troll"}))
    for owner, repo in (("node", "forum"), ("other", "x")):
        refusal, _ = gate(mod, "troll", owner=owner, repo=repo)
        assert refusal.payload["error"] == "account_blocked"
    assert run(mod.account_blocked(None, "troll", "other", "x"))
    response = run(mod.blocked_response(None, "troll", "other", "x"))
    assert response.status == 403


def test_administrators_and_yourself_cannot_be_blocked(world, mod):
    world.add_account("root")
    world.add_account("lead")
    world.admins.add("root")
    scope = run(mod.instance_scope(None))
    for name in ("root", "lead"):
        refused = run(mod.apply_action(
            None, Request("lead"), scope, mod.INSTANCE_SCOPE, "lead", True,
            {"action": "block", "name": name}))
        assert refused.status == 400
    refused = run(mod.apply_action(
        None, Request("lead"), scope, mod.INSTANCE_SCOPE, "lead", True,
        {"action": "block", "name": "nobody"}))
    assert refused.status == 404


def test_locked_threads_take_moderator_replies_only(world, mod):
    world.add_account("ann")
    world.add_account("mod1")
    world.add_org("example", {"mod1": "member"}, repos=["node/forum"])
    org_bi = bi("org:example")
    run(mod.apply_action(None, Request("root"), org_bi, "org:example",
                         "root", True, {"action": "add_moderator",
                                        "name": "mod1"}))
    response = run(mod.repo_handler(None, Request(
        "mod1", "POST", body={"action": "lock", "kind": "issue",
                              "number": 7}), "node", "forum"))
    assert response.payload["locked"] is True
    refusal, _ = gate(mod, "ann", "comment", number=7)
    assert refusal.status == 403 and refusal.payload["error"] == "thread_locked"
    assert gate(mod, "ann", "comment", number=8)[0] is None
    assert gate(mod, "ann", "open", number=7)[0] is None
    refusal, context = gate(mod, "mod1", "comment", number=7)
    assert refusal is None and context["moderator"]
    run(mod.repo_handler(None, Request(
        "mod1", "POST", body={"action": "unlock", "kind": "issue",
                              "number": 7}), "node", "forum"))
    assert gate(mod, "ann", "comment", number=7)[0] is None


def test_new_accounts_get_daily_thread_and_comment_limits(world, mod):
    set_policy(mod, newAccountDays=7, newThreadsPerDay=3, newCommentsPerDay=2)
    world.add_account("newbie", age_days=1)
    world.add_account("veteran", age_days=30)
    for _ in range(3):
        assert gate(mod, "newbie", "open")[0] is None
    refusal, _ = gate(mod, "newbie", "open")
    assert refusal.status == 429
    assert refusal.payload["error"] == "new_account_limit"
    assert gate(mod, "newbie", "comment")[0] is None
    assert gate(mod, "newbie", "comment")[0] is None
    assert gate(mod, "newbie", "comment")[0].status == 429
    for _ in range(5):
        assert gate(mod, "veteran", "open")[0] is None
    world.now += DAY
    assert gate(mod, "newbie", "open")[0] is None
    stored = world.rows("SELECT * FROM moderation_post_counts")
    assert {row["day"] for row in stored} == {world.now // DAY}
    columns = world.rows("PRAGMA table_info(moderation_post_counts)")
    assert {c["name"] for c in columns} == {"account_bi", "day", "kind", "count"}


def test_first_posts_of_new_accounts_are_held_for_review(world, mod):
    set_policy(mod, firstPostApproval=True, approvalSince=NOW - DAY)
    world.add_account("newbie", age_days=0)
    world.add_account("oldtimer", age_days=30)
    world.add_account("trusted", age_days=0, moderation_trusted_at=NOW)
    assert gate(mod, "newbie", "comment")[1]["hold"]
    assert gate(mod, "newbie", "open")[1]["hold"]
    assert not gate(mod, "newbie", "vote")[1]["hold"]
    assert not gate(mod, "newbie", "comment", surface="discussion")[1]["hold"]
    assert not gate(mod, "oldtimer", "comment")[1]["hold"]
    assert not gate(mod, "trusted", "comment")[1]["hold"]
    assert gate(mod, "", "comment")[1]["hold"]


def _hold(world, mod, account, kind="comment", number=7):
    refusal, context = gate(mod, account, kind, number)
    assert refusal is None and context["hold"]
    item = {"number": number if kind == "comment" else 0,
            "titleIfNew": "Hello" if kind == "open" else "",
            "event": event(kind, account, "first post"), "meta": {}}
    return run(mod.hold(None, "node", "forum", bi("node/forum"), item, context,
                        "example"))


def test_the_review_queue_flow(world, mod):
    set_policy(mod, firstPostApproval=True)
    world.add_org("example", {"lead": "owner", "mem": "member"},
                  repos=["node/forum"])
    for name in ("lead", "mem", "mod1", "newbie", "other"):
        world.add_account(name, age_days=0)
    response = _hold(world, mod, "newbie", "open")
    assert response.status == 202 and response.payload["pending"] == "review"
    _hold(world, mod, "other")
    org = Request("mem", "GET")
    assert run(mod.org_handler(None, org, "example")).status == 403
    run(mod.apply_action(None, Request("lead"), bi("org:example"),
                         "org:example", "lead", True,
                         {"action": "add_moderator", "name": "mod1"}))
    listed = run(mod.org_handler(None, Request("mod1"), "example"))
    assert listed.status == 200
    assert listed.payload["canManageModerators"] is False
    queue = listed.payload["queue"]
    assert [(q["author"], q["kind"], q["repo"]) for q in queue] == [
        ("newbie", "thread", "example/forum"), ("other", "comment",
                                                 "example/forum")]
    assert queue[0]["title"] == "Hello" and queue[0]["body"] == "first post"
    mine = run(mod.repo_handler(None, Request("other"), "node", "forum"))
    assert mine.status == 400
    view = run(mod.repo_handler(None, Request(
        "other", url="https://forum.test/?kind=issue&number=7"), "node", "forum"))
    assert view.payload["pendingMine"] == 1
    approved = run(mod.org_handler(None, Request(
        "mod1", "POST", body={"action": "approve", "id": queue[0]["id"]}),
        "example"))
    assert approved.status == 200 and len(world.commits) == 1
    assert world.commits[0]["item"]["titleIfNew"] == "Hello"
    assert "moderation_trusted_at" not in world.accounts["newbie"]
    assert gate(mod, "newbie", "comment")[1]["hold"]
    rejected = run(mod.org_handler(None, Request(
        "mod1", "POST", body={"action": "reject", "id": queue[1]["id"]}),
        "example"))
    assert rejected.payload["queue"] == [] and len(world.commits) == 1
    assert "moderation_trusted_at" not in world.accounts["other"]
    log = [(row["actor"], row["action"], row["target"])
           for row in rejected.payload["log"]]
    assert ("mod1", "approve", "newbie") in log
    assert ("mod1", "reject", "other") in log
    stored = world.rows("SELECT data FROM moderation_log")
    assert all("ip" not in json.loads(r["data"][4:]) for r in stored)


def test_only_admins_and_instance_moderators_trust_an_account_everywhere(world, mod):
    set_policy(mod, firstPostApproval=True)
    world.add_org("spam", {"spammer": "owner"}, repos=["node/forum"])
    world.add_account("spammer", age_days=0)
    world.add_account("sock", age_days=0)
    world.add_account("root")
    world.admins.add("root")
    refused = run(mod.org_handler(None, Request(
        "spammer", "POST", body={"action": "trust", "name": "sock"}), "spam"))
    assert refused.status == 403
    assert "moderation_trusted_at" not in world.accounts["sock"]
    assert gate(mod, "sock", "comment")[1]["hold"]
    _hold(world, mod, "sock")
    [row] = world.rows("SELECT id FROM moderation_queue")
    approved = run(mod.org_handler(None, Request(
        "root", "POST", body={"action": "approve", "id": row["id"]}), "spam"))
    assert approved.status == 200
    assert world.accounts["sock"]["moderation_trusted_at"] == world.now


def test_a_failed_approval_keeps_the_post_in_the_queue(world, mod):
    set_policy(mod, firstPostApproval=True)
    world.add_org("example", {"lead": "owner"}, repos=["node/forum"])
    world.add_account("lead")
    world.add_account("newbie", age_days=0)
    _hold(world, mod, "newbie")
    [row] = world.rows("SELECT id FROM moderation_queue")
    world.commit_status = 429
    failed = run(mod.org_handler(None, Request(
        "lead", "POST", body={"action": "approve", "id": row["id"]}),
        "example"))
    assert failed.status == 429
    [kept] = world.rows("SELECT claimed_at FROM moderation_queue")
    assert kept["claimed_at"] == 0
    assert "moderation_trusted_at" not in world.accounts["newbie"]


def test_the_queue_is_bounded_per_author(world, mod):
    set_policy(mod, firstPostApproval=True)
    world.add_account("newbie", age_days=0)
    for _ in range(mod.MAX_QUEUE_PER_AUTHOR):
        assert _hold(world, mod, "newbie").status == 202
    assert _hold(world, mod, "newbie").status == 429


def test_moderator_management_needs_an_owner_or_admin(world, mod):
    world.add_org("example", {"lead": "owner", "adm": "admin"},
                  repos=["node/forum"])
    for name in ("lead", "adm", "mod1", "mod2"):
        world.add_account(name)
    org_bi = bi("org:example")
    assert run(mod.can_manage_moderators(None, "adm", org_bi))
    assert not run(mod.can_manage_moderators(None, "mod1", org_bi))
    run(mod.apply_action(None, Request("adm"), org_bi, "org:example",
                         "adm", True, {"action": "add_moderator",
                                       "name": "mod1"}))
    assert run(mod.can_moderate_org(None, "mod1", org_bi))
    assert run(mod.can_moderate_repo(None, "mod1", "node", "forum"))
    assert not run(mod.can_moderate_repo(None, "mod1", "other", "x"))
    refused = run(mod.org_handler(None, Request(
        "mod1", "POST", body={"action": "add_moderator", "name": "mod2"}),
        "example"))
    assert refused.status == 403
    assert not run(mod.can_moderate_org(None, "mod2", org_bi))
    run(mod.apply_action(None, Request("adm"), org_bi, "org:example",
                         "adm", True, {"action": "remove_moderator",
                                       "name": "mod1"}))
    assert not run(mod.can_moderate_repo(None, "mod1", "node", "forum"))


def test_instance_moderators_moderate_everywhere(world, mod):
    world.add_account("watch")
    scope = run(mod.instance_scope(None))
    run(mod.apply_action(None, Request("root"), scope, mod.INSTANCE_SCOPE,
                         "root", True, {"action": "add_moderator",
                                        "name": "watch"}))
    assert run(mod.can_moderate_repo(None, "watch", "anyone", "repo"))
    world.add_org("acme", {}, repos=[])
    assert run(mod.can_moderate_org(None, "watch", bi("org:acme")))


def test_hide_and_unhide_posts(world, mod):
    world.add_org("example", {"lead": "owner"}, repos=["node/forum"])
    world.add_account("lead")
    world.add_account("ann")
    body = {"action": "hide", "kind": "issue", "number": 7,
            "target": "forge-github-comment-123", "reason": "off-topic"}
    denied = run(mod.repo_handler(None, Request("ann", "POST", body=body),
                                  "node", "forum"))
    assert denied.status == 403
    hidden = run(mod.repo_handler(None, Request("lead", "POST", body=body),
                                  "node", "forum"))
    assert hidden.payload["hidden"] == ["forge-github-comment-123"]
    bad = run(mod.repo_handler(None, Request("lead", "POST", body=dict(
        body, target="../../x")), "node", "forum"))
    assert bad.status == 400
    view = run(mod.repo_handler(None, Request(
        "", url="https://forum.test/?kind=issue&number=7"), "node", "forum"))
    assert view.payload["hidden"] == ["forge-github-comment-123"]
    assert view.payload["canModerate"] is False
    assert view.headers["cache-control"].startswith("public")
    as_mod = run(mod.repo_handler(None, Request(
        "lead", url="https://forum.test/?kind=issue&number=7"), "node", "forum"))
    assert as_mod.payload["canModerate"] is True
    assert as_mod.headers["cache-control"] == "private, no-store"
    run(mod.repo_handler(None, Request("lead", "POST", body=dict(
        body, action="unhide")), "node", "forum"))
    view = run(mod.repo_handler(None, Request(
        "", url="https://forum.test/?kind=issue&number=7"), "node", "forum"))
    assert view.payload["hidden"] == []


# --- recovery codes ----------------------------------------------------------


@pytest.fixture
def recovery(world):
    module = fresh("account_recovery")
    login = {"fails": {}, "locked": set()}
    sessions = []

    async def hash_password(text):
        return "salt", hashlib.sha256(("salt" + text).encode()).hexdigest()

    async def verify_password(text, salt, digest):
        return hashlib.sha256((salt + text).encode()).hexdigest() == digest

    async def _login_attempt_keys(env, request, identifier):
        return ["pair:" + identifier]

    async def _login_locked_until(env, key):
        return NOW + 60_000 if key in login["locked"] else 0

    async def _login_failure_response(env, keys, error):
        for key in keys:
            login["fails"][key] = login["fails"].get(key, 0) + 1
            if login["fails"][key] >= 3:
                login["locked"].add(key)
        return json_response({"error": error}, status=401)

    async def _login_clear(env, key):
        login["fails"].pop(key, None)

    async def _account_revoke_sessions(env, name_bi):
        sessions.append(name_bi)

    runtime = world.runtime()
    runtime.update({
        "hash_password": hash_password, "verify_password": verify_password,
        "_login_attempt_keys": _login_attempt_keys,
        "_login_locked_until": _login_locked_until,
        "_login_failure_response": _login_failure_response,
        "_login_clear": _login_clear,
        "_account_revoke_sessions": _account_revoke_sessions,
        "_account_kind": lambda rec: rec.get("kind", "user"),
        "MAX_NODE_NAME": 63,
        "username_has_blocked_term": lambda name: "evil" in name,
        "_random_handles": random_handles,
    })
    module._bind_runtime(runtime)
    module._test = SimpleNamespace(login=login, sessions=sessions)
    return module


def test_recovery_codes_are_long_random_and_forgiving_to_type(recovery):
    code = recovery.new_code()
    assert re.fullmatch(r"RC(-[A-Z2-7]{4}){5}", code)
    assert recovery.normalize(code.lower().replace("-", " ")) == \
        code.replace("-", "")
    assert recovery.normalize("RC-ABCD") == ""
    assert len({recovery.new_code() for _ in range(50)}) == 50


def test_only_a_hash_of_the_code_is_kept(world, recovery):
    rec = world.add_account("quiet-otter-7f3k")
    code = run(recovery.issue(rec))
    assert code not in json.dumps(rec)
    assert code.replace("-", "") not in json.dumps(rec)
    assert run(recovery.matches(rec, code))
    assert run(recovery.matches(rec, code.lower()))
    assert not run(recovery.matches(rec, recovery.new_code()))


def test_resetting_a_password_with_the_recovery_code(world, recovery):
    rec = world.add_account("quiet-otter-7f3k", pass_hash="old")
    code = run(recovery.issue(rec))
    request = Request(method="POST", body={
        "nodeName": "quiet-otter-7f3k", "recoveryCode": code,
        "newPassword": "a new passphrase"})
    response = run(recovery.recover_handler(None, request))
    assert response.status == 200
    fresh_code = response.payload["recoveryCode"]
    assert fresh_code != code
    assert rec["pass_hash"] != "old"
    assert recovery._test.sessions == [bi("quiet-otter-7f3k")]
    assert not run(recovery.matches(rec, code))
    assert run(recovery.matches(rec, fresh_code))
    again = run(recovery.recover_handler(None, request))
    assert again.status == 401


def test_wrong_codes_are_counted_and_locked_out(world, recovery):
    rec = world.add_account("quiet-otter-7f3k")
    code = run(recovery.issue(rec))
    body = {"nodeName": "quiet-otter-7f3k",
            "recoveryCode": recovery.new_code(), "newPassword": "long enough"}
    for _ in range(3):
        response = run(recovery.recover_handler(
            None, Request(method="POST", body=body)))
        assert response.status == 401
    response = run(recovery.recover_handler(
        None, Request(method="POST", body=body)))
    assert response.status == 429
    unknown = run(recovery.recover_handler(None, Request(method="POST", body=dict(
        body, nodeName="nobody-here"))))
    assert unknown.status == 401
    recovery._test.login["locked"].discard("pair:recovery:quiet-otter-7f3k")
    good = run(recovery.recover_handler(None, Request(method="POST", body=dict(
        body, recoveryCode=code))))
    assert good.status == 200
    short = run(recovery.recover_handler(None, Request(method="POST", body=dict(
        body, newPassword="short"))))
    assert short.payload["error"] == "password_too_short"


def test_a_signed_in_account_can_issue_a_new_code(world, recovery):
    rec = world.add_account("ann")
    rec["pass_salt"], rec["pass_hash"] = run(
        recovery.hash_password("correct horse"))
    wrong = run(recovery.regenerate_handler(None, Request(
        "ann", "POST", body={"password": "nope"})))
    assert wrong.status == 401
    ok = run(recovery.regenerate_handler(None, Request(
        "ann", "POST", body={"password": "correct horse"})))
    assert ok.status == 200
    assert run(recovery.matches(rec, ok.payload["recoveryCode"]))
    anonymous = run(recovery.regenerate_handler(None, Request(
        "", "POST", body={"password": "x"})))
    assert anonymous.status == 401


def test_handle_suggestions_skip_taken_and_blocked_names(world, recovery):
    handle = run(recovery.suggest_handle(None))
    assert random_handles.is_random_handle(handle)
    response = run(recovery.suggestion_handler(None, Request()))
    assert random_handles.is_random_handle(response.payload["handle"])
    assert response.headers["cache-control"].startswith("no-store")


# --- sign-up --------------------------------------------------------------------


def _signup_namespace(world, policy, code_mode=False):
    wanted = {"_account_signup", "_signup_policy"}
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    selected = [node for node in tree.body
                if isinstance(node, ast.AsyncFunctionDef)
                and node.name in wanted]
    assert {node.name for node in selected} == wanted
    saved = {}
    refusals = []
    mails = []

    async def refuse(env, reason, status, claim=None, name="", response=None,
                     detail=""):
        refusals.append(reason)
        return response or json_response({"error": reason}, status=status)

    async def nothing(*args, **kwargs):
        return None

    async def none_pair(env, name):
        return "bi-" + name, None

    async def save(env, name_bi, rec, email_bi=None, ip_bi=None):
        saved["rec"] = dict(rec)
        saved["email_bi"] = email_bi

    async def d1_first(env, sql, *args):
        return None

    async def hash_password(text):
        return "salt", hashlib.sha256(("salt" + text).encode()).hexdigest()

    async def session_response(env, request, payload, status=200):
        return json_response(payload, status=status)

    async def send_mail(env, request, name, email, rec=None):
        mails.append(email)
        return True

    async def matches(env, code):
        return True

    async def code_enabled(env):
        return code_mode

    async def true(*args, **kwargs):
        return True

    async def blind_index(env, text):
        return bi(text)

    async def public_payload(env, rec, **kwargs):
        return {"nodeName": rec["name"], "email": rec.get("email", ""),
                "emailVerified": bool(rec.get("email_verified"))}

    moderation = SimpleNamespace()

    async def policy_doc(env):
        return dict(account_policy.DEFAULTS, **policy)

    moderation.policy = policy_doc
    recovery = fresh("account_recovery")
    async def verify_password(text, salt, digest):
        return hashlib.sha256((salt + text).encode()).hexdigest() == digest

    recovery._bind_runtime({"hash_password": hash_password,
                            "verify_password": verify_password,
                            "Date": SimpleNamespace(now=lambda: NOW)})
    names = SimpleNamespace(apply_case=lambda rec, case: None,
                            clean_case=lambda a, b: a)
    namespace = {
        "_account_signup_intent_claim": lambda env, request: {"claimId": "c"},
        "_account_signup_intent_available": lambda env, claim: _value(
            {"email_field_name": "contact_" + "a" * 18}),
        "log_blocked_signup": nothing, "_signup_refused": refuse,
        "bounded_json_request": lambda request: _value(request.body),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "_turnstile": SimpleNamespace(
            signup_cdata=lambda claim: "cd", BACKUP_FIELD="backupProof",
            ACTION_SIGNUP="signup", backup_reason=lambda proof: ""),
        "_turnstile_gate": nothing, "_signup_backup_gate": nothing,
        "_attribution": SimpleNamespace(
            emit_from_request=nothing, link_user=nothing, emit=nothing),
        "_admin_signup_referral_code_normalize": lambda value: value,
        "_admin_signup_referral_code_matches": matches,
        "_admin_signup_referral_code_mode": code_enabled,
        "_rotate_admin_signup_referral_code": lambda *a: _value(0),
        "re": re,
        "valid_node_name": lambda v: bool(NODE_NAME_RE.match(v or "")),
        "valid_account_email": lambda v: bool(
            re.fullmatch(r"[^@\s]+@[a-z0-9.-]+\.[a-z]{2,}", v or "")),
        "MAX_NODE_NAME": 63, "_org_row": none_pair, "_account_row": none_pair,
        "_donation_in_progress": lambda rec: False,
        "blind_index": blind_index, "d1_first": d1_first,
        "_transient_client_address": lambda request: "",
        "signup_rate_check": nothing, "username_moderation_error": lambda *a:
        _value(""), "_consume_account_signup_intent": true,
        "hash_password": hash_password, "account_names": names,
        "Date": SimpleNamespace(now=lambda: NOW),
        "_signup_metadata": lambda request: {"at": NOW},
        "_capture_referral_source": lambda *a: _value({}),
        "marketing_email": SimpleNamespace(apply_consent=lambda *a: None),
        "_save_account": save, "_record_signup_origin": nothing,
        "_send_verification_email": send_mail, "_enqueue_verification": nothing,
        "_users_directory_cache_forget": nothing,
        "edge_cache_delete": nothing, "CHAT_ACTIVITY_CACHE_KEY": "k",
        "_record_referral_signup": lambda *a: _value(""),
        "_hackathon": SimpleNamespace(on_signup=nothing),
        "_broadcast_new_member": nothing,
        "chat_channels_api": SimpleNamespace(post_welcome=nothing),
        "_ChatChannelsRuntime": lambda env, request: None,
        "_award_badge": nothing, "FIRST_100_USERS_LIMIT": 100,
        "_account_public_payload": public_payload,
        "_account_session_device_label": lambda request: "",
        "_account_session_client_ip": lambda request: "",
        "_account_session_response": session_response,
        "_signup_page_required_response": lambda: json_response({}, 403),
        "json_response": json_response, "_moderation": moderation,
        "_account_policy": account_policy, "_random_handles": random_handles,
        "_account_recovery": recovery, "SIGNUP_PATH": "/api/accounts/signup",
    }
    module = ast.fix_missing_locations(ast.Module(selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    namespace["_saved"] = saved
    namespace["_refusals"] = refusals
    namespace["_mails"] = mails
    namespace["_recovery"] = recovery
    return namespace


async def _awaitable(value):
    return value


def _value(value):
    return _awaitable(value)


def _signup(ns, name="quiet-otter-7f3k", email="", **extra):
    body = {"nodeName": name, "contact_" + "a" * 18: email,
            "password": "long password", "email": ""}
    body.update(extra)
    return run(ns["_account_signup"](None, Request(method="POST", body=body)))


def test_email_optional_signup_stores_no_address_and_returns_a_code(world):
    ns = _signup_namespace(world, {"signup": "optional"})
    response = _signup(ns)
    assert response.status == 201
    code = response.payload["recoveryCode"]
    rec = ns["_saved"]["rec"]
    assert rec["email"] == "" and ns["_saved"]["email_bi"] is None
    assert rec["recovery_hash"] and code not in json.dumps(rec)
    assert run(ns["_recovery"].matches(rec, code))
    assert ns["_mails"] == []


def test_email_optional_still_accepts_and_verifies_an_address(world):
    ns = _signup_namespace(world, {"signup": "optional"})
    response = _signup(ns, email="ann@example.org")
    assert response.status == 201 and "recoveryCode" not in response.payload
    assert ns["_saved"]["email_bi"] == bi("ann@example.org")
    assert ns["_mails"] == ["ann@example.org"]
    bad = _signup(ns, email="not-an-address")
    assert bad.payload["error"] == "valid_email_required"


def test_email_required_is_unchanged(world):
    ns = _signup_namespace(world, {"signup": "required"})
    assert _signup(ns).payload["error"] == "valid_email_required"
    assert _signup(ns, email="ann@example.org").status == 201


def test_closed_and_invite_only_signup(world):
    ns = _signup_namespace(world, {"signup": "closed"})
    response = _signup(ns, email="ann@example.org")
    assert response.status == 403 and ns["_refusals"] == ["signup_closed"]
    ns = _signup_namespace(world, {"signup": "invite"}, code_mode=False)
    assert _signup(ns).payload["error"] == "invalid_signup_referral_code"
    ns = _signup_namespace(world, {"signup": "invite"}, code_mode=True)
    assert _signup(ns).status == 201


def test_random_only_usernames(world):
    ns = _signup_namespace(world, {"signup": "optional", "handles": "require"})
    assert _signup(ns, name="ann").payload["error"] == "random_handle_required"
    assert _signup(ns, name=random_handles.suggest()).status == 201


# --- wiring -------------------------------------------------------------------


ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
RUNTIME_TEXT = (SRC / "app_runtime.py").read_text(encoding="utf-8")


def _function(text, name):
    start = text.index("async def %s(" % name)
    end = text.index("\n\n\n", start)
    return text[start:end]


def test_every_web_submission_path_passes_the_moderation_gate():
    issue = _function(ENTRY_TEXT, "_accept_issue_submission")
    assert "moderation.submission_gate(" in issue
    assert issue.index("moderation.submission_gate(") < issue.index(
        "_forkbot_next_issue_number")
    assert issue.index("return await moderation.hold(") < issue.index(
        '"INSERT INTO issue_inbox')
    pulls = _function(ENTRY_TEXT, "pulls_handler")
    assert pulls.count('globals().get("_moderation_submission_refusal")') == 3
    discussions = _function(ENTRY_TEXT, "discussions_handler")
    assert discussions.count(
        'globals().get("_moderation_submission_refusal")') == 1
    reactions = (SRC / "comment_reactions.py").read_text(encoding="utf-8")
    assert 'globals().get("moderation_blocked_response")' in reactions


def test_moderators_may_sign_issue_and_discussion_moderation_events():
    issue = _function(ENTRY_TEXT, "_accept_issue_submission")
    assert "moderation.MODERATOR_ISSUE_EVENTS" in issue
    discussions = _function(ENTRY_TEXT, "discussions_handler")
    assert "moderation.MODERATOR_DISCUSSION_EVENTS" in discussions
    mod = fresh("moderation")
    assert {"edit", "delete", "status"} <= mod.MODERATOR_ISSUE_EVENTS
    assert not {"bounty", "agent"} & mod.MODERATOR_ISSUE_EVENTS


def test_routes_and_cleanup_are_registered():
    assert '"/api/accounts/recover" and method == "POST"' in ENTRY_TEXT
    assert '"/api/accounts/recovery-code" and method == "POST"' in ENTRY_TEXT
    assert '"/api/accounts/handle-suggestion" and method == "GET"' in ENTRY_TEXT
    assert "REPO_MODERATION_RE.match(url.path)" in RUNTIME_TEXT
    assert "ORG_MODERATION_RE.match(url.path)" in RUNTIME_TEXT
    assert '("moderation_flags", "repo_bi"),' in ENTRY_TEXT
    assert '("moderation_queue", "repo_bi"),' in ENTRY_TEXT
    for table in ("moderation_roles", "moderation_blocks",
                  "moderation_post_counts"):
        assert '("%s", "account_bi")' % table in ENTRY_TEXT
    assert '"__FORKMESH_SIGNUP_POLICY__"' in RUNTIME_TEXT
    page = (ROOT / "public/signup.html").read_text(encoding="utf-8")
    assert page.count("__FORKMESH_SIGNUP_POLICY__") == 1


def test_migration_matches_the_runtime_schema():
    up = (ROOT / "migrations/0292_accounts_moderation.sql").read_text("utf-8")
    down = (ROOT / "migrations/down/0292_accounts_moderation.sql").read_text(
        "utf-8")
    fingerprint = re.search(r"schema_fingerprint:([0-9a-f]{64})", up).group(1)
    assert fingerprint in down and fingerprint in ENTRY_TEXT
    db = sqlite3.connect(":memory:")
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
    db.executescript(up)
    tables = {row[0] for row in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name LIKE 'moderation_%'")}
    assert tables == {"moderation_roles", "moderation_blocks",
                      "moderation_flags", "moderation_queue",
                      "moderation_post_counts", "moderation_log"}
    for table in tables:
        columns = {row[1] for row in db.execute(
            "PRAGMA table_info(%s)" % table)}
        assert not {"ip", "ip_bi", "address", "email"} & columns
    db.executescript(down)
    assert not list(db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'moderation_%'"))


def test_the_dashboard_hides_exactly_what_the_server_hides(mod):
    import shutil
    import subprocess
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    source = (ROOT / "public/dashboard/js/09-moderation.js").read_text("utf-8")
    start = source.index("  function applyIssueModeration(")
    end = source.index("\n  }\n", start) + 4
    constant = re.search(r"  const MODERATION_HIDDEN_TEXT = [^\n]+\n", source)
    script = (constant.group(0) + source[start:end] +
              "\nconst input = JSON.parse(require('fs').readFileSync(0, 'utf8'));"
              "\nconst moderation = {hidden: input.hidden, hiddenAuthors:"
              " input.authors, canModerate: true};"
              "\nconst out = applyIssueModeration(JSON.stringify(input.record),"
              " moderation);"
              "\nprocess.stdout.write(JSON.stringify({record: JSON.parse(out),"
              " originals: moderation.originals || {}}));")
    for hidden, authors in ((["c2"], ["troll"]), (["body"], []), ([], [])):
        result = subprocess.run(
            [node, "-e", script], input=json.dumps(
                {"record": RECORD, "hidden": hidden, "authors": authors}),
            capture_output=True, text=True, check=True)
        browser = json.loads(result.stdout)
        server = mod.apply_to_record(RECORD, hidden, authors)
        assert browser["record"] == server
        if hidden == ["c2"]:
            assert browser["originals"]["c1"] == "spam"
            assert browser["originals"]["c2"] == "fine, edited"


def test_the_dashboard_applies_moderation_before_it_renders_an_issue():
    content = (ROOT / "public/dashboard/js/06-repo-content.js").read_text(
        "utf-8")
    apply_at = content.index("applyIssueModeration(blobText(blob), moderation)")
    assert content.index("fetchIssueModeration(repo, number)") < apply_at
    assert apply_at < content.index(
        "decorateIssueModeration(container, repo, number, parsed)")
    assert "if (moderation?.canModerate) parsed.issueMutationAuthorized = true;" \
        in content
    account = (ROOT / "public/dashboard/js/04-account.js").read_text("utf-8")
    assert "data-org-moderation-section hidden" in account
    assert "void loadOrgModeration(root, name);" in account
    io = (ROOT / "public/dashboard/js/03-issues-profile-io.js").read_text(
        "utf-8")
    assert 'result?.pending === "review"' in io
    compose = (ROOT / "public/dashboard/js/07-repo-compose-branch.js") \
        .read_text("utf-8")
    assert compose.count('result?.pending === "review"') == 2
    bundle = (SRC / "dashboard_bundle.py").read_text("utf-8")
    assert '"09-moderation.js",' in bundle


def test_the_signup_page_follows_the_policy():
    page = (ROOT / "public/signup.html").read_text("utf-8")
    script = (ROOT / "public/signup.js").read_text("utf-8")
    for marker in ("data-signup-suggest", "data-signup-handle-note",
                   "data-signup-email-optional", "data-signup-email-note",
                   'id="recovery-view"', 'id="signup-closed"'):
        assert marker in page
    assert 'api("/api/accounts/handle-suggestion")' in script
    assert 'if (handleMode === "require") nameInput.readOnly = true;' in script
    assert "if (body.recoveryCode) {" in script
    forgot = (ROOT / "public/forgot-password.js").read_text("utf-8")
    assert 'fetch("/api/accounts/recover"' in forgot


class ConsoleRequest:
    def __init__(self, method="GET", form=None):
        from urllib.parse import urlencode
        self.method = method
        self.url = "https://forum.test/secret?view=accounts-moderation"
        self.form = urlencode(form or {}, doseq=True)
        self.headers = {}

    async def text(self):
        return self.form


class Page:
    def __init__(self, body, status=200, headers=None):
        self.body, self.status, self.headers = body, status, headers or {}


def _console(world, mod, monkeypatch):
    import html
    import admin_shell
    console = fresh("instance_console")
    code = {"enabled": False}

    async def nav_badges(_env):
        return {}

    async def code_mode(env):
        return code["enabled"]

    async def set_code_mode(env, enabled):
        code["enabled"] = bool(enabled)
        return code["enabled"]

    monkeypatch.setattr(admin_shell, "nav_badges", nav_badges)
    runtime = world.runtime()
    runtime.update({
        "_admin_shell": admin_shell, "_html_escape": html.escape,
        "_admin_href": lambda _query, **params: "?" + "&amp;".join(
            "%s=%s" % item for item in params.items()),
        "Response": Page,
        "_admin_csrf_ok": lambda _env, form: form.get("csrf") == ["ok"],
        "instance_branded": lambda _env: True,
        "_admin_signup_referral_code_mode": code_mode,
        "_set_admin_signup_referral_code_mode": set_code_mode,
    })
    console.moderation = mod
    console._bind_runtime(runtime)
    env = SimpleNamespace(ADMIN_PATH="secret", INSTANCE_NAME="example")

    def call(form=None):
        request = ConsoleRequest("POST", dict({"csrf": "ok"}, **form)) if form \
            else ConsoleRequest()
        return run(console.admin_handler(
            env, request, {"view": ["accounts-moderation"]}, "<csrf>", "",
            "root"))
    return call, code


def test_the_admin_console_saves_the_account_policy(world, mod, monkeypatch):
    call, code = _console(world, mod, monkeypatch)
    page = call().body
    assert 'name="signup"' in page and 'value="optional"' in page
    assert 'name="handles"' in page and "Random names only" in page
    answer = call({"action": "save_accounts", "signup": "invite",
                   "handles": "suggest", "accountRequired": "1",
                   "firstPostApproval": "1", "newAccountDays": "7",
                   "newThreadsPerDay": "3", "newCommentsPerDay": "20"})
    assert answer.status == 303
    assert answer.headers["location"].endswith("notice=accounts_saved")
    setting = run(mod.policy(None))
    assert setting["signup"] == "invite" and setting["accountRequired"]
    assert setting["approvalSince"] == world.now
    assert code["enabled"] is True and setting["inviteSetCode"]
    call({"action": "save_accounts", "signup": "optional",
          "handles": "suggest"})
    assert code["enabled"] is False
    assert not run(mod.policy(None))["firstPostApproval"]
    assert ("root", "admin.console_save_accounts") in world.audit
    forged = call({"action": "save_accounts", "signup": "closed",
                   "csrf": "bad"})
    assert forged.headers["location"].endswith("notice=csrf")


def test_the_admin_console_manages_instance_moderators_and_blocks(
        world, mod, monkeypatch):
    call, _code = _console(world, mod, monkeypatch)
    world.add_account("root")
    world.add_account("troll")
    world.add_account("watch")
    call({"action": "add_moderator", "name": "watch"})
    blocked = call({"action": "block", "name": "troll", "reason": "spam",
                    "hideContent": "1"})
    assert blocked.headers["location"].endswith("notice=moderation_done")
    assert run(mod.can_moderate_repo(None, "watch", "any", "repo"))
    assert run(mod.account_blocked(None, "troll", "any", "repo"))
    page = call().body
    assert "watch" in page and "troll" in page and "spam" in page
    refused = call({"action": "block", "name": "root"})
    assert refused.headers["location"].endswith("notice=moderation_refused")
    call({"action": "unblock", "name": "troll"})
    assert not run(mod.account_blocked(None, "troll", "any", "repo"))


def test_the_console_view_is_in_the_people_menu():
    spec = importlib.util.spec_from_file_location("admin_shell_src", SRC / "admin_shell.py")
    admin_shell = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(admin_shell)
    keys = [item[0] for group, items in admin_shell.NAV for item in items
            if group == "People"]
    assert "accounts-moderation" in keys
