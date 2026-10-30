#!/usr/bin/env python3
"""Website issue-submission inbox contracts."""

import ast
import asyncio
import base64
import json
import re
from pathlib import Path
from urllib.parse import quote

from worker_test_helpers import json_from_request_double


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
) + "\n" + CATALOG.read_text(encoding="utf-8")

FUNCS = {
    "issues_handler",
    "_accept_issue_submission",
    "_queue_issue_actions",
    "_queued_issue_inbox_event",
    "_issue_event_signature_stored",
    "_bind_session_author",
    "_inbox_author_over_quota",
    "_best_effort_inbox_side_effect",
    "_clean_issue_attachment_data",
    "method_name",
    "clean_string",
    "repo_web_href",
}


def _load_functions(extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {node.name for node in selected}
    assert found == FUNCS, "missing functions: %s" % sorted(FUNCS - found)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Request:
    method = "POST"
    url = "https://forkmesh.test/api/repo/source/forkmesh/issues"

    def __init__(self, body):
        self._body = body

    async def json(self):
        return self._body


SIGNER_KEY = "MXcj4dF_" + "A" * 35


def _issue_handler_namespace(inserted, wakes, queued_rows=(), session="",
                             stored_events=()):

    class _Date:
        @staticmethod
        def now():
            return 1_000_000

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value)

    # The handler resolves its storage key through _inbox_repo_key so an
    # organization alias can never be keyed to a queue no drain reads; this
    # wire-contract test uses a plain node URL, where it is just the hash.
    async def inbox_repo_key(_env, _request, owner, repo):
        return "bi:" + owner + "/" + repo

    class OrgAliasUnresolved(Exception):
        pass

    async def verify_issue_event(_number, _event, repo_keys=()):
        assert repo_keys == ("source/forkmesh",)
        return True

    async def reserve_number(_env, repo_bi, owner, repo, kind="issue"):
        assert (repo_bi, owner, repo, kind) == (
            "bi:source/forkmesh", "source", "forkmesh", "issue")
        return 622

    async def encrypt_row(_env, obj):
        return dict(obj)

    async def d1_first(_env, sql, *_args):
        if "SELECT COUNT(*) AS c FROM issue_inbox" in sql:
            return {"c": 0}
        if "WHERE repo_bi=? AND submitter_bi=?" in sql:
            return {"c": 0}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_all(_env, sql, *_args):
        if "FROM issue_inbox WHERE repo_bi=? AND submitter_bi=?" in sql:
            return [{"data": row} for row in queued_rows]
        raise AssertionError("unexpected d1_all: " + sql)

    async def decrypt_row(_env, data):
        return data

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO issue_inbox"):
            inserted.append(args)
            return None
        raise AssertionError("unexpected d1_run: " + sql)

    async def record_contributor(_env, _author, _kind, _display_name=""):
        return None

    async def failing_side_effect(*_args, **_kwargs):
        raise RuntimeError("notification backend unavailable")

    async def account_for_pubkey(_env, pubkey):
        return "alice" if pubkey == SIGNER_KEY else ""

    async def notify_source(_env, _owner, _repo, _topic):
        wakes.append("source")
        return None

    async def schedule_mirrors(_env, _owner, _repo, _topic):
        wakes.append("mirrors")
        return True

    async def feature_on(_env, _owner, _repo, _feature):
        return None

    async def session_record(_env, _request, _data=None,
                             allow_unverified=False):
        assert allow_unverified is True
        return ("bi:" + session, {"name": session}) if session else ("", None)

    async def repo_gateway_json(_env, owner, repo, action_query):
        assert (owner, repo) == ("source", "forkmesh")
        assert action_query.startswith("blobs?path=")
        return {"blobs": {".forkmesh/issues/7/issue.json": json.dumps(
            {"number": 7, "events": list(stored_events)})}}

    ns = _load_functions({
        "_forkbot_repo_gateway_json": repo_gateway_json,
        "_forkbot_issue_json_candidates": lambda number: [
            ".forkmesh/issues/%d/issue.json" % number],
        "_forkbot_blob_text": lambda blob: blob or "",
        "json": json,
        "ISSUE_REPLAY_CHECKED_EVENT_TYPES": frozenset(
            {"comment", "edit", "vote"}),
        "_repo_feature_off": feature_on,
        "Date": _Date,
        "json_response": json_response,
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "_inbox_repo_key": inbox_repo_key,
        "_OrgAliasUnresolved": OrgAliasUnresolved,
        "verify_issue_event": verify_issue_event,
        "_event_signing_repo_keys": lambda _request, owner, repo: (
            (owner + "/" + repo).lower(),),
        "_forkbot_next_issue_number": reserve_number,
        "encrypt_row": encrypt_row,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "d1_run": d1_run,
        "_record_contributor": record_contributor,
        "_record_public_contributor": (
            lambda env, _owner, _repo, *args: record_contributor(env, *args)),
        "valid_node_pubkey": lambda value: value == SIGNER_KEY,
        "_git_push_account_for_pubkey": account_for_pubkey,
        "_account_session_record": session_record,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", value or "")),
        "notify_pending_inbox": failing_side_effect,
        "notify_mentions": failing_side_effect,
        "notify_issue_assignees": failing_side_effect,
        "notify_account_event": failing_side_effect,
        "notify_subscribers": failing_side_effect,
        "subscribe_thread": failing_side_effect,
        "notify_repo_host": notify_source,
        "notify_repo_mirrors": notify_source,
        "schedule_repo_mirror_wake": schedule_mirrors,
        # Fediverse publish is best-effort exactly like the notification
        # fan-out: a dead ActivityPub path must never 500 an accepted issue.
        "_ap_publish_repo_event": failing_side_effect,
        "quote": quote,
        "base64": base64,
        "MAX_ISSUE_BYTES": 64 * 1024,
        "MAX_PENDING_ISSUES": 500,
        "MAX_PENDING_PER_AUTHOR": 50,
        "MAX_NODE_NAME": 63,
        "MAX_ISSUE_ATTACHMENTS": 6,
        "MAX_ISSUE_ATTACHMENT_BYTES": 2 * 1024 * 1024,
        "ISSUE_ATTACHMENT_NAME_RE": re.compile(
            r"^[0-9a-f]{8}\.(png|jpg|jpeg|gif|webp|bmp|svg|bin)$"),
    })
    return ns


