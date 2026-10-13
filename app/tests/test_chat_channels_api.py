"""Executable authorization and persistence tests for private chat channels."""

import asyncio
import hashlib
import json
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import chat_channels_api as api  # noqa: E402


def run_async_test(function):
    def wrapper():
        return asyncio.run(function())

    wrapper.__name__ = function.__name__
    return wrapper


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        self.db.executescript(
            (ROOT / "migrations" / "0069_private_chat_channels.sql")
            .read_text(encoding="utf-8")
            + (ROOT / "migrations" / "0132_chat_channel_webhooks.sql")
            .read_text(encoding="utf-8")
            + (ROOT / "migrations" / "0227_org_office_site_chat.sql")
            .read_text(encoding="utf-8")
            + (ROOT / "migrations" / "0231_chat_channel_reads.sql")
            .read_text(encoding="utf-8")
            + (ROOT / "migrations" / "0254_chat_channel_invites.sql")
            .read_text(encoding="utf-8")
        )
        self.actor = ""
        self.request_method = "GET"
        self.request_data = {}
        self.clock = 1_800_000_000_000
        self.admins = {"admin"}
        self.accounts = {"admin", "alice", "bob"}
        self.audits = []
        self.account_events = []
        self.revoked_rooms = []
        self.ids = 0
        self.webhook_tokens = 0
        self.webhook_deliveries = []
        self.webhook_notifications = []
        self.fail_batch_at = None
        # room key -> [(ts, encrypted body)], as the room Durable Object
        # retained it. Bodies are opaque here exactly as they are in D1.
        self.retained = {}
        self.request_query = {}

    def use(self, method, actor="", data=None, query=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        self.request_query = dict(query or {})
        return self

    def query_params(self):
        return dict(self.request_query)

    def method(self):
        return self.request_method

    def now(self):
        return self.clock

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def new_webhook_token(self):
        self.webhook_tokens += 1
        return f"{self.webhook_tokens:064x}"

    def webhook_url(self, token):
        return "https://forkmesh.test/api/chat/webhooks/" + token

    def invite_url(self, token):
        return "https://forkmesh.test/chat#invite=" + token

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cache_control": cache_control,
            "headers": dict(extra_headers or {}),
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        if not isinstance(self.request_data, dict):
            return None, "invalid_json"
        return self.request_data, ""

    async def session(self, _data=None):
        if self.actor not in self.accounts:
            return "", None
        return f"bi:{self.actor}", {
            "name": self.actor,
            "status": "active",
        }

    async def is_admin(self, name):
        return name in self.admins

    async def account(self, name):
        normalized = str(name or "").strip().lower()
        if normalized not in self.accounts:
            return "", ""
        return f"bi:{normalized}", normalized

    async def blind(self, value):
        digest = hashlib.sha256(str(value).strip().lower().encode()).hexdigest()
        return "blind:" + digest

    async def seal(self, value):
        return json.dumps(value, sort_keys=True)

    async def open(self, value):
        return json.loads(value)

    async def notify_account(self, owner, topic):
        # Payload-free push to one account's clients (adhoc #1604).
        self.account_events.append((owner, topic))

    async def deliver_webhook(self, channel_id, key_version, plain):
        self.webhook_deliveries.append((channel_id, key_version, plain))

    async def notify_webhook(self, recipient, channel, webhook, text,
                             channel_id, message_id):
        self.webhook_notifications.append({
            "recipient": recipient,
            "channel": channel,
            "webhook": webhook,
            "text": text,
            "channelId": channel_id,
            "messageId": message_id,
        })

    async def public_webhook_recipients(self):
        return sorted(self.accounts)

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.audits.append({
            "actor": actor,
            "action": action,
            "target_type": target_type,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    async def room_access(self, channel_id, key_version, account_bi, actor):
        assert actor == self.actor
        return {
            "room": f"private-{channel_id}-v{key_version}",
            "passphrase": f"key:{channel_id}:{key_version}",
            "webSocketUrl": (
                f"/api/chat/channels/{channel_id}/ws?ticket="
                f"ticket:{account_bi}"
            ),
            "meetingWebSocketUrl": (
                f"/api/world/office/channels/{channel_id}/ws?ticket="
                f"meeting:{account_bi}"
            ),
        }

    async def revoke_room(self, channel_id, key_version):
        self.revoked_rooms.append((channel_id, key_version))

    async def channel_passphrase(self, channel_id, key_version):
        return f"key:{channel_id}:{key_version}"

    async def history(self, channel_id, key_version, since_ts=0):
        room = f"chat-channel:{channel_id}:v{key_version}"
        return [
            {"ts": ts, "body": body}
            for ts, body in self.retained.get(room, [])
            if ts > int(since_ts or 0)
        ]

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return {"changes": cursor.rowcount}

    async def batch(self, statements):
        self.db.execute("BEGIN")
        try:
            for index, (sql, args) in enumerate(statements):
                if self.fail_batch_at == index:
                    raise RuntimeError("forced_batch_failure")
                self.db.execute(sql, args)
            self.db.commit()
        except Exception:
            self.db.rollback()
            raise


async def create_channel(runtime, name, visibility=None, members=None,
                         actor="admin"):
    data = {"name": name}
    if visibility is not None:
        data["visibility"] = visibility
    if members is not None:
        data["members"] = members
    return await api.handle(
        runtime.use("POST", actor, data),
        "/api/chat/channels",
    )


@run_async_test
async def test_registered_user_creates_and_manages_a_private_channel():
    runtime = FakeRuntime()
    created = await api.handle(
        runtime.use("POST", "alice", {"name": "release-team"}),
        "/api/chat/channels",
    )
    assert created["status"] == 201
    assert created["data"]["channel"] == {
        "id": "00000000000000000000000000000001",
        "name": "release-team",
        "updatedAt": runtime.clock,
        "keyVersion": 1,
        "canManage": True,
        "visibility": "private",
        "topic": "",
        "members": ["alice"],
    }
    stored = runtime.db.execute(
        "SELECT name_bi,data,created_by_bi,key_version FROM chat_channels"
    ).fetchone()
    assert stored["name_bi"] != "release-team"
    assert json.loads(stored["data"])["name"] == "release-team"
    assert stored["created_by_bi"] == "bi:alice"
    assert stored["key_version"] == 1

    listed = await api.handle(
        runtime.use("GET", "alice"), "/api/chat/channels")
    assert listed["data"]["channels"][0]["canManage"] is True
    access = await api.handle(
        runtime.use("GET", "alice"),
        f"/api/chat/channels/{created['data']['channel']['id']}/room-access",
    )
    assert access["status"] == 200


@run_async_test
async def test_admin_creates_private_channel_with_initial_registered_members():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime,
        "release-team",
        visibility="private",
        members=["Alice", "alice", "admin", "bob"],
    )

    assert created["status"] == 201
    assert created["data"]["channel"]["visibility"] == "private"
    assert created["data"]["members"] == [
        {"username": "alice", "joinedAt": runtime.clock},
        {"username": "bob", "joinedAt": runtime.clock},
    ]
    rows = runtime.db.execute(
        "SELECT data FROM chat_channel_members ORDER BY member_bi"
    ).fetchall()
    assert sorted(json.loads(row["data"])["username"] for row in rows) == [
        "alice",
        "bob",
    ]
    # Each invitee's channel list changed for somebody else's reason, and the
    # chat page reads that list once when it opens rather than on a timer, so
    # they are told (adhoc #1604). "admin" created it and is not notified.
    assert sorted(runtime.account_events) == [
        ("alice", "private-channels"),
        ("bob", "private-channels"),
    ]


@run_async_test
async def test_channel_creation_rejects_invalid_visibility_and_members():
    runtime = FakeRuntime()

    invalid_visibility = await create_channel(
        runtime, "release-team", visibility="shared")
    assert invalid_visibility["status"] == 400
    assert invalid_visibility["data"] == {"error": "invalid_visibility"}

    invalid_members = await create_channel(
        runtime, "release-team", visibility="private", members="alice")
    assert invalid_members["status"] == 400
    assert invalid_members["data"] == {"error": "invalid_members"}

    invalid_member_item = await create_channel(
        runtime, "release-team", visibility="private", members=[1])
    assert invalid_member_item["status"] == 400
    assert invalid_member_item["data"] == {"error": "invalid_members"}

    too_many_members = await create_channel(
        runtime,
        "release-team",
        visibility="private",
        members=["alice"] * (api.MAX_CHANNEL_MEMBERS + 1),
    )
    assert too_many_members["status"] == 429
    assert too_many_members["data"] == {"error": "too_many_members"}

    public_members = await create_channel(
        runtime, "release-team", visibility="public", members=["alice"])
    assert public_members["status"] == 400
    assert public_members["data"] == {"error": "members_not_allowed"}

    missing_user = await create_channel(
        runtime, "release-team", visibility="private", members=["missing"])
    assert missing_user["status"] == 404
    assert missing_user["data"] == {"error": "user_not_found"}
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chat_channels").fetchone()[0] == 0
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chat_channel_members").fetchone()[0] == 0


@run_async_test
async def test_channel_and_initial_members_are_written_in_one_transaction():
    runtime = FakeRuntime()
    runtime.fail_batch_at = 1

    try:
        await create_channel(
            runtime,
            "release-team",
            visibility="private",
            members=["alice"],
        )
    except RuntimeError as error:
        assert str(error) == "forced_batch_failure"
    else:
        raise AssertionError("batch failure must escape the API boundary")

    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chat_channels").fetchone()[0] == 0
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chat_channel_members").fetchone()[0] == 0


@run_async_test
async def test_registered_users_list_and_access_public_but_not_other_private_channels():
    runtime = FakeRuntime()
    public = await create_channel(
        runtime, "announcements", visibility="public")
    private = await create_channel(
        runtime,
        "release-team",
        visibility="private",
        members=["alice"],
    )

    alice_list = await api.handle(
        runtime.use("GET", "alice"), "/api/chat/channels")
    bob_list = await api.handle(
        runtime.use("GET", "bob"), "/api/chat/channels")
    assert [item["name"] for item in alice_list["data"]["channels"]] == [
        "announcements",
        "release-team",
    ]
    assert "members" not in alice_list["data"]["channels"][0]
    assert alice_list["data"]["channels"][1]["members"] == ["admin", "alice"]
    assert [item["name"] for item in bob_list["data"]["channels"]] == [
        "announcements"
    ]

    public_id = public["data"]["channel"]["id"]
    private_id = private["data"]["channel"]["id"]
    public_access = await api.handle(
        runtime.use("GET", "bob"),
        f"/api/chat/channels/{public_id}/room-access",
    )
    private_access = await api.handle(
        runtime.use("GET", "bob"),
        f"/api/chat/channels/{private_id}/room-access",
    )
    assert public_access["status"] == 200
    assert public_access["data"]["channel"]["visibility"] == "public"
    assert private_access["status"] == 404


@run_async_test
async def test_legacy_channel_without_visibility_remains_private():
    runtime = FakeRuntime()
    runtime.db.execute(
        "INSERT INTO chat_channels "
        "(channel_id,name_bi,data,created_by_bi,created_at,updated_at,key_version) "
        "VALUES (?,?,?,?,?,?,1)",
        (
            "f" * 32,
            "legacy-name-bi",
            json.dumps({"name": "legacy"}),
            "bi:admin",
            runtime.clock,
            runtime.clock,
        ),
    )
    runtime.db.commit()

    listed = await api.handle(
        runtime.use("GET", "bob"), "/api/chat/channels")
    access = await api.handle(
        runtime.use("GET", "bob"),
        f"/api/chat/channels/{'f' * 32}/room-access",
    )
    assert listed["data"]["channels"] == []
    assert access["status"] == 404


@run_async_test
async def test_public_channel_rejects_private_membership_management():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "announcements", visibility="public")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}/members"

    for method, data in (
        ("GET", None),
        ("POST", {"username": "alice"}),
        ("DELETE", {"username": "alice"}),
    ):
        response = await api.handle(
            runtime.use(method, "admin", data), path)
        assert response["status"] == 409
        assert response["data"] == {"error": "members_not_allowed"}


