#!/usr/bin/env python3
"""Verified fediverse feedback stays manual and pending until owner confirmation."""

import asyncio
import json
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import fediverse_mentions_api as api  # noqa: E402


NOW = 1_800_000_000_000
REMOTE = {
    "actor_id": "https://social.example.org/users/alice",
    "url": "https://social.example.org/@alice",
    "handle": "alice@social.example.org",
    "display_name": "Alice",
    "inbox": "https://social.example.org/users/alice/inbox",
}
NOTE = {
    "id": "https://social.example.org/notes/42",
    "url": "https://social.example.org/@alice/42",
    "content": "The save button crashes after selecting a large project.",
    "published": "2026-07-23T12:00:00Z",
}


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT.parent / "app" / "migrations"
             / "0066_fediverse_mention_reviews.sql")
            .read_text(encoding="utf-8")
        )
        self.request_method = "GET"
        self.request_data = {}
        self.clock = NOW
        self.ids = 0
        self.actor = ""
        self.owner_node = ""
        self.enqueue_ok = True
        self.proposed_number = 17
        self.enqueued = []
        self.followups = []
        self.followup_ok = True
        self.audits = []

    def use(self, method, actor="", data=None, owner_node="", now=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        self.owner_node = owner_node
        if now is not None:
            self.clock = now
        return self

    def method(self):
        return self.request_method

    def now(self):
        return self.clock

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def public_origin(self):
        return "https://forkmesh.com"

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cacheControl": cache_control,
            "headers": dict(extra_headers or {}),
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        if not isinstance(self.request_data, dict):
            return None, "invalid_json"
        return dict(self.request_data), ""

    async def blind(self, value):
        return "bi:" + value

    async def seal(self, value):
        return json.dumps(value, separators=(",", ":"), sort_keys=True)

    async def open(self, value):
        return json.loads(value)

    async def authorize_repository_owner(self, owner, _data):
        if self.actor == owner:
            return "bi-account:" + owner, owner
        return "", ""

    async def authorize_owner_node(self, owner):
        return self.owner_node == owner

    async def enqueue_issue(
        self, owner, repo, title, body, requester, mention_id, remote_url
    ):
        self.enqueued.append({
            "owner": owner,
            "repo": repo,
            "title": title,
            "body": body,
            "requester": requester,
            "mentionId": mention_id,
            "remoteUrl": remote_url,
        })
        if not self.enqueue_ok:
            return False, "inbox_full"
        return True, {"issueNumber": self.proposed_number}

    async def publish_followup(self, mention_id, record, message, number):
        self.followups.append({
            "mentionId": mention_id,
            "record": dict(record),
            "message": message,
            "number": number,
        })
        return self.followup_ok

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.audits.append({
            "actor": actor,
            "action": action,
            "targetType": target_type,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    async def d1_all(self, sql, *args):
        return [
            dict(row) for row in self.db.execute(sql, args).fetchall()
        ]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()


def run(awaitable):
    return asyncio.run(awaitable)


def record(runtime, **updates):
    kwargs = {
        "note": dict(NOTE),
        "remote": dict(REMOTE),
        "actor_owner": "acme",
        "data_owner": "owner",
        "repo": "project",
        "kind": "mention",
        "signature_verified": True,
        "public_activity": True,
        "public_repository": True,
    }
    kwargs.update(updates)
    return run(api.record_verified(runtime, **kwargs))


def call(runtime, mention_id, action, data=None, actor="", owner_node=""):
    return run(api.handle(
        runtime.use(
            "POST", actor=actor, data=data or {}, owner_node=owner_node),
        f"/api/world/fediverse-mentions/{mention_id}/{action}",
    ))


def test_only_verified_public_notes_enter_feed_and_redelivery_deduplicates():
    runtime = FakeRuntime()
    assert record(runtime, signature_verified=False) == ""
    assert record(runtime, public_activity=False) == ""
    assert record(runtime, public_repository=False) == ""
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM world_fediverse_mentions"
    ).fetchone()[0] == 0

    mention_id = record(runtime)
    assert mention_id
    assert record(runtime) == mention_id
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM world_fediverse_mentions"
    ).fetchone()[0] == 1

    feed = run(api.handle(
        runtime.use("GET"), "/api/world/fediverse-mentions"))
    item = feed["data"]["items"][0]
    assert item["verifiedPublicActivity"] is True
    assert item["automaticIssueCreation"] is False
    assert item["state"] == "review"
    assert item["repository"] == "acme/project"
    assert item["remoteUrl"] == NOTE["url"]
    rendered = json.dumps(feed["data"]).lower()
    assert "private" not in item["excerpt"].lower()
    assert "actor_id" not in rendered
    assert "remote_id" not in rendered


def test_manual_preview_and_create_require_owner_and_explicit_confirmation():
    runtime = FakeRuntime()
    mention_id = record(runtime)
    denied = call(runtime, mention_id, "preview", actor="mallory")
    assert denied["status"] == 403
    denied_create = call(
        runtime,
        mention_id,
        "create",
        actor="mallory",
        data={"confirm": True, "title": "T", "body": "B"},
    )
    assert denied_create["status"] == 403
    assert runtime.enqueued == []

    preview = call(runtime, mention_id, "preview", actor="owner")
    assert preview["status"] == 200
    assert preview["data"]["requiresExplicitConfirmation"] is True
    assert preview["data"]["followupDefault"] is False
    assert NOTE["url"] in preview["data"]["draft"]["body"]

    unconfirmed = call(
        runtime,
        mention_id,
        "create",
        actor="owner",
        data={"title": "Save crash", "body": "Details"},
    )
    assert unconfirmed["status"] == 400
    assert runtime.enqueued == []

    queued = call(
        runtime,
        mention_id,
        "create",
        actor="owner",
        data={
            "confirm": True,
            "title": "Save crash",
            "body": "Details",
            "publishFollowup": True,
        },
    )
    assert queued["status"] == 202
    assert queued["data"]["state"] == "pending"
    assert queued["data"]["created"] is False
    assert queued["data"]["issueNumber"] == 0
    assert queued["data"]["proposedIssueNumber"] == 17
    assert len(runtime.enqueued) == 1
    assert runtime.enqueued[0]["mentionId"] == mention_id
    assert runtime.followups == []

    duplicate = call(
        runtime,
        mention_id,
        "create",
        actor="owner",
        data={
            "confirm": True,
            "title": "Duplicate",
            "body": "Duplicate",
        },
    )
    assert duplicate["data"]["deduplicated"] is True
    assert len(runtime.enqueued) == 1


def test_pending_stays_pending_until_owner_node_confirms_real_number():
    runtime = FakeRuntime()
    mention_id = record(runtime)
    call(
        runtime,
        mention_id,
        "create",
        actor="owner",
        data={
            "confirm": True,
            "title": "Save crash",
            "body": "Details",
            "publishFollowup": True,
        },
    )
    runtime.clock += 14 * 24 * 60 * 60 * 1000
    feed = run(api.handle(
        runtime.use("GET"), "/api/world/fediverse-mentions"))
    item = feed["data"]["items"][0]
    assert item["state"] == "pending"
    assert item["issueNumber"] == 0
    assert "Pending owner-node" in item["progress"]
    assert runtime.followups == []

    denied = call(
        runtime,
        mention_id,
        "materialized",
        owner_node="mallory",
        data={"issueNumber": 23},
    )
    assert denied["status"] == 403
    invalid = call(
        runtime,
        mention_id,
        "materialized",
        owner_node="owner",
        data={"issueNumber": 0},
    )
    assert invalid["status"] == 400

    confirmed = call(
        runtime,
        mention_id,
        "materialized",
        owner_node="owner",
        data={"issueNumber": 23},
    )
    assert confirmed["data"]["state"] == "created"
    assert confirmed["data"]["issueNumber"] == 23
    assert confirmed["data"]["issueUrl"].endswith(
        "/acme/project/issues/23")
    assert len(runtime.followups) == 1
    assert "owner node materialized" in runtime.followups[0]["message"]
    assert "/acme/project/issues/23" in runtime.followups[0]["message"]

    duplicate = call(
        runtime,
        mention_id,
        "materialized",
        owner_node="owner",
        data={"issueNumber": 23},
    )
    assert duplicate["data"]["deduplicated"] is True
    assert len(runtime.followups) == 1


def test_followup_is_consent_only_and_failure_does_not_undo_creation():
    runtime = FakeRuntime()
    mention_id = record(runtime)
    call(
        runtime,
        mention_id,
        "create",
        actor="owner",
        data={
            "confirm": True,
            "title": "Save crash",
            "body": "Details",
            "publishFollowup": False,
        },
    )
    created = call(
        runtime,
        mention_id,
        "materialized",
        owner_node="owner",
        data={"issueNumber": 19},
    )
    assert created["data"]["created"] is True
    assert runtime.followups == []

    second = FakeRuntime()
    second_id = record(second)
    call(
        second,
        second_id,
        "create",
        actor="owner",
        data={
            "confirm": True,
            "title": "Save crash",
            "body": "Details",
            "publishFollowup": True,
        },
    )
    second.followup_ok = False
    call(
        second,
        second_id,
        "materialized",
        owner_node="owner",
        data={"issueNumber": 21},
    )
    feed = run(api.handle(
        second.use("GET"), "/api/world/fediverse-mentions"))
    assert feed["data"]["items"][0]["state"] == "created"
    assert "delivery is delayed" in feed["data"]["items"][0]["progress"]


def test_queue_failure_is_retryable_and_progress_is_generalized():
    runtime = FakeRuntime()
    mention_id = record(runtime)
    runtime.enqueue_ok = False
    failed = call(
        runtime,
        mention_id,
        "create",
        actor="owner",
        data={"confirm": True, "title": "T", "body": "B"},
    )
    assert failed["status"] == 503
    feed = run(api.handle(
        runtime.use("GET"), "/api/world/fediverse-mentions"))
    item = feed["data"]["items"][0]
    assert item["state"] == "failed"
    assert "inbox_full" not in json.dumps(item)
    assert "retry" in item["progress"].lower()
    runtime.enqueue_ok = True
    retried = call(
        runtime,
        mention_id,
        "create",
        actor="owner",
        data={"confirm": True, "title": "T", "body": "B"},
    )
    assert retried["data"]["state"] == "pending"
    assert len(runtime.enqueued) == 2


def test_owner_moderation_hides_and_restores_reviewable_activity():
    runtime = FakeRuntime()
    mention_id = record(runtime)
    denied = call(
        runtime,
        mention_id,
        "moderate",
        actor="mallory",
        data={"action": "dismiss", "reason": "spam"},
    )
    assert denied["status"] == 403
    dismissed = call(
        runtime,
        mention_id,
        "moderate",
        actor="owner",
        data={"action": "dismiss", "reason": "Unrelated promotion."},
    )
    assert dismissed["data"]["state"] == "dismissed"
    hidden = run(api.handle(
        runtime.use("GET"), "/api/world/fediverse-mentions"))
    assert hidden["data"]["items"] == []
    restored = call(
        runtime,
        mention_id,
        "moderate",
        actor="owner",
        data={"action": "restore", "reason": "Reviewed on appeal."},
    )
    assert restored["data"]["state"] == "review"
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM world_fediverse_mention_moderation"
    ).fetchone()[0] == 2


def test_verified_reply_is_linked_but_never_offered_as_new_issue():
    runtime = FakeRuntime()
    reply_id = record(
        runtime,
        kind="reply",
        context={
            "ref": 8,
            "issueUrl": "https://forkmesh.com/acme/project/issues/8",
        },
    )
    feed = run(api.handle(
        runtime.use("GET"), "/api/world/fediverse-mentions"))
    item = feed["data"]["items"][0]
    assert item["kind"] == "reply"
    assert item["state"] == "linked"
    assert item["issueNumber"] == 8
    create = call(
        runtime,
        reply_id,
        "create",
        actor="owner",
        data={"confirm": True, "title": "T", "body": "B"},
    )
    assert create["status"] == 409
    assert runtime.enqueued == []