def _open_body(sig="sig-a"):
    return {
        "number": 0,
        "titleIfNew": "Mirrors should accept reports",
        "event": {
            "type": "open",
            "author": "web-author",
            "authorName": "mirror-node",
            "title": "Mirrors should accept reports",
            "body": "source is offline, mirror is online",
            "sig": sig,
        },
        "meta": {"assignees": ["source"]},
    }


def test_issue_post_succeeds_when_notification_fanout_fails():
    inserted = []
    wakes = []
    ns = _issue_handler_namespace(inserted, wakes)
    body = _open_body()
    response = asyncio.run(
        ns["issues_handler"](object(), _Request(body), "source", "forkmesh"))

    assert response == {
        "status": 201, "data": {"ok": True, "number": 622}}
    assert len(inserted) == 1
    assert inserted[0][0] == "bi:source/forkmesh"
    assert inserted[0][1]["number"] == 622
    assert inserted[0][1]["signedNumber"] == 0
    # Acceptance keeps the single owner/source wake inline, but hands the
    # potentially slow multi-mirror fan-out to the durable doorbell.
    assert wakes == ["source", "mirrors"]


def test_replayed_signed_issue_answers_with_the_already_queued_number():
    # Issue #698: the browser retries the SAME signed event when an attempt's
    # response was lost. The relay must hand back the number it already
    # queued instead of reserving another and queuing the event twice.
    inserted = []
    wakes = []
    queued = [
        {"number": 610, "event": {"sig": "other-issue"}},
        {"number": 622, "event": {"sig": "sig-a"}},
    ]
    ns = _issue_handler_namespace(inserted, wakes, queued)

    async def must_not_reserve(*_args, **_kwargs):
        raise AssertionError("a replay must not reserve a second number")

    ns["_forkbot_next_issue_number"] = must_not_reserve
    response = asyncio.run(ns["issues_handler"](
        object(), _Request(_open_body("sig-a")), "source", "forkmesh"))

    assert response == {"status": 201, "data": {"ok": True, "number": 622}}
    assert inserted == []
    assert wakes == []