@run_async_test
async def test_channel_collection_requires_a_session_and_validates_names():
    runtime = FakeRuntime()
    unauthenticated = await api.handle(
        runtime.use("GET"), "/api/chat/channels")
    assert unauthenticated["status"] == 401
    assert unauthenticated["data"] == {"error": "invalid_session"}

    invalid = await create_channel(runtime, "Release Team")
    assert invalid["status"] == 400
    assert invalid["data"] == {"error": "invalid_channel_name"}

    created = await create_channel(runtime, "release-team")
    duplicate = await create_channel(runtime, "release-team")
    assert created["status"] == 201
    assert duplicate["status"] == 409
    assert duplicate["data"] == {"error": "channel_name_taken"}


@run_async_test
async def test_admin_updates_and_clears_a_channel_topic():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "release-team", visibility="public")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}"

    denied = await api.handle(
        runtime.use("PATCH", "alice", {"topic": "Release coordination"}),
        path,
    )
    assert denied["status"] == 403
    assert denied["data"] == {"error": "manager_required"}

    runtime.clock += 1000
    updated = await api.handle(
        runtime.use(
            "PATCH", "admin", {"topic": "  Release\n coordination  "}),
        path,
    )
    assert updated["status"] == 200
    assert updated["data"]["channel"]["topic"] == "Release coordination"
    assert updated["data"]["channel"]["updatedAt"] == runtime.clock
    assert runtime.audits[-1]["action"] == "chat.channel.topic.update"

    listed = await api.handle(
        runtime.use("GET", "alice"), "/api/chat/channels")
    assert listed["data"]["channels"][0]["topic"] == "Release coordination"
    stored = runtime.db.execute(
        "SELECT data FROM chat_channels WHERE channel_id=?", (channel_id,)
    ).fetchone()
    assert json.loads(stored["data"])["topic"] == "Release coordination"

    cleared = await api.handle(
        runtime.use("PATCH", "admin", {"topic": ""}), path)
    assert cleared["status"] == 200
    assert cleared["data"]["channel"]["topic"] == ""
    stored = runtime.db.execute(
        "SELECT data FROM chat_channels WHERE channel_id=?", (channel_id,)
    ).fetchone()
    assert "topic" not in json.loads(stored["data"])


