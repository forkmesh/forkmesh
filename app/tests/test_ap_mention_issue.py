#!/usr/bin/env python3
"""Fediverse repo-mention -> manual World review flow.

Mentioning a repo actor on Mastodon ("@forkmesh.forkmesh@forkmesh.com the
save button is broken") enters a verified-public activity feed. It does not
run AI, create an issue, fetch attachments, or publish a reply during inbox
handling. These pin the contracts:

  * only Mention tags pointing at OUR repo actors open the gate (and the
    inbox lets such a Create through to signature verification);
  * legacy auto-created mention ids remain deduplicated;
  * privacy/settings gates (private repo, federate/acceptComments off) drop
    before activity recording;
  * the consented post-materialization reply Note is addressed to the author,
    deduplicated, and carries the confirmed issue thread context.

These load the real handlers out of src/entry.py (same pattern as
test_ap_inbox_load.py) with the real activitypub.py protocol module.

Run: python3 -m pytest cloudflare_worker/tests/test_ap_mention_issue.py
"""

import ast
import asyncio
import importlib.util
import json
import re
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "request_bodies.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)

spec = importlib.util.spec_from_file_location(
    "activitypub", ROOT / "src" / "activitypub.py")
ap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ap)

ORIGIN = "https://forkmesh.com"
REMOTE = {
    "actor_id": "https://mastodon.social/users/alice",
    "inbox": "https://mastodon.social/users/alice/inbox",
    "shared_inbox": "https://mastodon.social/inbox",
    "handle": "alice@mastodon.social",
    "display_name": "Alice",
    "url": "https://mastodon.social/@alice",
}


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {n.name for n in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def test_public_feed_requires_explicit_activitystreams_public_audience():
    is_public = _load(
        "_ap_activity_is_public", extra_globals={"ap": ap}
    )["_ap_activity_is_public"]
    private = {
        "type": "Create",
        "to": [ORIGIN + "/ap/repos/forkmesh/forkmesh"],
        "object": {"type": "Note", "cc": [REMOTE["actor_id"]]},
    }
    assert is_public(private) is False
    public_activity = dict(private, cc=[ap.AS_PUBLIC])
    assert is_public(public_activity) is True
    public_object = dict(
        private,
        object={"type": "Note", "to": [{"id": ap.AS_PUBLIC}]},
    )
    assert is_public(public_object) is True


async def _passthrough_alias(env, owner, repo):
    # Non-org repos resolve to themselves; org-alias resolution is pinned in
    # test_orgs_teams.py.
    return owner


class FakeResponse:
    def __init__(self, data, status=200):
        self.data = data
        self.status = status


def _json_response(data, status=200, **kwargs):
    return FakeResponse(data, status=status)


def _note(content="@forkmesh.forkmesh the save button crashes the app",
          mentions=("https://forkmesh.com/ap/repos/forkmesh/forkmesh",),
          images=()):
    return {
        "id": "https://mastodon.social/notes/1",
        "inReplyTo": "",
        "content": content,
        "attributedTo": REMOTE["actor_id"],
        "url": "https://mastodon.social/@alice/1",
        "published": "2026-07-17T00:00:00Z",
        "mentions": list(mentions),
        "images": list(images),
    }


def _mention_env(log, *, is_private=0, has_repo=True, seen=False,
                 ai_result="__unset__", images=(), enqueue_ok=True,
                 issue_number=7, settings=None):
    async def blind_index(env, value):
        return "bi:" + value

    async def d1_first(env, sql, *args):
        log.append(("d1_first", sql, args))
        if "FROM repositories" in sql:
            return {"is_private": is_private} if has_repo else None
        if "FROM ap_mentions" in sql:
            return {"ts": 1} if seen else None
        return None

    async def d1_run(env, sql, *args):
        log.append(("d1_run", sql, args))

    async def _repo_is_private(env, owner, repo):
        return not has_repo or bool(is_private)

    async def _ap_repo_settings_get(env, owner, repo):
        return dict(settings or {"federate": True, "broadcastEvents": True,
                                 "acceptComments": True})

    async def _ap_mention_ai_intent(env, text):
        log.append(("ai", text))
        return None if ai_result == "__unset__" else ai_result

    async def _ap_fetch_note_images(env, atts):
        log.append(("fetch_images", tuple(a["url"] for a in atts)))
        return list(images)

    async def _forkbot_enqueue_issue(env, owner, repo, title, body, requester,
                                     source="forkbot", labels=None,
                                     attachments=None):
        log.append(("enqueue", owner, repo, title, body, requester, source,
                    labels, attachments))
        if not enqueue_ok:
            return False, "inbox_full"
        return True, {"issueNumber": issue_number, "titleIfNew": title}

    async def _ap_send_mention_reply(env, request, owner, repo, remote, note,
                                     message, number):
        log.append(("reply", owner, repo, message, number))

    async def _best_effort_inbox_side_effect(coro):
        await coro

    class MentionAPI:
        @staticmethod
        async def record_verified(runtime, **kwargs):
            log.append(("record", kwargs))
            return "01" * 16

    class WorldRuntime:
        def __init__(self, env, request):
            self.env = env
            self.request = request

    return _load(
        "_ap_handle_repo_mention", "_ap_note_repo_mention",
        "_forkbot_command_hints_issue", "_forkbot_fallback_issue_fields",
        "_forkbot_issue_title",
        extra_globals={
            "re": re,
            "ap": ap,
            "json_response": _json_response,
            "blind_index": blind_index,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "_repo_is_private": _repo_is_private,
            "_ap_org_alias_owner": _passthrough_alias,
            "_ap_repo_settings_get": _ap_repo_settings_get,
            "_ap_mention_ai_intent": _ap_mention_ai_intent,
            "_ap_fetch_note_images": _ap_fetch_note_images,
            "_forkbot_enqueue_issue": _forkbot_enqueue_issue,
            "_ap_send_mention_reply": _ap_send_mention_reply,
            "_best_effort_inbox_side_effect": _best_effort_inbox_side_effect,
            "fediverse_mentions_api": MentionAPI,
            "_WorldCommunityRuntime": WorldRuntime,
            "_ap_origin": lambda env, request=None: ORIGIN,
            "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
            "clean_string": lambda value, cap: str(value or "")[:cap],
            "FORKBOT_MAX_COMMAND": 4000,
            "MAX_ISSUE_BYTES": 64 * 1024,
            "Date": SimpleNamespace(now=lambda: 1750000000000),
        })


# --- Mention-tag gate ---------------------------------------------------------

def test_note_repo_mention_maps_only_local_repo_actors():
    ns = _mention_env([])
    fn = ns["_ap_note_repo_mention"]
    assert fn(ORIGIN, _note()) == ("forkmesh", "forkmesh")
    # User actors, foreign servers, and mention-free notes don't open the gate.
    assert fn(ORIGIN, _note(mentions=[ORIGIN + "/ap/users/alice"])) is None
    assert fn(ORIGIN, _note(
        mentions=["https://other.example/ap/repos/a/b"])) is None
    assert fn(ORIGIN, _note(mentions=[])) is None
    # Actor ids are lowercase-normalized like every other AP lookup.
    assert fn(ORIGIN, _note(
        mentions=[ORIGIN + "/ap/repos/ForkMesh/ForkMesh"])) == \
        ("forkmesh", "forkmesh")


# --- Verified activity recording ---------------------------------------------

def test_verified_mention_records_for_manual_review_only():
    log = []
    ns = _mention_env(log)
    resp = _run(ns["_ap_handle_repo_mention"](
        None, None, _note(), REMOTE, "forkmesh", "forkmesh"))
    assert resp.status == 202
    recorded = next(entry for entry in log if entry[0] == "record")[1]
    assert recorded["signature_verified"] is True
    assert recorded["public_repository"] is True
    assert recorded["kind"] == "mention"
    assert recorded["actor_owner"] == "forkmesh"
    assert recorded["data_owner"] == "forkmesh"
    assert "save button crashes" in recorded["note"]["content"]
    assert not any(
        entry[0] in ("ai", "fetch_images", "enqueue", "reply")
        for entry in log
    )


def test_mention_strips_addressing_handle_before_public_review():
    log = []
    ns = _mention_env(log)
    _run(ns["_ap_handle_repo_mention"](
        None, None,
        _note(content="<p>@forkmesh.forkmesh@forkmesh.com "
                      "love the project!</p>"),
        REMOTE, "forkmesh", "forkmesh"))
    content = next(e for e in log if e[0] == "record")[1]["note"]["content"]
    assert "forkmesh.forkmesh" not in content
    assert "love the project!" in content


def test_inbox_does_not_fetch_attachments_or_auto_create():
    log = []
    ns = _mention_env(log)
    _run(ns["_ap_handle_repo_mention"](
        None, None,
        _note(images=[{"url": "https://files.example/1.png",
                       "mediaType": "image/png", "name": "screenshot"}]),
        REMOTE, "forkmesh", "forkmesh"))
    assert any(e[0] == "record" for e in log)
    assert not any(
        e[0] in ("fetch_images", "enqueue", "reply", "ai") for e in log)


def test_non_issue_shaped_public_feedback_is_still_reviewable_not_filed():
    log = []
    ns = _mention_env(log)
    resp = _run(ns["_ap_handle_repo_mention"](
        None, None, _note(content="@forkmesh.forkmesh great work!"),
        REMOTE, "forkmesh", "forkmesh"))
    assert resp.status == 202
    assert any(e[0] == "record" for e in log)
    assert not any(e[0] in ("ai", "enqueue", "reply") for e in log)


# --- Gates --------------------------------------------------------------------

def test_legacy_seen_note_drops_before_new_review_record():
    log = []
    ns = _mention_env(log, seen=True)
    resp = _run(ns["_ap_handle_repo_mention"](
        None, None, _note(), REMOTE, "forkmesh", "forkmesh"))
    assert resp.status == 202
    assert not any(
        e[0] in ("record", "ai", "enqueue", "reply") for e in log)


def test_private_missing_or_defederated_repo_drops_before_recording():
    for kwargs in ({"is_private": 1}, {"has_repo": False},
                   {"settings": {"federate": False, "acceptComments": True}},
                   {"settings": {"federate": True, "acceptComments": False}}):
        log = []
        ns = _mention_env(log, **kwargs)
        resp = _run(ns["_ap_handle_repo_mention"](
            None, None, _note(), REMOTE, "forkmesh", "forkmesh"))
        assert resp.status == 202, kwargs
        assert not any(
            e[0] in ("record", "ai", "enqueue", "reply") for e in log), kwargs
        assert not any(
            e[0] == "d1_run" and "ap_mentions" in e[1] for e in log), kwargs


def test_inbox_capacity_is_not_consulted_until_manual_create():
    log = []
    ns = _mention_env(log, enqueue_ok=False, ai_result={
        "intent": "create_issue", "title": "T", "body": "B"})
    resp = _run(ns["_ap_handle_repo_mention"](
        None, None, _note(), REMOTE, "forkmesh", "forkmesh"))
    assert resp.status == 202
    assert any(e[0] == "record" for e in log)
    assert not any(e[0] in ("enqueue", "reply", "ai") for e in log)


# --- The reply Note -----------------------------------------------------------

def _reply_env(log):
    async def _ap_local_actor(env, kind, handle, create=False):
        log.append(("local_actor", kind, handle, create))
        return {"actorBi": "bi:actor", "privkey": "PRIV"}

    async def _ap_actor_bi(env, kind, handle):
        return "bi:actor"

    async def blind_index(env, value):
        return "bi:" + value

    async def encrypt_row(env, rec):
        return json.dumps(rec)

    async def d1_run(env, sql, *args):
        log.append(("d1_run", sql, args))

    async def _ap_drain_outbox(env, limit):
        log.append(("drain", limit))

    return _load("_ap_send_mention_reply", extra_globals={
        "json": json,
        "re": re,
        "ap": ap,
        "_ap_origin": lambda env, request=None: ORIGIN,
        "_ap_actor_url": lambda origin, kind, handle:
            origin + "/ap/repos/" + handle.replace(".", "/", 1),
        "_ap_local_actor": _ap_local_actor,
        "_ap_actor_bi": _ap_actor_bi,
        "_ap_uuid": lambda: "ab" * 16,
        "AP_ACTOR_REPO": "repo",
        "blind_index": blind_index,
        "encrypt_row": encrypt_row,
        "d1_run": d1_run,
        "_ap_drain_outbox": _ap_drain_outbox,
        "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
        "Date": SimpleNamespace(now=lambda: 1750000000000),
    })


def test_reply_note_addresses_author_and_carries_issue_context():
    log = []
    ns = _reply_env(log)
    _run(ns["_ap_send_mention_reply"](
        None, None, "forkmesh", "forkmesh", REMOTE, _note(),
        "The owner node confirmed issue #7", 7))
    inserts = [e for e in log if e[0] == "d1_run"]
    object_insert = next(e for e in inserts if "ap_objects" in e[1])
    rec = json.loads(object_insert[2][3])
    note = rec["note"]
    assert note["inReplyTo"] == "https://mastodon.social/notes/1"
    assert note["to"] == [REMOTE["actor_id"]]
    assert ap.AS_PUBLIC in note["cc"]
    assert note["tag"] == [{"type": "Mention", "href": REMOTE["actor_id"],
                            "name": "@alice@mastodon.social"}]
    assert "confirmed issue #7" in note["content"]
    # Replies to our reply thread into the new issue as federated comments.
    assert rec["context"] == {"owner": "forkmesh", "repo": "forkmesh",
                              "kind": "issue", "ref": "7",
                              "key": "forkmesh/forkmesh#issue#7"}
    outbox_insert = next(e for e in inserts if "ap_outbox" in e[1])
    assert outbox_insert[2][0] == REMOTE["inbox"]
    delivery = json.loads(json.loads(outbox_insert[2][1])["body"])
    assert delivery["type"] == "Create"
    assert delivery["object"]["id"] == ORIGIN + "/ap/o/" + "ab" * 16
    assert ("drain", 1) in log


# --- Inbox gate ---------------------------------------------------------------

def test_inbox_lets_repo_mention_create_through_to_verification():
    # Borrow test_ap_inbox_load's environment shape: no Signature header, so
    # reaching 401 signature_required proves the mention passed the pre-drop.
    class FakeRequest:
        def __init__(self, body):
            self.method = "POST"
            self.url = ORIGIN + "/ap/inbox"
            self._body = body
            self.headers = SimpleNamespace(get=lambda name: None)

        async def text(self):
            return self._body

    async def ensure_schema(env):
        pass

    async def _ap_enabled(env):
        return True

    async def _ap_domain_blocked(env, host):
        return False

    async def d1_first(env, sql, *args):
        return None

    ns = _load(
        "ap_inbox_handler", "bounded_request_text", "_ap_note_repo_mention",
        extra_globals={
        "json": json,
        "urlparse": __import__("urllib.parse", fromlist=["urlparse"]).urlparse,
        "method_name": lambda request: request.method,
        "json_response": _json_response,
        "ensure_schema": ensure_schema,
        "_ap_enabled": _ap_enabled,
        "_ap_origin": lambda env, request=None: ORIGIN,
        "_ap_domain_blocked": _ap_domain_blocked,
        "d1_first": d1_first,
        "ap": ap,
        "AP_MAX_INBOX_BYTES": 128 * 1024,
        "AP_DATE_SKEW_MS": 12 * 60 * 60 * 1000,
        "Date": SimpleNamespace(now=lambda: 1750000000000),
        "RequestBodyTooLarge": type("RequestBodyTooLarge", (BaseException,), {}),
    })
    activity = {
        "type": "Create",
        "actor": REMOTE["actor_id"],
        "object": {
            "id": "https://mastodon.social/notes/1",
            "type": "Note",
            "content": "@forkmesh.forkmesh the save button crashes",
            "attributedTo": REMOTE["actor_id"],
            "tag": [{"type": "Mention",
                     "href": ORIGIN + "/ap/repos/forkmesh/forkmesh"}],
        },
    }
    resp = _run(ns["ap_inbox_handler"](None, FakeRequest(json.dumps(activity))))
    assert resp.status == 401
    assert resp.data["error"] == "signature_required"