def test_a_new_signature_from_the_same_author_is_still_queued():
    inserted = []
    wakes = []
    ns = _issue_handler_namespace(
        inserted, wakes, [{"number": 610, "event": {"sig": "sig-a"}}])
    response = asyncio.run(ns["issues_handler"](
        object(), _Request(_open_body("sig-b")), "source", "forkmesh"))

    assert response == {"status": 201, "data": {"ok": True, "number": 622}}
    assert len(inserted) == 1


def test_issue_signed_without_a_name_is_queued_under_the_signer_username():
    inserted = []
    ns = _issue_handler_namespace(inserted, [])
    body = _open_body()
    body["event"]["author"] = SIGNER_KEY
    body["event"].pop("authorName")
    response = asyncio.run(
        ns["issues_handler"](object(), _Request(body), "source", "forkmesh"))

    assert response["status"] == 201
    assert inserted[0][1]["event"]["authorName"] == "alice"


def test_a_spoofed_name_is_replaced_by_the_account_the_key_belongs_to():
    inserted = []
    ns = _issue_handler_namespace(inserted, [])
    body = _open_body()
    body["event"]["author"] = SIGNER_KEY
    response = asyncio.run(
        ns["issues_handler"](object(), _Request(body), "source", "forkmesh"))

    assert response["status"] == 201
    assert inserted[0][1]["event"]["authorName"] == "alice"
    assert inserted[0][1]["event"]["authorVerified"] is True


def test_the_signed_in_account_names_a_web_key_comment():
    inserted = []
    ns = _issue_handler_namespace(inserted, [], session="carol")
    body = {"number": 7, "event": {
        "type": "comment", "author": "web-author", "authorName": "alice",
        "body": "/assign", "sig": "sig-c"}}
    response = asyncio.run(
        ns["issues_handler"](object(), _Request(body), "source", "forkmesh"))

    assert response == {"status": 201, "data": {"ok": True, "number": 7}}
    assert inserted[0][1]["event"]["authorName"] == "carol"
    assert inserted[0][1]["event"]["authorVerified"] is True


def _stored_edit(sig="sig-edit", target="comment-1"):
    return {"type": "edit", "id": "edit-1", "author": "web-author",
            "target": target, "body": "old text", "sig": sig}


def test_a_replayed_edit_with_a_new_id_and_target_is_not_queued():
    inserted = []
    ns = _issue_handler_namespace(
        inserted, [], stored_events=[_stored_edit()])
    replay = dict(_stored_edit(), id="edit-again", target="comment-2")
    response = asyncio.run(ns["issues_handler"](
        object(), _Request({"number": 7, "event": replay}),
        "source", "forkmesh"))

    assert response == {"status": 200, "data": {
        "ok": True, "number": 7, "alreadyApplied": True}}
    assert inserted == []


def test_a_replayed_comment_or_vote_is_not_queued_twice():
    for event in (
            {"type": "comment", "id": "c-1", "author": "web-author",
             "body": "hi", "sig": "sig-comment"},
            {"type": "vote", "id": "v-1", "author": "web-author",
             "sig": "sig-vote"}):
        inserted = []
        ns = _issue_handler_namespace(inserted, [], stored_events=[event])
        replay = dict(event, id="fresh-id")
        response = asyncio.run(ns["issues_handler"](
            object(), _Request({"number": 7, "event": replay}),
            "source", "forkmesh"))
        assert response["data"].get("alreadyApplied") is True
        assert inserted == []


def test_a_comment_with_a_signature_not_in_the_issue_is_queued():
    inserted = []
    ns = _issue_handler_namespace(
        inserted, [], stored_events=[_stored_edit()])
    body = {"number": 7, "event": {
        "type": "comment", "author": "web-author", "body": "new",
        "sig": "sig-new"}}
    response = asyncio.run(
        ns["issues_handler"](object(), _Request(body), "source", "forkmesh"))

    assert response == {"status": 201, "data": {"ok": True, "number": 7}}
    assert len(inserted) == 1