@run_async_test
async def test_channel_topic_update_validates_input_and_method():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}"

    for topic in (None, 42, "x" * (api.MAX_CHANNEL_TOPIC_LENGTH + 1)):
        invalid = await api.handle(
            runtime.use("PATCH", "admin", {"topic": topic}), path)
        assert invalid["status"] == 400
        assert invalid["data"] == {"error": "invalid_topic"}

    wrong_method = await api.handle(runtime.use("GET", "admin"), path)
    assert wrong_method["status"] == 405
    assert wrong_method["headers"]["allow"] == "PATCH, DELETE"

    empty = await api.handle(runtime.use("PATCH", "admin", {}), path)
    assert empty["status"] == 400
    assert empty["data"] == {"error": "invalid_topic"}


@run_async_test
async def test_invited_user_lists_and_accesses_only_their_channel():
    runtime = FakeRuntime()
    first = await create_channel(runtime, "release-team")
    second = await create_channel(runtime, "security")
    channel_id = first["data"]["channel"]["id"]
    granted = await api.handle(
        runtime.use("POST", "admin", {"username": "alice"}),
        f"/api/chat/channels/{channel_id}/members",
    )
    assert granted["status"] == 201
    assert granted["data"]["member"]["username"] == "alice"

    listed = await api.handle(
        runtime.use("GET", "alice"), "/api/chat/channels")
    assert [item["name"] for item in listed["data"]["channels"]] == [
        "release-team"
    ]
    assert second["data"]["channel"]["id"] not in json.dumps(listed["data"])

    access = await api.handle(
        runtime.use("GET", "alice"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    assert access["status"] == 200
    assert access["data"]["channel"]["name"] == "release-team"
    assert access["data"]["passphrase"].startswith("key:")
    assert access["data"]["meetingWebSocketUrl"].startswith(
        f"/api/world/office/channels/{channel_id}/ws?ticket=")
    assert access["cache_control"].startswith("no-store")


@run_async_test
async def test_member_management_is_manager_only_and_idempotent():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}/members"

    hidden = await api.handle(
        runtime.use("POST", "alice", {"username": "bob"}), path)
    assert hidden["status"] == 404
    assert hidden["data"] == {"error": "not_found"}

    first = await api.handle(
        runtime.use("POST", "admin", {"username": "alice"}), path)
    again = await api.handle(
        runtime.use("POST", "admin", {"username": "alice"}), path)
    assert first["status"] == 201
    assert again["status"] == 200

    denied = await api.handle(
        runtime.use("POST", "alice", {"username": "bob"}), path)
    assert denied["status"] == 403
    assert denied["data"] == {"error": "manager_required"}
    assert runtime.audits[-1]["outcome"] == "denied"
    members = await api.handle(runtime.use("GET", "admin"), path)
    assert members["data"]["members"] == [
        {"username": "alice", "joinedAt": runtime.clock}
    ]

    runtime.accounts.remove("bob")
    missing = await api.handle(
        runtime.use("POST", "admin", {"username": "bob"}), path)
    assert missing["status"] == 404
    assert missing["data"] == {"error": "user_not_found"}


@run_async_test
async def test_member_removal_rotates_once_and_revokes_access():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}/members"
    await api.handle(
        runtime.use("POST", "admin", {"username": "alice"}), path)

    removed = await api.handle(
        runtime.use("DELETE", "admin", {"username": "alice"}), path)
    assert removed["status"] == 200
    assert removed["data"]["removed"] is True
    assert removed["data"]["keyVersion"] == 2
    assert runtime.revoked_rooms == [(channel_id, 1)]

    again = await api.handle(
        runtime.use("DELETE", "admin", {"username": "alice"}), path)
    assert again["status"] == 200
    assert again["data"]["removed"] is False
    assert again["data"]["keyVersion"] == 2
    assert runtime.revoked_rooms == [(channel_id, 1)]

    denied = await api.handle(
        runtime.use("GET", "alice"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    missing = await api.handle(
        runtime.use("GET", "alice"),
        f"/api/chat/channels/{'f' * 32}/room-access",
    )
    assert denied["status"] == missing["status"] == 404
    assert denied["data"] == missing["data"] == {"error": "not_found"}


@run_async_test
async def test_member_removal_does_not_depend_on_d1_run_metadata():
    class ProductionLikeRuntime(FakeRuntime):
        async def d1_run(self, sql, *args):
            await super().d1_run(sql, *args)
            return None

    runtime = ProductionLikeRuntime()
    created = await create_channel(runtime, "release-team")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}/members"
    await api.handle(
        runtime.use("POST", "admin", {"username": "alice"}), path)

    removed = await api.handle(
        runtime.use("DELETE", "admin", {"username": "alice"}), path)
    assert removed["data"]["removed"] is True
    assert removed["data"]["keyVersion"] == 2


@run_async_test
async def test_admins_have_no_implicit_access_to_private_channels():
    runtime = FakeRuntime()
    runtime.admins.add("bob")
    created = await create_channel(
        runtime, "release-team", actor="alice", members=["admin"])
    channel_id = created["data"]["channel"]["id"]
    assert created["data"]["channel"]["members"] == ["alice", "admin"]

    listed = await api.handle(
        runtime.use("GET", "bob"), "/api/chat/channels")
    assert listed["data"]["channels"] == []
    for method, path, data in (
        ("GET", f"/api/chat/channels/{channel_id}/room-access", None),
        ("GET", f"/api/chat/channels/{channel_id}/history", None),
        ("GET", f"/api/chat/channels/{channel_id}/members", None),
        ("POST", f"/api/chat/channels/{channel_id}/members",
         {"username": "bob"}),
        ("PATCH", f"/api/chat/channels/{channel_id}", {"topic": "x"}),
        ("DELETE", f"/api/chat/channels/{channel_id}", {}),
    ):
        response = await api.handle(
            runtime.use(method, "bob", data), path)
        assert response["status"] == 404, (method, path)
        assert response["data"] == {"error": "not_found"}

    invited_admin = await api.handle(
        runtime.use("GET", "admin"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    assert invited_admin["status"] == 200
    assert invited_admin["data"]["channel"]["canManage"] is False
    count = runtime.db.execute(
        "SELECT COUNT(*) FROM chat_channel_members"
    ).fetchone()[0]
    assert count == 1


@run_async_test
async def test_non_member_gets_not_found_from_every_private_channel_endpoint():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "secret-plans", actor="alice", visibility="private")
    channel_id = created["data"]["channel"]["id"]
    missing_id = "f" * 32
    webhook_id = "e" * 32
    cases = (
        ("GET", "/api/chat/channels/{}/room-access", None),
        ("GET", "/api/chat/channels/{}/history", None),
        ("GET", "/api/chat/channels/{}/members", None),
        ("POST", "/api/chat/channels/{}/members", {"username": "bob"}),
        ("DELETE", "/api/chat/channels/{}/members", {"username": "alice"}),
        ("PATCH", "/api/chat/channels/{}", {"name": "renamed"}),
        ("DELETE", "/api/chat/channels/{}", {}),
        ("GET", "/api/chat/channels/{}/webhooks", None),
        ("POST", "/api/chat/channels/{}/webhooks", {"name": "CI"}),
        ("POST", "/api/chat/channels/{}/webhooks/" + webhook_id, {}),
        ("DELETE", "/api/chat/channels/{}/webhooks/" + webhook_id, {}),
    )
    for method, template, data in cases:
        hidden = await api.handle(
            runtime.use(method, "bob", data), template.format(channel_id))
        absent = await api.handle(
            runtime.use(method, "bob", data), template.format(missing_id))
        assert hidden["status"] == absent["status"] == 404, (method, template)
        assert hidden["data"] == absent["data"] == {"error": "not_found"}

    listed = await api.handle(runtime.use("GET", "bob"), "/api/chat/channels")
    assert channel_id not in json.dumps(listed["data"])
    assert "secret-plans" not in json.dumps(listed["data"])
    assert json.loads(runtime.db.execute(
        "SELECT data FROM chat_channels").fetchone()[0])["name"] == (
        "secret-plans")
    assert runtime.revoked_rooms == []


@run_async_test
async def test_private_channel_membership_is_exactly_the_invited_people():
    runtime = FakeRuntime()
    runtime.accounts.update({"carol", "dave"})
    created = await create_channel(
        runtime, "not-very-secret", actor="alice", visibility="private",
        members=["bob"])
    channel_id = created["data"]["channel"]["id"]
    assert created["data"]["channel"]["members"] == ["alice", "bob"]
    for outsider in ("carol", "dave", "admin"):
        listed = await api.handle(
            runtime.use("GET", outsider), "/api/chat/channels")
        assert listed["data"]["channels"] == [], outsider
    for insider in ("alice", "bob"):
        listed = await api.handle(
            runtime.use("GET", insider), "/api/chat/channels")
        assert [item["id"] for item in listed["data"]["channels"]] == [
            channel_id]
        assert listed["data"]["channels"][0]["members"] == ["alice", "bob"]

    await api.handle(
        runtime.use("POST", "alice", {"username": "carol"}),
        f"/api/chat/channels/{channel_id}/members",
    )
    carol = await api.handle(
        runtime.use("GET", "carol"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    assert carol["status"] == 200
    dave = await api.handle(
        runtime.use("GET", "dave"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    assert dave["status"] == 404


@run_async_test
async def test_admin_still_manages_public_channels():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "lobby", actor="alice", visibility="public")
    channel_id = created["data"]["channel"]["id"]
    listed = await api.handle(runtime.use("GET", "admin"), "/api/chat/channels")
    assert listed["data"]["channels"][0]["canManage"] is True
    renamed = await api.handle(
        runtime.use("PATCH", "admin", {"topic": "welcome"}),
        f"/api/chat/channels/{channel_id}",
    )
    assert renamed["status"] == 200


async def read_history(runtime, actor, channel_id, since=None):
    return await api.handle(
        runtime.use(
            "GET", actor,
            query={} if since is None else {"since": str(since)}),
        f"/api/chat/channels/{channel_id}/history",
    )


@run_async_test
async def test_history_replays_encrypted_backlog_to_a_reader():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "design", visibility="private", members=["alice"])
    channel_id = created["data"]["channel"]["id"]
    runtime.retained[f"chat-channel:{channel_id}:v1"] = [
        (100, '{"iv":"a","data":"first"}'),
        (200, '{"iv":"b","data":"second"}'),
    ]

    replayed = await read_history(runtime, "alice", channel_id)
    assert replayed["status"] == 200
    assert replayed["data"]["room"] == f"chat-channel:{channel_id}:v1"
    assert replayed["data"]["keyVersion"] == 1
    assert replayed["data"]["passphrase"] == f"key:{channel_id}:1"
    assert replayed["data"]["channel"]["name"] == "design"
    assert replayed["data"]["messages"] == [
        {"ts": 100, "body": '{"iv":"a","data":"first"}'},
        {"ts": 200, "body": '{"iv":"b","data":"second"}'},
    ]
    assert replayed["data"]["latestTs"] == 200
    assert replayed["cache_control"] == "no-store, max-age=0, must-revalidate"

    tail = await read_history(runtime, "alice", channel_id, since=100)
    assert [m["ts"] for m in tail["data"]["messages"]] == [200]
    assert tail["data"]["latestTs"] == 200
    caught_up = await read_history(runtime, "alice", channel_id, since=200)
    assert caught_up["data"]["messages"] == []
    assert caught_up["data"]["latestTs"] == 200


@run_async_test
async def test_history_is_gated_by_the_same_membership_as_room_access():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "design", visibility="private", members=["alice"])
    channel_id = created["data"]["channel"]["id"]
    runtime.retained[f"chat-channel:{channel_id}:v1"] = [
        (100, '{"iv":"a","data":"first"}'),
    ]

    denied = await read_history(runtime, "bob", channel_id)
    assert denied["status"] == 404
    assert denied["data"] == {"error": "not_found"}
    assert (await read_history(runtime, "admin", channel_id))["status"] == 200
    runtime.admins.add("bob")
    assert (await read_history(runtime, "bob", channel_id))["status"] == 404
    unauthenticated = await read_history(runtime, "nobody", channel_id)
    assert unauthenticated["status"] == 401
    assert unauthenticated["data"] == {"error": "invalid_session"}
    missing = await read_history(runtime, "alice", "f" * 32)
    assert missing["status"] == 404


@run_async_test
async def test_public_channel_history_is_readable_without_membership():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "lobby", visibility="public")
    channel_id = created["data"]["channel"]["id"]
    runtime.retained[f"chat-channel:{channel_id}:v1"] = [
        (100, '{"iv":"a","data":"first"}'),
    ]

    read = await read_history(runtime, "bob", channel_id)
    assert read["status"] == 200
    assert [m["ts"] for m in read["data"]["messages"]] == [100]


@run_async_test
async def test_history_rejects_writes_and_malformed_since():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "lobby", visibility="public")
    channel_id = created["data"]["channel"]["id"]
    runtime.retained[f"chat-channel:{channel_id}:v1"] = [
        (100, '{"iv":"a","data":"first"}'),
    ]

    posted = await api.handle(
        runtime.use("POST", "admin", {}),
        f"/api/chat/channels/{channel_id}/history",
    )
    assert posted["status"] == 405
    assert posted["headers"]["allow"] == "GET"

    junk = await read_history(runtime, "bob", channel_id, since="tomorrow")
    assert junk["status"] == 200
    assert [m["ts"] for m in junk["data"]["messages"]] == [100]


@run_async_test
async def test_creator_manages_private_members_while_other_users_cannot():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "design", visibility="private", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}/members"

    invited = await api.handle(
        runtime.use("POST", "alice", {"username": "bob"}), path)
    assert invited["status"] == 201
    denied = await api.handle(
        runtime.use("POST", "bob", {"username": "admin"}), path)
    assert denied["status"] == 403
    assert denied["data"] == {"error": "manager_required"}


