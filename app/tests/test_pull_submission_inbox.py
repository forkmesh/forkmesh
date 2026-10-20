#!/usr/bin/env python3
"""Pull-inbox wire contracts for portable browser submissions."""

import ast
import asyncio
import time
import hashlib
from pathlib import Path

from worker_test_helpers import json_from_request_double


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
EVENTS = ROOT / "src" / "events.py"
EVENTS_TEXT = EVENTS.read_text(encoding="utf-8")


def _load_pulls_handler(extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name in ("pulls_handler", "_bind_session_author")
    ]
    assert len(selected) == 2
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["pulls_handler"]


def _load_pull_verifier(extra_globals):
    tree = ast.parse(EVENTS_TEXT, filename=str(EVENTS))
    wanted = {"verify_pull_event", "_verify_event_messages",
              "event_signing_messages", "v1_event_intake_open"}
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in wanted)
        or (isinstance(node, ast.Assign) and any(
            getattr(target, "id", "") == "EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS"
            for target in node.targets))
    ]
    assert len(selected) == len(wanted) + 1
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {"time": time}
    namespace.update(extra_globals)
    exec(compile(module, str(EVENTS), "exec"), namespace)
    return namespace["verify_pull_event"]


SIGNER_KEY = "MXcj4dF_" + "A" * 35


class _Request:
    method = "POST"
    url = "https://forkmesh.test/api/repo/alice/project/pulls"

    def __init__(self, pull=None, body=None):
        self._body = body if body is not None else {"pull": pull}

    async def json(self):
        return self._body


def _runtime(inserted, notifications, verified, receipts=None, session=""):
    if receipts is None:
        receipts = {}
    class _Date:
        @staticmethod
        def now():
            return 1_000_000

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def noop(*_args, **_kwargs):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def d1_first(_env, sql, *_args):
        if "SELECT COUNT(*) AS c FROM pull_inbox" in sql:
            return {"c": 0}
        if "FROM pull_submission_receipts" in sql:
            repo_bi, submission_bi = _args
            return receipts.get((repo_bi, submission_bi))
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT OR IGNORE INTO pull_submission_receipts"):
            repo_bi, submission_bi, data = args
            receipts.setdefault((repo_bi, submission_bi), {"data": data})
            return None
        if sql.startswith("INSERT OR IGNORE INTO pull_inbox"):
            inserted.append(args)
            return None
        if sql.startswith("INSERT INTO pull_inbox"):
            inserted.append(args)
            return None
        if sql.startswith("UPDATE pull_submission_receipts SET data"):
            data, repo_bi, submission_bi = args
            receipts[(repo_bi, submission_bi)] = {"data": data}
            return None
        raise AssertionError("unexpected d1_run: " + sql)

    async def encrypt_row(_env, value):
        return value

    async def decrypt_row(_env, value):
        return value

    async def verify_pull_event(pull, _repo_keys=()):
        verified.append(dict(pull))
        return True

    async def verify_pull_comment_event(_number, event, _repo_keys=()):
        verified.append(dict(event))
        return True

    async def account_for_pubkey(_env, pubkey):
        return "bob" if pubkey == SIGNER_KEY else ""

    async def session_record(_env, _request, _data=None,
                             allow_unverified=False):
        return ("bi:" + session, {"name": session}) if session else ("", None)

    async def reserve_number(_env, repo_bi, owner, repo, kind="issue"):
        assert (repo_bi, owner, repo, kind) == (
            "bi:alice/project", "alice", "project", "pull")
        return 95

    async def notify_mentions(
        _env, _owner, _repo, _actor, _title, body, *_args, **_kwargs
    ):
        notifications.append(body)

    return {
        "Date": _Date,
        "base64": __import__("base64"),
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "ensure_schema": noop,
        "blind_index": blind_index,
        # The handler resolves its storage key through
        # _inbox_repo_key so an organization alias can never be keyed
        # to a queue no drain reads; these wire-contract tests use a
        # plain node URL, where it is just the hash.
        "_inbox_repo_key": (
            lambda _env, _request, owner, repo:
                _async_value("bi:" + owner + "/" + repo)
        ),
        "_OrgAliasUnresolved": _OrgAliasUnresolved,
        "verify_pull_event": verify_pull_event,
        "verify_pull_comment_event": verify_pull_comment_event,
        "valid_node_pubkey": lambda value: value == SIGNER_KEY,
        "_git_push_account_for_pubkey": account_for_pubkey,
        "_account_session_record": session_record,
        "valid_node_name": lambda value: bool(value) and value.isalnum(),
        "_repo_feature_off": noop,
        "_record_pull_review": noop,
        "notify_subscribers": noop,
        "subscribe_thread": noop,
        "_event_signing_repo_keys": lambda _request, owner, repo: (
            (owner + "/" + repo).lower(),),
        "_forkbot_next_issue_number": reserve_number,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "_inbox_author_over_quota": (
            lambda *_args, **_kwargs: _async_value(False)
        ),
        "_record_contributor": noop,
        "notify_pending_inbox": noop,
        "notify_mentions": notify_mentions,
        "notify_repo_host": noop,
        "notify_repo_mirrors": noop,
        "_best_effort_inbox_side_effect": noop,
        "_ap_publish_repo_event": lambda *_args, **_kwargs: None,
        "repo_web_href": lambda owner, repo: f"/{owner}/{repo}",
        "patch_file_stats": lambda _patch: [],
        "pull_badge_png": lambda *_args, **_kwargs: b"",
        "clean_string": (
            lambda value, limit=240:
                value.strip()[:limit] if isinstance(value, str) else ""
        ),
        "MAX_ISSUE_BYTES": 64 * 1024,
        "MAX_PULL_BYTES": 100 * 1024 * 1024,
        "MAX_PENDING_PULLS": 200,
        "MAX_NODE_NAME": 63,
    }