def test_a_key_bound_to_another_account_than_the_session_is_refused():
    inserted = []
    ns = _issue_handler_namespace(inserted, [], session="carol")
    body = {"number": 7, "event": {
        "type": "comment", "author": SIGNER_KEY, "authorName": "carol",
        "body": "hi", "sig": "sig-d"}}
    response = asyncio.run(
        ns["issues_handler"](object(), _Request(body), "source", "forkmesh"))

    assert response == {"status": 403, "data": {"error": "author_mismatch"}}
    assert inserted == []


def test_an_anonymous_comment_is_never_given_a_username():
    inserted = []
    ns = _issue_handler_namespace(inserted, [])
    body = {"number": 7, "event": {
        "type": "comment", "author": "web-author", "authorName": "alice",
        "authorVerified": True, "body": "/assign", "sig": "sig-e"}}
    response = asyncio.run(
        ns["issues_handler"](object(), _Request(body), "source", "forkmesh"))

    assert response["status"] == 201
    assert inserted[0][1]["event"]["authorName"] == ""
    assert inserted[0][1]["event"]["authorVerified"] is False


def test_an_actions_job_submission_keeps_the_bot_name_and_is_verified():
    inserted = []
    ns = _issue_handler_namespace(inserted, [], session="carol")
    body = {"number": 7, "event": {
        "type": "comment", "author": "bot-key", "authorName": "acme-actions",
        "body": "Thanks", "sig": "sig-f"}}
    response = asyncio.run(ns["_accept_issue_submission"](
        object(), _Request(body), "source", "forkmesh", "bi:source/forkmesh",
        body, ("source/forkmesh",), actions_job=True))

    assert response["status"] == 201
    assert inserted[0][1]["event"]["authorName"] == "acme-actions"
    assert inserted[0][1]["event"]["authorVerified"] is True


def test_discussion_handler_resolves_an_unnamed_signer_before_notifying():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    handler = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "discussions_handler")
    source = ast.get_source_segment(ENTRY_TEXT, handler)
    assert source is not None
    assert source.index(
        "await _bind_session_author(env, request, data, event)") < source.index(
        'actor = clean_string(event.get("authorName", "")')


def _claim_namespace(inserted, notified, claims, session=""):
    ns = _issue_handler_namespace(inserted, [], session=session)

    async def record_assignees(_env, _owner, _repo, assignees, actor,
                               _title, number=0):
        notified.append((sorted(assignees), actor, number))

    async def self_claim(_env, _data, _request, event):
        claims.append(event.get("assignee"))
        return True, None

    ns["notify_issue_assignees"] = record_assignees
    ns["_authorize_issue_self_claim"] = self_claim
    return ns


def _claim_body(assignee, author_name, sig):
    return {"number": 7, "event": {
        "type": "claim", "author": "bot-key", "authorName": author_name,
        "assignee": assignee, "claimState": "assigned", "sig": sig}}


def test_an_actions_job_claim_assigns_and_notifies_the_assignee():
    inserted, notified, claims = [], [], []
    ns = _claim_namespace(inserted, notified, claims)
    body = _claim_body("Bob", "acme-actions", "sig-g")
    response = asyncio.run(ns["_accept_issue_submission"](
        object(), _Request(body), "source", "forkmesh", "bi:source/forkmesh",
        body, ("source/forkmesh",), actions_job=True))

    assert response == {"status": 201, "data": {"ok": True, "number": 7}}
    assert claims == []
    assert inserted[0][1]["event"]["assignee"] == "Bob"
    assert notified == [(["bob"], "acme-actions", 7)]


def test_a_self_claim_still_needs_the_session_and_notifies_nobody():
    inserted, notified, claims = [], [], []
    ns = _claim_namespace(inserted, notified, claims, session="alice")
    response = asyncio.run(ns["issues_handler"](
        object(), _Request(_claim_body("alice", "alice", "sig-h")),
        "source", "forkmesh"))

    assert response["status"] == 201
    assert claims == ["alice"]
    assert notified == [([], "alice", 7)]