@run_async_test
async def test_public_channels_support_incoming_webhooks():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "public-updates", visibility="public", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    collection = f"/api/chat/channels/{channel_id}/webhooks"

    webhook = await api.handle(
        runtime.use("POST", "alice", {"name": "Deploy bot"}),
        collection,
    )
    assert webhook["status"] == 201
    assert webhook["data"]["url"].startswith(
        "https://forkmesh.test/api/chat/webhooks/")
    token = webhook["data"]["url"].rsplit("/", 1)[-1]
    dump = "\n".join(runtime.db.iterdump())
    assert token not in dump

    listed = await api.handle(runtime.use("GET", "alice"), collection)
    assert listed["data"]["webhooks"] == [{
        "id": webhook["data"]["webhook"]["id"],
        "name": "Deploy bot",
        "createdAt": runtime.clock,
        "lastUsedAt": 0,
    }]
    assert "url" not in listed["data"]["webhooks"][0]

    delivery = await api.handle(
        runtime.use("POST", "", {"text": "Build passed"}),
        f"/api/chat/webhooks/{token}",
    )
    assert delivery["status"] == 202
    assert runtime.webhook_deliveries[0][0:2] == (channel_id, 1)
    plain = runtime.webhook_deliveries[0][2]
    assert plain["sender"] == "Deploy bot"
    assert plain["channel"] == "#public-updates"
    assert plain["text"] == "Build passed"
    recipients = {
        item["recipient"] for item in runtime.webhook_notifications
    }
    assert recipients == {"admin", "alice", "bob"}