async def _async_value(value):
    return value


class _OrgAliasUnresolved(Exception):
    """Stand-in for entry.py's narrow alias-resolution failure."""


def test_pull_post_preserves_portable_change_data_and_normalizes_legacy_body():
    inserted = []
    notifications = []
    verified = []
    handler = _load_pulls_handler(
        _runtime(inserted, notifications, verified))
    pull = {
        "title": "Portable browser pull",
        "body": "Description from the browser",
        "base": "main",
        "head": "feature",
        "patch": "diff --git a/a.txt b/a.txt\n+portable\n",
        "commits": "From abc Mon Sep 17 00:00:00 2001\nSubject: [PATCH] portable\n",
        "author": "web-author",
        "authorName": "alice",
        "ts": 123,
        "sig": "signed-change-set",
    }

    response = asyncio.run(
        handler(object(), _Request(pull), "alice", "project"))

    assert response == {
        "status": 201, "data": {"ok": True, "number": 95}}
    assert len(inserted) == 1
    stored = inserted[0][1]["pull"]
    assert stored["description"] == "Description from the browser"
    assert "body" not in stored
    assert stored["patch"] == pull["patch"]
    assert stored["commits"] == pull["commits"]
    assert stored["number"] == 95
    # The relay number is assigned only after the signed change set verifies.
    assert "number" not in verified[0]
    assert notifications == ["Description from the browser"]


def test_pull_post_rejects_an_unbounded_description_before_storage():
    inserted = []
    handler = _load_pulls_handler(_runtime(inserted, [], []))
    pull = {
        "title": "Too large",
        "description": "x" * (64 * 1024 + 1),
        "base": "main",
        "head": "feature",
        "patch": "diff",
        "commits": "",
        "author": "web-author",
        "ts": 123,
        "sig": "signature",
    }

    response = asyncio.run(
        handler(object(), _Request(pull), "alice", "project"))

    assert response == {
        "status": 413,
        "data": {"error": "description_too_large"},
    }
    assert inserted == []


def test_pull_post_replay_returns_the_original_number_without_reenqueuing():
    inserted = []
    notifications = []
    verified = []
    receipts = {}
    handler = _load_pulls_handler(
        _runtime(inserted, notifications, verified, receipts))
    pull = {
        "title": "Retry-safe pull",
        "description": "The response may have been lost.",
        "base": "main",
        "head": "feature",
        "patch": "diff --git a/a b/a\n+retry\n",
        "commits": "",
        "author": "desktop-key",
        "authorName": "desktop",
        "ts": 123,
        "sig": "stable-signature",
    }

    first = asyncio.run(handler(object(), _Request(dict(pull)), "alice", "project"))
    replay = asyncio.run(handler(object(), _Request(dict(pull)), "alice", "project"))

    assert first == {"status": 201, "data": {"ok": True, "number": 95}}
    assert replay == {
        "status": 201,
        "data": {"ok": True, "number": 95, "duplicate": True},
    }
    assert len(inserted) == 1
    assert len(notifications) == 1


def test_pull_post_repairs_a_receipt_left_before_its_inbox_write():
    inserted = []
    verified = []
    submission = "bi:pull-submission:desktop-key:stable-signature"
    receipts = {("bi:alice/project", submission): {
        "data": {"number": 96, "queued": False},
    }}
    handler = _load_pulls_handler(_runtime(inserted, [], verified, receipts))
    pull = {
        "title": "Repair interrupted submission",
        "description": "",
        "base": "main",
        "head": "feature",
        "patch": "diff --git a/a b/a\n+repair\n",
        "commits": "",
        "author": "desktop-key",
        "ts": 123,
        "sig": "stable-signature",
    }

    response = asyncio.run(handler(object(), _Request(pull), "alice", "project"))

    assert response == {
        "status": 201,
        "data": {"ok": True, "number": 96, "duplicate": True},
    }
    assert len(inserted) == 1
    assert inserted[0][1]["pull"]["number"] == 96
    assert receipts[("bi:alice/project", submission)]["data"] == {
        "number": 96, "queued": True,
    }