@run_async_test
async def test_private_channels_reject_controller_webhooks():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "private-updates", visibility="private", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    result = await api.handle(
        runtime.use("POST", "alice", {"name": "Deploy bot"}),
        f"/api/chat/channels/{channel_id}/webhooks",
    )
    assert result["status"] == 409
    assert result["data"] == {"error": "public_channel_required"}
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM chat_channel_webhooks"
    ).fetchone()["n"] == 0


@run_async_test
async def test_webhook_management_is_owner_or_admin_only_and_revocable():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "ops", visibility="public", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    collection = f"/api/chat/channels/{channel_id}/webhooks"

    denied = await api.handle(
        runtime.use("POST", "bob", {"name": "Nope"}), collection)
    assert denied["status"] == 403
    assert denied["data"] == {"error": "manager_required"}

    made = await api.handle(
        runtime.use("POST", "admin", {"name": "CI"}), collection)
    webhook_id = made["data"]["webhook"]["id"]
    removed = await api.handle(
        runtime.use("DELETE", "alice", {}),
        f"{collection}/{webhook_id}",
    )
    assert removed["data"] == {"ok": True, "removed": True}
    token = made["data"]["url"].rsplit("/", 1)[-1]
    missing = await api.handle(
        runtime.use("POST", "", {"message": "too late"}),
        f"/api/chat/webhooks/{token}",
    )
    assert missing["status"] == 404


@run_async_test
async def test_manager_renames_and_reopens_a_channel_from_settings():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}"
    await api.handle(
        runtime.use("POST", "alice", {"username": "bob"}),
        f"{path}/members",
    )

    taken = await create_channel(runtime, "security", actor="alice")
    assert taken["status"] == 201
    clash = await api.handle(
        runtime.use("PATCH", "alice", {"name": "security"}), path)
    assert clash["status"] == 409
    assert clash["data"] == {"error": "channel_name_taken"}

    invalid = await api.handle(
        runtime.use("PATCH", "alice", {"name": "Release Team"}), path)
    assert invalid["status"] == 400
    assert invalid["data"] == {"error": "invalid_channel_name"}

    denied = await api.handle(
        runtime.use("PATCH", "bob", {"name": "shipping"}), path)
    assert denied["status"] == 403
    assert denied["data"] == {"error": "manager_required"}

    runtime.clock += 1000
    renamed = await api.handle(
        runtime.use(
            "PATCH",
            "alice",
            {"name": "shipping", "topic": "Ship it", "visibility": "public"},
        ),
        path,
    )
    assert renamed["status"] == 200
    assert renamed["data"]["channel"]["name"] == "shipping"
    assert renamed["data"]["channel"]["topic"] == "Ship it"
    assert renamed["data"]["channel"]["visibility"] == "public"
    assert renamed["data"]["channel"]["updatedAt"] == runtime.clock
    actions = [entry["action"] for entry in runtime.audits[-3:]]
    assert actions == [
        "chat.channel.rename",
        "chat.channel.visibility.update",
        "chat.channel.topic.update",
    ]
    stored = runtime.db.execute(
        "SELECT name_bi,data FROM chat_channels WHERE channel_id=?",
        (channel_id,),
    ).fetchone()
    assert json.loads(stored["data"])["name"] == "shipping"
    # The uniqueness index moved with the name, so the old one frees up.
    assert stored["name_bi"] == await runtime.blind(
        "chat-channel-name:shipping")
    reused = await create_channel(runtime, "release-team", actor="alice")
    assert reused["status"] == 201

    # Reopening the name it already has is a no-op, not a self-collision.
    unchanged = await api.handle(
        runtime.use("PATCH", "alice", {"name": "shipping"}), path)
    assert unchanged["status"] == 200
    assert unchanged["data"]["channel"]["name"] == "shipping"


@run_async_test
async def test_closing_a_public_channel_rotates_its_room_key():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "release-team", visibility="public", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}"
    made = await api.handle(
        runtime.use("POST", "alice", {"name": "CI"}),
        f"{path}/webhooks",
    )
    token = made["data"]["url"].rsplit("/", 1)[-1]

    invalid = await api.handle(
        runtime.use("PATCH", "alice", {"visibility": "secret"}), path)
    assert invalid["status"] == 400
    assert invalid["data"] == {"error": "invalid_visibility"}

    closed = await api.handle(
        runtime.use("PATCH", "alice", {"visibility": "private"}), path)
    assert closed["status"] == 200
    assert closed["data"]["channel"]["visibility"] == "private"
    assert closed["data"]["channel"]["keyVersion"] == 2
    assert runtime.revoked_rooms == [(channel_id, 1)]
    stored = runtime.db.execute(
        "SELECT key_version FROM chat_channels WHERE channel_id=?",
        (channel_id,),
    ).fetchone()
    assert stored["key_version"] == 2
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM chat_channel_webhooks"
    ).fetchone()["n"] == 0
    retired = await api.handle(
        runtime.use("POST", "", {"text": "too late"}),
        f"/api/chat/webhooks/{token}",
    )
    assert retired["status"] == 404

    # Bob was never a member, so the closed channel leaves his list.
    listed = await api.handle(
        runtime.use("GET", "bob"), "/api/chat/channels")
    assert listed["data"]["channels"] == []

    # Reopening it does not rotate again: widening access locks nobody out.
    opened = await api.handle(
        runtime.use("PATCH", "alice", {"visibility": "public"}), path)
    assert opened["data"]["channel"]["keyVersion"] == 2
    assert runtime.revoked_rooms == [(channel_id, 1)]