def test_legacy_pull_signature_cannot_authenticate_an_appended_commit_series():
    calls = []

    async def sha256_hex(value):
        return hashlib.sha256(value.encode("utf-8")).hexdigest()

    async def accept_only_legacy(_author, _signature, canonical):
        calls.append(canonical)
        # The deliberately permissive fake makes the old four-field fallback
        # succeed. The verifier must never reach it while commits are present.
        return len(calls) == 2

    verify = _load_pull_verifier({
        "sha256_hex": sha256_hex,
        "ed25519_verify": accept_only_legacy,
    })
    pull = {
        "title": "Signed patch",
        "base": "main",
        "head": "feature",
        "patch": "signed patch bytes",
        "commits": "unsigned appended mbox",
        "author": "author",
        "sig": "signature",
        "ts": 123,
    }

    assert asyncio.run(verify(pull)) is False
    assert len(calls) == 1


def test_legacy_four_field_signature_remains_valid_without_commits():
    calls = []

    async def sha256_hex(value):
        return hashlib.sha256(value.encode("utf-8")).hexdigest()

    async def accept_legacy(_author, _signature, canonical):
        calls.append(canonical)
        return len(calls) == 2

    verify = _load_pull_verifier({
        "sha256_hex": sha256_hex,
        "ed25519_verify": accept_legacy,
    })
    pull = {
        "title": "Legacy patch",
        "base": "main",
        "head": "feature",
        "patch": "legacy signed patch",
        "commits": "",
        "author": "author",
        "sig": "signature",
        "ts": 123,
    }

    assert asyncio.run(verify(pull)) is True
    assert len(calls) == 2


def _signed_pull(**overrides):
    pull = {
        "title": "Signed by key only",
        "description": "",
        "base": "main",
        "head": "feature",
        "patch": "diff",
        "commits": "",
        "author": SIGNER_KEY,
        "ts": 123,
        "sig": "signature",
    }
    pull.update(overrides)
    return pull


def test_pull_signed_without_a_name_is_queued_under_the_signer_username():
    inserted = []
    handler = _load_pulls_handler(_runtime(inserted, [], []))

    response = asyncio.run(
        handler(object(), _Request(_signed_pull()), "alice", "project"))

    assert response["status"] == 201
    assert inserted[0][1]["pull"]["authorName"] == "bob"


def test_pull_name_comes_from_the_key_not_the_client():
    inserted = []
    handler = _load_pulls_handler(_runtime(inserted, [], []))

    asyncio.run(handler(
        object(), _Request(_signed_pull(authorName="carol")),
        "alice", "project"))

    assert inserted[0][1]["pull"]["authorName"] == "bob"
    assert inserted[0][1]["pull"]["authorVerified"] is True


def test_pull_from_an_unknown_key_is_unverified_and_unnamed():
    inserted = []
    handler = _load_pulls_handler(_runtime(inserted, [], []))

    asyncio.run(handler(
        object(), _Request(_signed_pull(author="web-author", authorName="bob")),
        "alice", "project"))

    assert inserted[0][1]["pull"]["authorName"] == ""
    assert inserted[0][1]["pull"]["authorVerified"] is False


def test_pull_comment_takes_the_session_name_over_a_spoofed_one():
    inserted = []
    handler = _load_pulls_handler(
        _runtime(inserted, [], [], session="carol"))
    body = {"number": 281, "event": {
        "type": "comment", "author": "web-author", "authorName": "bob",
        "body": "lgtm", "ts": 123, "sig": "signature"}}

    response = asyncio.run(
        handler(object(), _Request(body=body), "alice", "project"))

    assert response["status"] == 201
    assert inserted[0][1]["event"]["authorName"] == "carol"
    assert inserted[0][1]["event"]["authorVerified"] is True


def test_pull_comment_with_another_accounts_key_is_refused():
    inserted = []
    handler = _load_pulls_handler(
        _runtime(inserted, [], [], session="carol"))
    body = {"number": 281, "event": {
        "type": "comment", "author": SIGNER_KEY, "authorName": "carol",
        "body": "lgtm", "ts": 123, "sig": "signature"}}

    response = asyncio.run(
        handler(object(), _Request(body=body), "alice", "project"))

    assert response == {"status": 403, "data": {"error": "author_mismatch"}}
    assert inserted == []


def test_review_signed_without_a_name_is_queued_under_the_signer_username():
    inserted = []
    handler = _load_pulls_handler(_runtime(inserted, [], []))
    body = {"number": 281, "event": {
        "type": "review", "author": SIGNER_KEY, "state": "approved",
        "body": "", "ts": 123, "sig": "signature"}}

    response = asyncio.run(
        handler(object(), _Request(body=body), "alice", "project"))

    assert response["status"] == 201
    assert inserted[0][1]["event"]["authorName"] == "bob"