@run_async_test
async def test_manager_deletes_a_channel_with_members_and_webhooks():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}"
    await api.handle(
        runtime.use("POST", "alice", {"username": "bob"}),
        f"{path}/members",
    )
    await api.handle(
        runtime.use("PATCH", "alice", {"visibility": "public"}), path)
    await api.handle(
        runtime.use("POST", "alice", {"name": "CI"}), f"{path}/webhooks")
    key_version = int(runtime.db.execute(
        "SELECT key_version FROM chat_channels WHERE channel_id=?",
        (channel_id,),
    ).fetchone()["key_version"])

    denied = await api.handle(runtime.use("DELETE", "bob", {}), path)
    assert denied["status"] == 403
    assert denied["data"] == {"error": "manager_required"}
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM chat_channels").fetchone()["n"] == 1

    runtime.account_events.clear()
    removed = await api.handle(runtime.use("DELETE", "alice", {}), path)
    assert removed["status"] == 200
    assert removed["data"] == {"ok": True, "removed": True}
    assert runtime.revoked_rooms == [(channel_id, key_version)]
    assert runtime.account_events == [("bob", "private-channels")]
    assert runtime.audits[-1]["action"] == "chat.channel.delete"
    for table in (
        "chat_channels", "chat_channel_members", "chat_channel_webhooks",
    ):
        assert runtime.db.execute(
            f"SELECT COUNT(*) AS n FROM {table}").fetchone()["n"] == 0

    gone = await api.handle(runtime.use("DELETE", "alice", {}), path)
    assert gone["status"] == 404
    assert gone["data"] == {"error": "not_found"}
    listed = await api.handle(
        runtime.use("GET", "alice"), "/api/chat/channels")
    assert listed["data"]["channels"] == []
    # The freed name is available again.
    assert (await create_channel(
        runtime, "release-team", actor="alice"))["status"] == 201


@run_async_test
async def test_rotating_a_webhook_reveals_a_new_url_and_retires_the_old():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "ops", visibility="public", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    collection = f"/api/chat/channels/{channel_id}/webhooks"
    made = await api.handle(
        runtime.use("POST", "alice", {"name": "CI"}), collection)
    webhook_id = made["data"]["webhook"]["id"]
    old_token = made["data"]["url"].rsplit("/", 1)[-1]
    await api.handle(
        runtime.use("POST", "", {"text": "first"}),
        f"/api/chat/webhooks/{old_token}",
    )

    denied = await api.handle(
        runtime.use("POST", "bob", {}), f"{collection}/{webhook_id}")
    assert denied["status"] == 403
    assert denied["data"] == {"error": "manager_required"}

    missing = await api.handle(
        runtime.use("POST", "alice", {}), f"{collection}/{'f' * 32}")
    assert missing["status"] == 404

    rotated = await api.handle(
        runtime.use("POST", "alice", {}), f"{collection}/{webhook_id}")
    assert rotated["status"] == 200
    assert rotated["data"]["webhook"]["id"] == webhook_id
    assert rotated["data"]["webhook"]["name"] == "CI"
    assert rotated["data"]["webhook"]["lastUsedAt"] == 0
    new_token = rotated["data"]["url"].rsplit("/", 1)[-1]
    assert new_token != old_token
    assert runtime.audits[-1]["action"] == "chat.channel.webhook.rotate"

    retired = await api.handle(
        runtime.use("POST", "", {"text": "stale"}),
        f"/api/chat/webhooks/{old_token}",
    )
    assert retired["status"] == 404
    accepted = await api.handle(
        runtime.use("POST", "", {"text": "fresh"}),
        f"/api/chat/webhooks/{new_token}",
    )
    assert accepted["status"] == 202
    # Listing still never exposes a token.
    listed = await api.handle(runtime.use("GET", "alice"), collection)
    assert "url" not in listed["data"]["webhooks"][0]


@run_async_test
async def test_history_entries_carry_the_relay_recipient_count():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "design", visibility="private", members=["alice"])
    channel_id = created["data"]["channel"]["id"]
    runtime.retained[f"chat-channel:{channel_id}:v1"] = [
        (100, '{"kind":"cipher","v":1,"persist":true,"recipients":2,'
              '"recipientNames":["alice","bob"]}'),
        (200, '{"kind":"cipher","v":1,"persist":true}'),
        (300, '{"kind":"cipher","recipients":true}'),
        (400, '{"kind":"cipher","recipients":-1}'),
        (500, "not json"),
    ]

    replayed = await read_history(runtime, "alice", channel_id)

    messages = replayed["data"]["messages"]
    assert messages[0]["recipients"] == 2
    assert messages[0]["recipientNames"] == ["alice", "bob"]
    assert json.loads(messages[0]["body"])["recipients"] == 2
    assert all("recipients" not in message for message in messages[1:])
    assert all("recipientNames" not in message for message in messages[1:])


@run_async_test
async def test_opening_a_channel_records_the_read_and_cleanup_drops_it():
    runtime = FakeRuntime()
    created = await create_channel(
        runtime, "design", visibility="private", members=["alice", "bob"])
    channel_id = created["data"]["channel"]["id"]

    def reads():
        return {
            row["account_bi"]: row["read_at"]
            for row in runtime.db.execute(
                "SELECT account_bi,read_at FROM chat_channel_reads "
                "WHERE channel_id=?", (channel_id,))
        }

    opened = await api.handle(
        runtime.use("GET", "alice"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    assert opened["status"] == 200
    assert reads() == {"bi:alice": runtime.clock}

    runtime.clock += 5000
    await read_history(runtime, "bob", channel_id)
    runtime.clock -= 10000
    await read_history(runtime, "alice", channel_id)
    assert reads() == {
        "bi:alice": runtime.clock + 10000 - 5000,
        "bi:bob": runtime.clock + 10000,
    }

    removed = await api.handle(
        runtime.use("DELETE", "admin", {"username": "bob"}),
        f"/api/chat/channels/{channel_id}/members",
    )
    assert removed["data"]["removed"] is True
    assert set(reads()) == {"bi:alice"}

    deleted = await api.handle(
        runtime.use("DELETE", "admin"), f"/api/chat/channels/{channel_id}")
    assert deleted["status"] == 200
    assert reads() == {}


def _invite_token(url):
    return url.rsplit("#invite=", 1)[1]


@run_async_test
async def test_invite_link_lets_someone_join_a_private_channel():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}/invite"

    empty = await api.handle(runtime.use("GET", "alice"), path)
    assert empty["data"] == {"invite": None}

    made = await api.handle(runtime.use("POST", "alice", {}), path)
    assert made["status"] == 201
    url = made["data"]["invite"]["url"]
    assert url.startswith("https://forkmesh.test/chat#invite=")
    token = _invite_token(url)
    stored = runtime.db.execute(
        "SELECT token_bi,data FROM chat_channel_invites").fetchone()
    assert token not in stored["token_bi"]

    again = await api.handle(runtime.use("GET", "alice"), path)
    assert again["data"]["invite"]["url"] == url

    invite_path = f"/api/chat/invites/{token}"
    preview = await api.handle(runtime.use("GET", "bob"), invite_path)
    assert preview["status"] == 200
    assert preview["data"]["channel"]["name"] == "release-team"
    assert preview["data"]["invitedBy"] == "alice"
    assert preview["data"]["joined"] is False

    denied = await api.handle(
        runtime.use("GET", "bob"), f"/api/chat/channels/{channel_id}/history")
    assert denied["status"] == 404

    joined = await api.handle(runtime.use("POST", "bob", {}), invite_path)
    assert joined["status"] == 201
    assert joined["data"]["alreadyMember"] is False
    assert joined["data"]["channel"]["id"] == channel_id
    assert ("bob", "private-channels") in runtime.account_events
    assert ("alice", "private-channels") in runtime.account_events
    member = runtime.db.execute(
        "SELECT invited_by_bi FROM chat_channel_members WHERE member_bi=?",
        ("bi:bob",),
    ).fetchone()
    assert member["invited_by_bi"] == "bi:alice"

    access = await api.handle(
        runtime.use("GET", "bob"),
        f"/api/chat/channels/{channel_id}/room-access")
    assert access["status"] == 200

    repeat = await api.handle(runtime.use("POST", "bob", {}), invite_path)
    assert repeat["status"] == 200
    assert repeat["data"]["alreadyMember"] is True


@run_async_test
async def test_invite_links_are_manager_only_and_can_be_reset_or_revoked():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}/invite"
    await api.handle(
        runtime.use("POST", "alice", {"username": "bob"}),
        f"/api/chat/channels/{channel_id}/members",
    )
    for method in ("GET", "POST", "DELETE"):
        refused = await api.handle(runtime.use(method, "bob", {}), path)
        assert refused["status"] == 403
    hidden = await api.handle(runtime.use("POST", "admin", {}), path)
    assert hidden["status"] == 404

    first = _invite_token(
        (await api.handle(runtime.use("POST", "alice", {}), path))
        ["data"]["invite"]["url"])
    second = _invite_token(
        (await api.handle(runtime.use("POST", "alice", {}), path))
        ["data"]["invite"]["url"])
    assert first != second
    stale = await api.handle(
        runtime.use("POST", "admin", {}), f"/api/chat/invites/{first}")
    assert stale["status"] == 404
    assert stale["data"] == {"error": "invite_not_found"}

    revoked = await api.handle(runtime.use("DELETE", "alice", {}), path)
    assert revoked["data"]["removed"] is True
    gone = await api.handle(
        runtime.use("POST", "admin", {}), f"/api/chat/invites/{second}")
    assert gone["status"] == 404


@run_async_test
async def test_invite_needs_a_signed_in_account_and_a_private_room():
    runtime = FakeRuntime()
    public = await create_channel(runtime, "lobby", visibility="public")
    public_id = public["data"]["channel"]["id"]
    refused = await api.handle(
        runtime.use("POST", "admin", {}),
        f"/api/chat/channels/{public_id}/invite")
    assert refused["status"] == 409
    assert refused["data"] == {"error": "private_channel_required"}

    created = await create_channel(runtime, "release-team", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    token = _invite_token(
        (await api.handle(
            runtime.use("POST", "alice", {}),
            f"/api/chat/channels/{channel_id}/invite"))
        ["data"]["invite"]["url"])
    anonymous = await api.handle(
        runtime.use("POST", "", {}), f"/api/chat/invites/{token}")
    assert anonymous["status"] == 401
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chat_channel_members").fetchone()[0] == 0


@run_async_test
async def test_removing_a_member_retires_the_invite_link():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    path = f"/api/chat/channels/{channel_id}/invite"
    token = _invite_token(
        (await api.handle(runtime.use("POST", "alice", {}), path))
        ["data"]["invite"]["url"])
    await api.handle(
        runtime.use("POST", "bob", {}), f"/api/chat/invites/{token}")
    removed = await api.handle(
        runtime.use("DELETE", "alice", {"username": "bob"}),
        f"/api/chat/channels/{channel_id}/members",
    )
    assert removed["data"]["removed"] is True
    assert removed["data"]["inviteReset"] is True
    rejoin = await api.handle(
        runtime.use("POST", "bob", {}), f"/api/chat/invites/{token}")
    assert rejoin["status"] == 404
    assert (await api.handle(runtime.use("GET", "alice"), path))[
        "data"] == {"invite": None}


@run_async_test
async def test_deleting_a_channel_deletes_its_invite_link():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team", actor="alice")
    channel_id = created["data"]["channel"]["id"]
    await api.handle(
        runtime.use("POST", "alice", {}),
        f"/api/chat/channels/{channel_id}/invite")
    await api.handle(
        runtime.use("DELETE", "alice", {}),
        f"/api/chat/channels/{channel_id}")
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chat_channel_invites").fetchone()[0] == 0
