"""Focused policy tests for the server-only organization Discord connector."""

import asyncio
import ast
import base64
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = SRC / "entry.py"
sys.path.insert(0, str(SRC))

spec = importlib.util.spec_from_file_location(
    "forkmesh_organization_discord", SRC / "organization_discord.py")
discord_api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(discord_api)
import organization_rbac  # noqa: E402
from _maintenance_stubs import with_maintenance_off


def run_async_test(function):
    def wrapped(*args, **kwargs):
        return asyncio.run(function(*args, **kwargs))
    return wrapped


def test_message_projection_uses_discord_username_and_safe_avatar_url():
    projected = discord_api._message_projection({
        "id": "300000000000000001",
        "content": "Hello",
        "timestamp": "2026-07-29T18:00:00Z",
        "author": {
            "id": "200000000000000001",
            "username": "account-name",
            "global_name": "Display Name",
            "avatar": "a_avatarHash_123",
            "bot": False,
        },
    }, PUBLIC)
    assert projected["author"] == {
        "name": "Display Name",
        "bot": False,
        "avatarUrl": (
            "https://cdn.discordapp.com/avatars/200000000000000001/"
            "a_avatarHash_123.webp?size=64"
        ),
    }


GUILD = "100000000000000001"
PUBLIC = "200000000000000001"
ANNOUNCEMENT = "200000000000000002"
PRIVATE = "200000000000000003"
PRIVATE_CATEGORY = "200000000000000004"
PRIVATE_CHILD = "200000000000000005"
VOICE = "200000000000000006"


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0038_orgs_teams.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" / "0075_world_office_marketing_tasks.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" / "0105_organization_tasks.sql")
            .read_text(encoding="utf-8"))
        self.db.executescript(
            (ROOT / "migrations" / "0111_organization_discord_connector.sql")
            .read_text(encoding="utf-8"))
        self.db.execute(
            "INSERT INTO orgs(org_bi,name,data,created_at) VALUES (?,?,?,?)",
            ("org-bi", "forkmesh", "sealed:org", 1),
        )
        self.db.executemany(
            "INSERT INTO org_members"
            "(org_bi,member_bi,role,name,created_at) VALUES (?,?,?,?,?)",
            [
                ("org-bi", "account-alice", "owner", "alice", 1),
                ("org-bi", "account-ada", "admin", "ada", 1),
                ("org-bi", "account-bob", "member", "bob", 1),
            ],
        )
        self.db.commit()
        self.request_method = "GET"
        self.request_data = {}
        self.query_data = {}
        self.actor = ""
        self.same_origin_request = True
        self.secret_configured = False
        self.oauth_configured = False
        self.guild_result_status = 200
        self.channel_result_status = 200
        self.message_result_status = 200
        self.send_result_status = 201
        self.oauth_guilds = [{
            "id": GUILD,
            "owner": False,
            "permissions": str(
                discord_api.ADMINISTRATOR_PERMISSION
                | discord_api.MANAGE_GUILD_PERMISSION),
        }]
        self.oauth_user = {"id": "400000000000000001"}
        self.oauth_transaction = ""
        self.oauth_cookies = []
        self.active_sessions = {"account-alice": "session-alice"}
        self.oauth_exchange_status = 200
        self.oauth_identity_status = 200
        self.oauth_guilds_status = 200
        self.channels = [
            {"id": PUBLIC, "name": "general", "type": 0},
            {"id": ANNOUNCEMENT, "name": "announcements", "type": 5},
            {
                "id": PRIVATE,
                "name": "private",
                "type": 0,
                "permission_overwrites": [
                    {"id": GUILD, "type": 0, "deny": "1024"},
                ],
            },
            {
                "id": PRIVATE_CATEGORY,
                "name": "staff",
                "type": 4,
                "permission_overwrites": [
                    {"id": GUILD, "type": 0, "deny": "1024"},
                ],
            },
            {
                "id": PRIVATE_CHILD,
                "name": "staff-chat",
                "type": 0,
                "parent_id": PRIVATE_CATEGORY,
                # Discord stores the synchronized category overwrite on the
                # child too; category permissions are copied, not inherited.
                "permission_overwrites": [
                    {"id": GUILD, "type": 0, "deny": "1024"},
                ],
            },
            {"id": VOICE, "name": "voice", "type": 2},
            {"id": "200000000000000007", "name": "nsfw", "type": 0,
             "nsfw": True},
            {"id": "200000000000000008", "name": "thread", "type": 11},
        ]
        self.roles = [{
            "id": GUILD,
            "permissions": str(
                discord_api.VIEW_CHANNEL_PERMISSION
                | discord_api.READ_MESSAGE_HISTORY_PERMISSION),
        }]
        self.public_channel_cache = {}
        self.remote_messages = [
            {
                "id": "300000000000000001",
                "content": "Hello from Discord",
                "timestamp": "2026-07-29T18:00:00.000000+00:00",
                "edited_timestamp": None,
                "author": {"username": "discord-user", "bot": False},
            },
        ]
        self.sent = []
        self.bot_channel_calls = 0
        self.guild_result_status = 200
        self.bot_role_calls = 0
        self.audits = []
        self.ids = 0
        self.memberships = {
            "alice": "owner",
            "ada": "admin",
            "bob": "member",
        }

    def use(self, method, actor="", data=None, same_origin=True, query=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        self.same_origin_request = same_origin
        self.query_data = dict(query or {})
        return self

    def method(self):
        return self.request_method

    def now(self):
        return 2_100_000_000_000

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def query(self, name):
        return self.query_data.get(name, "")

    def same_origin(self):
        return self.same_origin_request

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cache_control": cache_control,
            "headers": dict(extra_headers or {}),
        }

    def oauth_start_response(self, data, transaction):
        response = self.response(data, cache_control="no-store")
        response["oauthTransaction"] = transaction
        return response

    def oauth_callback_response(self, outcome):
        self.oauth_transaction = ""
        return {
            "status": 303,
            "data": {"outcome": outcome},
            "headers": {
                "location": "/world/?discord=" + outcome,
                "referrer-policy": "no-referrer",
            },
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return (
            (self.request_data, "")
            if isinstance(self.request_data, dict)
            else (None, "invalid_json")
        )

    async def session(self, _data):
        if self.actor not in self.memberships:
            return "", None
        return "account-" + self.actor, {"name": self.actor}

    async def session_id(self):
        return self.active_sessions.get("account-" + self.actor, "")

    async def session_active(self, account_bi, session_id):
        return self.active_sessions.get(account_bi) == session_id

    async def organization(self, org):
        if org != "forkmesh":
            return "org-bi", None
        return "org-bi", {"name": "forkmesh"}

    async def org_role(self, _org_bi, account):
        return self.memberships.get(account, "")

    async def seal(self, value):
        return "sealed:" + base64.urlsafe_b64encode(
            json.dumps(value, sort_keys=True).encode()).decode()

    async def open(self, value):
        if not str(value or "").startswith("sealed:"):
            return None
        return json.loads(base64.urlsafe_b64decode(
            str(value)[7:].encode()).decode())

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.audits.append({
            "actor": actor,
            "action": action,
            "targetType": target_type,
            "target": target,
            "outcome": outcome,
            "details": dict(details or {}),
        })

    def discord_ready(self):
        return self.secret_configured

    def discord_oauth_ready(self):
        return self.oauth_configured

    def discord_oauth_redirect_uri(self):
        return "https://forkmesh.test/api/integrations/discord/callback"

    def discord_oauth_authorization_url(self, state, challenge):
        return (
            "https://discord.test/oauth2/authorize?state=" + state
            + "&code_challenge=" + challenge
            + "&scope=identify%20guilds")

    def oauth_transaction_cookie(self, state=""):
        jar = [value for value in [self.oauth_transaction, *self.oauth_cookies]
               if value]
        if state:
            return state if state in jar else ""
        return jar[0] if len(jar) == 1 else ""

    async def discord_guild(self, guild_id):
        if guild_id != GUILD:
            return {"status": 404, "data": None, "retryAfterMs": 0}
        return {
            "status": self.guild_result_status,
            "data": {
                "id": GUILD, "name": "ForkMesh <Community>",
                "description": "Git without the middleman",
                "vanity_url_code": "forkmesh",
                "approximate_member_count": 412,
                "approximate_presence_count": 57,
            },
            "retryAfterMs": 0,
        }

    async def discord_channels(self, guild_id):
        self.bot_channel_calls += 1
        if guild_id != GUILD:
            return {"status": 404, "data": None, "retryAfterMs": 0}
        return {
            "status": self.channel_result_status,
            "data": list(self.channels),
            "retryAfterMs": 250,
        }

    def discord_public_channels_cache(self, guild_id):
        return self.public_channel_cache.get(guild_id)

    def cache_discord_public_channels(self, guild_id, channels):
        self.public_channel_cache[guild_id] = list(channels)

    async def discord_guild_roles(self, guild_id):
        self.bot_role_calls += 1
        if guild_id != GUILD:
            return {"status": 404, "data": None, "retryAfterMs": 0}
        return {
            "status": self.channel_result_status,
            "data": list(self.roles),
            "retryAfterMs": 250,
        }

    async def discord_oauth_exchange(self, _code, _verifier):
        return {
            "status": self.oauth_exchange_status,
            "accessToken": (
                "transient-oauth-bearer" if self.oauth_exchange_status == 200
                else ""),
        }

    async def discord_oauth_identity(self, _access_token):
        return {
            "status": self.oauth_identity_status,
            "data": dict(self.oauth_user),
        }

    async def discord_oauth_guilds(self, _access_token):
        return {
            "status": self.oauth_guilds_status,
            "data": list(self.oauth_guilds),
        }

    async def discord_messages(self, channel_id, _limit):
        if channel_id not in {PUBLIC, ANNOUNCEMENT}:
            return {"status": 403, "data": None, "retryAfterMs": 0}
        return {
            "status": self.message_result_status,
            "data": list(self.remote_messages),
            "retryAfterMs": 250,
        }

    async def discord_send(self, channel_id, content):
        # Match the real adapter's non-pinging Discord payload; the policy
        # module can only supply selected channel id + sanitized text.
        payload = {
            "content": content,
            "allowed_mentions": {"parse": []},
        }
        self.sent.append({"channelId": channel_id, "payload": payload})
        if self.send_result_status not in {200, 201}:
            return {
                "status": self.send_result_status,
                "data": None,
                "retryAfterMs": 250,
            }
        return {
            "status": self.send_result_status,
            "data": {
                "id": "300000000000000002",
                "content": content,
                "timestamp": "2026-07-29T18:01:00.000000+00:00",
                "author": {"username": "forkmesh-bot", "bot": True},
            },
            "retryAfterMs": 0,
        }


async def authorize_guild(runtime, guild_id=GUILD):
    runtime.oauth_configured = True
    started = await discord_api.handle(
        runtime.use("POST", "alice", {"guildId": guild_id}),
        "forkmesh", "oauth/start")
    assert started["status"] == 200
    params = parse_qs(urlparse(started["data"]["authorizationUrl"]).query)
    state = params["state"][0]
    runtime.oauth_transaction = started["oauthTransaction"]
    completed = await discord_api.handle_oauth_callback(
        runtime.use("GET", "", query={
            "state": state,
            "code": "oauth-authorization-code-123456",
        }))
    assert completed["status"] == 303
    assert completed["data"]["outcome"] == "connected"
    return started, completed


async def configure(runtime, channel_ids=None):
    runtime.secret_configured = True
    await authorize_guild(runtime)
    return await discord_api.handle(
        runtime.use("PUT", "alice", {
            "guildId": GUILD,
            "channelIds": channel_ids or [PUBLIC],
        }),
        "forkmesh",
    )


@run_async_test
async def test_secret_missing_returns_explicit_human_setup_without_credential():
    runtime = FakeRuntime()
    response = await discord_api.handle(
        runtime.use("GET", "alice"), "forkmesh")
    assert response["status"] == 409
    payload = response["data"]
    assert payload["state"] == "oauth_client_required"
    assert payload["humanTask"]["title"] == (
        "Configure Discord OAuth for this ForkMesh deployment")
    assert "DISCORD_CLIENT_SECRET" in json.dumps(payload)
    assert "DISCORD_BOT_TOKEN" not in json.dumps(payload)
    assert "fmbot_" not in json.dumps(payload)
    assert "tokenValue" not in payload
    task_id = payload["humanTask"]["organizationTaskId"]
    task = runtime.db.execute(
        "SELECT task_id,assignee_kind,assignee_bi,data,completed_at "
        "FROM organization_tasks WHERE task_id=?", (task_id,)
    ).fetchone()
    assert task["assignee_kind"] == "unassigned"
    assert task["assignee_bi"] == ""
    assert task["completed_at"] == 0
    assert "Configure Discord connector" not in task["data"]
    repeated = await discord_api.handle(
        runtime.use("GET", "alice"), "forkmesh")
    assert repeated["data"]["humanTask"]["organizationTaskId"] == task_id
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM organization_discord_setup_tasks"
    ).fetchone()["n"] == 1
    await authorize_guild(runtime)
    completed = await discord_api.handle(
        runtime.use("GET", "alice"), "forkmesh")
    assert completed["status"] == 200
    assert completed["data"]["state"] == "secret_required"
    pending = runtime.db.execute(
        "SELECT completed_at FROM organization_tasks WHERE task_id=?", (task_id,)
    ).fetchone()
    assert pending["completed_at"] == 0
    denied = await discord_api.handle(
        runtime.use("PUT", "bob", {
            "guildId": GUILD,
            "channelIds": [PUBLIC],
        }), "forkmesh")
    assert denied["status"] == 403
    assert denied["data"]["error"] == "owner_required"
    csrf = await discord_api.handle(
        runtime.use("PUT", "alice", {
            "guildId": GUILD,
            "channelIds": [PUBLIC],
        }, same_origin=False), "forkmesh")
    assert csrf["status"] == 403
    assert csrf["data"]["error"] == "origin_not_allowed"


@run_async_test
async def test_oauth_grant_is_required_before_any_bot_guild_discovery():
    runtime = FakeRuntime()
    runtime.secret_configured = True
    runtime.oauth_configured = True
    response = await discord_api.handle(
        runtime.use("GET", "alice"), "forkmesh")
    assert response["status"] == 409
    payload = response["data"]
    assert payload["state"] == "guild_authorization_required"
    task = payload["humanTask"]
    assert task["title"] == "Authorize this organization’s Discord guild"
    assert "Manage Server" in json.dumps(task)
    assert not runtime.sent
    assert runtime.bot_channel_calls == 0
    assert runtime.bot_role_calls == 0


@run_async_test
async def test_oauth_callback_is_one_time_and_requires_guild_permission():
    runtime = FakeRuntime()
    runtime.oauth_configured = True
    runtime.oauth_guilds = [{"id": GUILD, "owner": False, "permissions": "0"}]
    started = await discord_api.handle(
        runtime.use("POST", "alice", {"guildId": GUILD}),
        "forkmesh", "oauth/start")
    assert started["status"] == 200
    params = parse_qs(urlparse(started["data"]["authorizationUrl"]).query)
    state = params["state"][0]
    assert params["scope"] == ["identify guilds"]
    row = runtime.db.execute(
        "SELECT data FROM organization_discord_oauth_states"
    ).fetchone()
    assert state not in row["data"]
    runtime.oauth_transaction = started["oauthTransaction"]
    denied = await discord_api.handle_oauth_callback(
        runtime.use("GET", query={
            "state": state, "code": "oauth-authorization-code-123456",
        }))
    assert denied["data"]["outcome"] == "denied"
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM organization_discord_oauth_states"
    ).fetchone()["n"] == 0
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM organization_discord_oauth_grants"
    ).fetchone()["n"] == 0
    replay = await discord_api.handle_oauth_callback(
        runtime.use("GET", query={
            "state": state, "code": "oauth-authorization-code-123456",
        }))
    assert replay["data"]["outcome"] == "invalid_state_missing"
    assert runtime.bot_channel_calls == 0
    assert runtime.bot_role_calls == 0


@run_async_test
async def test_oauth_callback_accepts_browser_that_omits_transaction_cookie():
    runtime = FakeRuntime()
    runtime.oauth_configured = True
    started = await discord_api.handle(
        runtime.use("POST", "alice", {"guildId": GUILD}),
        "forkmesh", "oauth/start")
    state = parse_qs(
        urlparse(started["data"]["authorizationUrl"]).query)["state"][0]
    runtime.oauth_transaction = ""
    refused = await discord_api.handle_oauth_callback(
        runtime.use("GET", query={
            "state": state, "code": "oauth-authorization-code-123456",
        }))
    assert refused["data"]["outcome"] == "invalid_context"
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM organization_discord_oauth_grants"
    ).fetchone()["n"] == 0
    panel = (ROOT.parent / "world" / "public" / "world" /
             "world-discord-panel.js").read_text(encoding="utf-8")
    message = panel.split("invalid_context:", 1)[1].split("\n", 1)[0]
    assert "Connect Discord server" in message and "again" in message


@run_async_test
async def test_oauth_callback_uses_secure_cookie_when_provider_state_is_malformed():
    runtime = FakeRuntime()
    runtime.oauth_configured = True
    started = await discord_api.handle(
        runtime.use("POST", "alice", {"guildId": GUILD}),
        "forkmesh", "oauth/start")
    state = parse_qs(
        urlparse(started["data"]["authorizationUrl"]).query)["state"][0]
    assert started["oauthTransaction"] == state
    runtime.oauth_transaction = started["oauthTransaction"]
    completed = await discord_api.handle_oauth_callback(
        runtime.use("GET", query={
            "state": "provider-returned-a-malformed-state",
            "code": "oauth-authorization-code-123456",
        }))
    assert completed["data"]["outcome"] == "connected"
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM organization_discord_oauth_grants"
    ).fetchone()["n"] == 1


@run_async_test
async def test_oauth_callback_tolerates_replaced_transaction_cookie():
    runtime = FakeRuntime()
    runtime.oauth_configured = True
    started = await discord_api.handle(
        runtime.use("POST", "alice", {"guildId": GUILD}),
        "forkmesh", "oauth/start")
    state = parse_qs(
        urlparse(started["data"]["authorizationUrl"]).query)["state"][0]
    runtime.oauth_transaction = "f" * 64
    refused = await discord_api.handle_oauth_callback(
        runtime.use("GET", query={
            "state": state, "code": "oauth-authorization-code-123456",
        }))
    assert refused["data"]["outcome"] == "invalid_context"
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM organization_discord_oauth_grants"
    ).fetchone()["n"] == 0


@run_async_test
async def test_oauth_callback_finishes_every_flow_started_in_this_browser():
    runtime = FakeRuntime()
    runtime.oauth_configured = True
    states = []
    for _ in range(2):
        started = await discord_api.handle(
            runtime.use("POST", "alice", {"guildId": GUILD}),
            "forkmesh", "oauth/start")
        states.append(parse_qs(
            urlparse(started["data"]["authorizationUrl"]).query)["state"][0])
        runtime.oauth_cookies.append(started["oauthTransaction"])
    for state in reversed(states):
        completed = await discord_api.handle_oauth_callback(
            runtime.use("GET", query={
                "state": state, "code": "oauth-authorization-code-123456",
            }))
        assert completed["data"]["outcome"] == "connected"


@run_async_test
async def test_oauth_callback_from_another_browser_is_refused():
    runtime = FakeRuntime()
    runtime.oauth_configured = True
    runtime.memberships["victim"] = "member"
    runtime.active_sessions["account-victim"] = "session-victim"
    for transaction in ("", "f" * 64):
        for actor in ("", "victim"):
            started = await discord_api.handle(
                runtime.use("POST", "alice", {"guildId": GUILD}),
                "forkmesh", "oauth/start")
            state = parse_qs(urlparse(
                started["data"]["authorizationUrl"]).query)["state"][0]
            runtime.oauth_transaction = transaction
            refused = await discord_api.handle_oauth_callback(
                runtime.use("GET", actor, query={
                    "state": state, "code": "oauth-authorization-code-123456",
                }))
            assert refused["data"]["outcome"] == "invalid_context"
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM organization_discord_oauth_grants"
    ).fetchone()["n"] == 0


@run_async_test
async def test_oauth_callback_consumes_state_if_oauth_settings_change_mid_flow():
    runtime = FakeRuntime()
    runtime.oauth_configured = True
    started = await discord_api.handle(
        runtime.use("POST", "alice", {"guildId": GUILD}),
        "forkmesh", "oauth/start")
    state = parse_qs(urlparse(started["data"]["authorizationUrl"]).query)["state"][0]
    runtime.oauth_transaction = started["oauthTransaction"]
    # A deployment can rotate/remove the OAuth client configuration between
    # the redirect and callback. The callback must still burn the one-time
    # state before telling the browser that setup is required.
    runtime.oauth_configured = False
    response = await discord_api.handle_oauth_callback(
        runtime.use("GET", query={
            "state": state,
            "code": "oauth-authorization-code-123456",
        }))
    assert response["data"]["outcome"] == "setup"
    assert runtime.db.execute(
        "SELECT COUNT(*) AS n FROM organization_discord_oauth_states"
    ).fetchone()["n"] == 0


@run_async_test
async def test_discord_rate_error_sets_a_standard_retry_after_header():
    runtime = FakeRuntime()
    response = await discord_api._discord_error(runtime, {
        "status": 429,
        "retryAfterMs": 1_501,
    })
    assert response["status"] == 429
    assert response["data"]["retryAfterMs"] == 1_501
    assert response["headers"]["retry-after"] == "2"


@run_async_test
async def test_discord_provider_failures_are_marked_as_expected_degradation():
    runtime = FakeRuntime()
    unavailable = await discord_api._discord_error(runtime, {"status": 502})
    assert unavailable["status"] == 502
    assert unavailable["headers"]["x-forkmesh-expected-degraded"] == "1"

    unauthorized = await discord_api._discord_error(runtime, {"status": 401})
    assert unauthorized["status"] == 503
    assert unauthorized["headers"]["x-forkmesh-expected-degraded"] == "1"


@run_async_test
async def test_owner_configures_only_installed_public_text_channels_and_data_is_sealed():
    runtime = FakeRuntime()
    response = await configure(runtime, [PUBLIC, ANNOUNCEMENT])
    assert response["status"] == 200
    assert response["data"]["connector"] == {
        "guildId": GUILD,
        "channelIds": [PUBLIC, ANNOUNCEMENT],
    }
    row = runtime.db.execute(
        "SELECT data,updated_by_bi FROM organization_discord_connectors"
    ).fetchone()
    assert row["updated_by_bi"] == "account-alice"
    assert row["data"].startswith("sealed:")
    assert GUILD not in row["data"]
    assert PUBLIC not in row["data"]
    assert "DISCORD_BOT_TOKEN" not in row["data"]
    owner_view = await discord_api.handle(
        runtime.use("GET", "alice", query={"guildId": GUILD}), "forkmesh")
    assert owner_view["status"] == 200
    assert owner_view["data"]["state"] == "configured"
    assert {item["id"] for item in owner_view["data"]["channels"]} == {
        PUBLIC, ANNOUNCEMENT,
    }
    member_view = await discord_api.handle(
        runtime.use("GET", "bob"), "forkmesh")
    assert member_view["status"] == 200
    assert "guilds" not in member_view["data"]
    assert "channels" not in member_view["data"]


@run_async_test
async def test_private_category_children_voice_and_unknown_fields_are_rejected():
    runtime = FakeRuntime()
    runtime.secret_configured = True
    await authorize_guild(runtime)
    for channel_id in (
        PRIVATE, PRIVATE_CHILD, VOICE,
        "200000000000000007", "200000000000000008",
    ):
        response = await discord_api.handle(
            runtime.use("PUT", "alice", {
                "guildId": GUILD,
                "channelIds": [channel_id],
            }), "forkmesh")
        assert response["status"] == 400
        assert response["data"]["error"] == "selected_channel_not_public"
    unexpected = await discord_api.handle(
        runtime.use("PUT", "alice", {
            "guildId": GUILD,
            "channelIds": [PUBLIC],
            "token": "do-not-store-me",
        }), "forkmesh")
    assert unexpected["status"] == 400
    assert unexpected["data"]["error"] == "unsupported_configuration_field"
    unavailable = await discord_api.handle(
        runtime.use("PUT", "alice", {
            "guildId": "100000000000000099",
            "channelIds": [PUBLIC],
        }), "forkmesh")
    assert unavailable["status"] == 409
    assert unavailable["data"]["state"] == "guild_authorization_required"


def test_desynchronized_children_and_missing_parents_fail_closed():
    """Category permission copies must not be mistaken for live inheritance."""

    category_id = "200000000000000020"
    child_id = "200000000000000021"
    missing_parent_child_id = "200000000000000022"
    channels = [
        {
            "id": category_id,
            "name": "public-category",
            "type": 4,
            "permission_overwrites": [{
                "id": GUILD,
                "type": 0,
                "allow": str(discord_api.VIEW_CHANNEL_PERMISSION),
            }],
        },
        {
            "id": child_id,
            "name": "desynchronized-private-child",
            "type": 0,
            "parent_id": category_id,
        },
        {
            "id": missing_parent_child_id,
            "name": "partial-payload-child",
            "type": 0,
            "parent_id": "200000000000000099",
        },
    ]
    roles = [{
        "id": GUILD,
        "permissions": str(discord_api.READ_MESSAGE_HISTORY_PERMISSION),
    }]
    assert discord_api._public_channels(channels, roles, GUILD) == []


@run_async_test
async def test_guild_summary_reads_counts_only_after_an_owner_grant():
    runtime = FakeRuntime()
    assert await discord_api.guild_summary(runtime, "org-bi") == {
        "ok": False, "error": "discord_connector_not_connected"}
    await authorize_guild(runtime)
    assert await discord_api.guild_summary(runtime, "org-bi") == {
        "ok": False, "error": "discord_secret_required"}
    runtime.secret_configured = True
    assert await discord_api.guild_summary(runtime, "org-bi") == {
        "ok": True, "guildId": GUILD, "name": "ForkMesh Community",
        "description": "Git without the middleman", "vanityCode": "forkmesh",
        "memberCount": 412, "presenceCount": 57}
    for status, error in ((403, "discord_authorization_required"),
                          (429, "discord_rate_limited"),
                          (500, "discord_unavailable")):
        runtime.guild_result_status = status
        assert await discord_api.guild_summary(runtime, "org-bi") == {
            "ok": False, "error": error}
    runtime.db.execute(
        "UPDATE org_members SET role='admin' WHERE org_bi=? AND member_bi=?",
        ("org-bi", "account-alice"))
    runtime.db.commit()
    runtime.guild_result_status = 200
    assert await discord_api.guild_summary(runtime, "org-bi") == {
        "ok": False, "error": "discord_connector_not_connected"}


def _message_at(ms, suffix=0):
    return str(((ms - discord_api.DISCORD_EPOCH_MS) << 22) + suffix)


@run_async_test
async def test_recent_messages_read_the_selected_public_channels_for_six_days():
    runtime = FakeRuntime()
    assert await discord_api.recent_messages(runtime, "org-bi", 6) == {
        "ok": False, "error": "discord_connector_not_connected"}
    await authorize_guild(runtime)
    assert await discord_api.recent_messages(runtime, "org-bi", 6) == {
        "ok": False, "error": "discord_secret_required"}
    runtime.secret_configured = True
    assert await discord_api.recent_messages(runtime, "org-bi", 6) == {
        "ok": False, "error": "discord_channel_required"}
    assert (await configure(runtime, [PUBLIC, ANNOUNCEMENT]))["status"] == 200
    day = 86_400_000
    now = runtime.now()
    runtime.remote_messages = [
        {"id": _message_at(now - day), "content": "Fresh <b>news</b>",
         "timestamp": "t", "author": {"username": "ada", "bot": False}},
        {"id": _message_at(now - 5 * day), "content": "Five days ago",
         "timestamp": "t", "author": {"username": "forkmesh-bot", "bot": True}},
        {"id": _message_at(now - 7 * day), "content": "Too old",
         "timestamp": "t", "author": {"username": "ada"}},
    ]
    recent = await discord_api.recent_messages(runtime, "org-bi", 6)
    assert recent["ok"] is True and recent["days"] == 6
    assert recent["since"] == now - 6 * day
    assert recent["channels"] == [{"id": PUBLIC, "name": "general"},
                                  {"id": ANNOUNCEMENT, "name": "announcements"}]
    assert len(recent["messages"]) == 4
    assert [item["content"] for item in recent["messages"]] == [
        "Fresh  b news /b", "Fresh  b news /b", "Five days ago", "Five days ago"]
    first = recent["messages"][0]
    assert first["postedAt"] == now - day
    assert first["url"] == (
        "https://discord.com/channels/" + GUILD + "/" + first["channelId"]
        + "/" + first["id"])
    assert recent["truncated"] is False and recent["contentMissing"] is False
    assert (await discord_api.recent_messages(runtime, "org-bi", 99))["days"] == 14
    runtime.message_result_status = 429
    assert await discord_api.recent_messages(runtime, "org-bi", 6) == {
        "ok": False, "error": "discord_rate_limited"}


@run_async_test
async def test_former_owner_grant_is_revoked_before_any_discord_call():
    runtime = FakeRuntime()
    assert (await configure(runtime))["status"] == 200
    before_channels = runtime.bot_channel_calls
    before_roles = runtime.bot_role_calls
    runtime.db.execute(
        "UPDATE org_members SET role='admin' "
        "WHERE org_bi=? AND member_bi=?",
        ("org-bi", "account-alice"),
    )
    runtime.db.commit()

    response = await discord_api.handle(
        runtime.use("GET", "bob", query={"channelId": PUBLIC}),
        "forkmesh", "messages")

    assert response["status"] == 409
    assert response["data"]["state"] == "guild_authorization_required"
    assert runtime.bot_channel_calls == before_channels
    assert runtime.bot_role_calls == before_roles
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_discord_oauth_grants"
    ).fetchone()[0] == 0
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM organization_discord_connectors"
    ).fetchone()[0] == 0


@run_async_test
async def test_read_and_send_recheck_public_permissions_without_stale_cache():
    runtime = FakeRuntime()
    assert (await configure(runtime))["status"] == 200
    assert runtime.public_channel_cache[GUILD]
    runtime.channels[0]["permission_overwrites"] = [{
        "id": GUILD,
        "type": 0,
        "deny": str(discord_api.VIEW_CHANNEL_PERMISSION),
    }]

    viewed = await discord_api.handle(
        runtime.use("GET", "bob", query={"channelId": PUBLIC}),
        "forkmesh", "messages")
    sent = await discord_api.handle(
        runtime.use("POST", "ada", {
            "channelId": PUBLIC,
            "content": "must not escape after permission revocation",
        }), "forkmesh", "messages")

    assert viewed["status"] == 409
    assert viewed["data"]["error"] == "selected_channel_unavailable"
    assert sent["status"] == 409
    assert sent["data"]["error"] == "selected_channel_unavailable"
    assert runtime.sent == []


@run_async_test
async def test_members_read_selected_channels_while_admins_send_non_pinging_text_only():
    runtime = FakeRuntime()
    assert (await configure(runtime))["status"] == 200
    viewed = await discord_api.handle(
        runtime.use("GET", "bob", query={"channelId": PUBLIC}),
        "forkmesh", "messages")
    assert viewed["status"] == 200
    assert viewed["data"]["messages"][0]["author"] == {
        "name": "discord-user", "bot": False,
    }
    assert viewed["data"]["channel"] == {
        "id": PUBLIC, "name": "general",
    }
    assert "guilds" not in viewed["data"]
    denied = await discord_api.handle(
        runtime.use("POST", "bob", {
            "channelId": PUBLIC,
            "content": "members cannot send",
        }), "forkmesh", "messages")
    assert denied["status"] == 403
    created = await discord_api.handle(
        runtime.use("POST", "ada", {
            "sessionToken": "compatibility-marker-only",
            "channelId": PUBLIC,
            "content": "hello <@123>\nplease review",
        }), "forkmesh", "messages")
    assert created["status"] == 201
    assert runtime.sent[-1]["channelId"] == PUBLIC
    assert runtime.sent[-1]["payload"]["allowed_mentions"] == {"parse": []}
    assert "<" not in runtime.sent[-1]["payload"]["content"]
    assert "sessionToken" not in json.dumps(runtime.audits[-1])
    assert "hello" not in json.dumps(runtime.audits[-1])
    not_selected = await discord_api.handle(
        runtime.use("POST", "alice", {
            "channelId": ANNOUNCEMENT,
            "content": "not selected",
        }), "forkmesh", "messages")
    assert not_selected["status"] == 403
    assert not_selected["data"]["error"] == "channel_not_selected"


@run_async_test
async def test_blank_discord_messages_expose_intent_diagnostic_and_reopen_one_task():
    runtime = FakeRuntime()
    assert (await configure(runtime))["status"] == 200
    runtime.remote_messages = [{
        "id": "300000000000000003",
        "content": "",
        "timestamp": "2026-07-29T18:00:00.000000+00:00",
        "author": {"username": "discord-user", "bot": False},
    }]
    response = await discord_api.handle(
        runtime.use("GET", "alice", query={"channelId": PUBLIC}),
        "forkmesh", "messages")
    assert response["status"] == 200
    diagnostic = response["data"]["messageContentDiagnostic"]
    assert diagnostic["state"] == "message_content_required"
    assert "Message Content privileged intent" in json.dumps(
        diagnostic["humanTask"])
    task_id = diagnostic["humanTask"]["organizationTaskId"]
    task = runtime.db.execute(
        "SELECT completed_at,data FROM organization_tasks WHERE task_id=?",
        (task_id,),
    ).fetchone()
    assert task["completed_at"] == 0
    assert "Enable Discord message content access" not in task["data"]


def test_route_schema_and_worker_adapter_keep_the_secret_server_side():
    urls_source = (SRC / "urls.py").read_text(encoding="utf-8")
    entry_source = (SRC / "entry.py").read_text(encoding="utf-8")
    provider_runtime_source = (SRC / "provider_runtimes.py").read_text(
        encoding="utf-8")
    schema_source = (SRC / "schema.py").read_text(encoding="utf-8")
    migration_source = (ROOT / "migrations" /
                        "0111_organization_discord_connector.sql").read_text(
                            encoding="utf-8")
    assert "ORG_DISCORD_RE" in urls_source
    assert "/api/orgs/([^/]+)/discord" in urls_source
    assert "organization_discord_handler" in entry_source
    assert "getattr(env, \"DISCORD_BOT_TOKEN\", \"\")" in entry_source
    assert "\"allowed_mentions\": {\"parse\": []}" in provider_runtime_source
    assert "https://discord.com/api/v10" in entry_source
    bot_paths = entry_source[
        entry_source.index("def _discord_api_path_allowed"):
        entry_source.index("def _discord_oauth_path_allowed")]
    assert "/users/@me/guilds" not in bot_paths
    assert "_DISCORD_GUILD_PATH_RE.fullmatch(path)" in bot_paths
    assert r'"^/guilds/[0-9]{17,20}\?with_counts=true$"' in entry_source
    plugin_runtime = entry_source[
        entry_source.index("class _PluginApiRuntime"):
        entry_source.index("class _PluginLifecycleRuntime")]
    guild_capability = plugin_runtime[
        plugin_runtime.index("async def discord_guild"):
        plugin_runtime.index("def mailtrap_configured")]
    assert "PLUGIN_DISCORD_CONNECTOR_PERMISSION not in plugin_data.permissions" in (
        guild_capability)
    assert "self.plugin_installed(org_bi, self._plugin)" in guild_capability
    assert "organization_discord.guild_summary(" in guild_capability
    messages_capability = plugin_runtime[
        plugin_runtime.index("async def discord_recent_messages"):
        plugin_runtime.index("def mailtrap_configured")]
    assert "PLUGIN_DISCORD_MESSAGES_PERMISSION not in plugin_data.permissions" in (
        messages_capability)
    assert "self.plugin_installed(org_bi, self._plugin)" in messages_capability
    assert "organization_discord.recent_messages(" in messages_capability
    assert "_DISCORD_OAUTH_GUILDS_PATH" in entry_source
    assert "DISCORD_OAUTH_CALLBACK_RE" in urls_source
    assert "organization_discord_oauth_callback_handler" in entry_source
    discord_runtime = provider_runtime_source[
        provider_runtime_source.index("class _OrganizationDiscordRuntime"):
        provider_runtime_source.index("class _AccountPlanRuntime")]
    assert "parse_qs(" in discord_runtime
    assert "urlparse(str(self.request.url)).query" in discord_runtime
    assert "URL(self.request.url).searchParams" not in discord_runtime
    assert "oauth/start" in urls_source
    assert "SameSite=Lax" in entry_source
    assert "code_challenge_method" in provider_runtime_source
    assert "discord_guilds" not in entry_source
    assert "organization_discord_connectors" in schema_source
    assert "organization_discord_connectors" in migration_source
    assert "organization_discord_setup_tasks" in migration_source
    assert "organization_discord_oauth_grants" in migration_source
    assert "organization_discord_oauth_states" in migration_source
    assert "DELETE FROM organization_discord_connectors WHERE org_bi=?" in entry_source
    assert "SELECT task_id FROM organization_discord_setup_tasks" in entry_source
    assert "organization_task_checkins" in entry_source
    assert "organization_task_qa_reviews" in entry_source
    assert "organization_task_responses" in entry_source
    assert "organization_task_attachments" in entry_source
    assert "DELETE FROM organization_discord_setup_tasks WHERE org_bi=?" in entry_source
    assert "DELETE FROM organization_discord_oauth_grants WHERE org_bi=?" in entry_source
    assert "DELETE FROM organization_discord_oauth_states WHERE org_bi=?" in entry_source
    assert "DISCORD_BOT_TOKEN=" not in entry_source
    assert "DELETE FROM organization_discord_oauth_states " in (
        SRC / "organization_discord.py").read_text(encoding="utf-8")
    assert "RETURNING data,expires_at" not in (
        SRC / "organization_discord.py").read_text(encoding="utf-8")


def test_discord_oauth_redirect_is_bound_to_the_canonical_public_origin():
    tree = ast.parse((
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
), filename=str(ENTRY))
    function_node = next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_discord_oauth_config")
    namespace = {
        "urlparse": urlparse,
        "_discord_snowflake": lambda value: str(value),
        "DISCORD_OAUTH_CALLBACK_PATH": (
            "/api/integrations/discord/callback"),
    }
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[function_node], type_ignores=[])), str(ENTRY), "exec"), with_maintenance_off(namespace))
    config = namespace["_discord_oauth_config"]
    base = {
        "DISCORD_CLIENT_ID": GUILD,
        "DISCORD_CLIENT_SECRET": "server-only-secret",
        "PUBLIC_BASE_URL": "https://forkmesh.test",
    }
    accepted = config(SimpleNamespace(
        **base,
        DISCORD_OAUTH_REDIRECT_URI=(
            "https://forkmesh.test/api/integrations/discord/callback"),
    ))
    assert accepted["redirectUri"].startswith("https://forkmesh.test/")
    assert config(SimpleNamespace(
        **base,
        DISCORD_OAUTH_REDIRECT_URI=(
            "https://attacker.test/api/integrations/discord/callback"),
    )) is None
    assert config(SimpleNamespace(
        **base,
        DISCORD_OAUTH_REDIRECT_URI=(
            "https://forkmesh.test:444/api/integrations/discord/callback"),
    )) is None


@run_async_test
async def test_durable_rate_gate_reserves_and_persists_metadata_only():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    class_node = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef)
        and node.name == "ForkMeshDiscordGate")
    calls = []

    class Storage:
        def __init__(self):
            self.values = {}

        async def get(self, key):
            return self.values.get(key)

        async def put(self, key, value):
            self.values[key] = value

    class Request:
        method = "POST"
        url = "https://forkmesh.internal/discord-gate/request"

        async def text(self):
            return json.dumps({
                "method": "POST",
                "path": f"/channels/{PUBLIC}/messages",
                "body": {"content": "private-message-marker"},
            })

    async def provider(_env, method, path, body):
        calls.append((method, path, body))
        return {
            "status": 429,
            "data": None,
            "retryAfterMs": 2_000,
            "rateBucket": "messages",
            "rateGlobal": False,
        }

    clock = SimpleNamespace(now=lambda: 10_000)
    namespace = {
        "DurableObject": object,
        "Date": clock,
        "DISCORD_API_TIMEOUT_SECONDS": 8,
        "json": json,
        "urlparse": urlparse,
        "_discord_api_path_allowed": lambda method, path: (
            method == "POST" and path == f"/channels/{PUBLIC}/messages"),
        "_discord_rate_major": lambda _path: "channels:" + PUBLIC,
        "_discord_rate_route": lambda _path: "/channels/:id/messages",
        "_discord_provider_request": provider,
        "durable_object_traffic_note": lambda *_args, **_kwargs: None,
        "durable_object_traffic_flush": (
            lambda *_args, **_kwargs: asyncio.sleep(0)),
        "json_response": lambda data, status=200, **_kwargs: {
            "status": status, "data": data,
        },
    }
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[class_node], type_ignores=[])), str(ENTRY), "exec"), with_maintenance_off(namespace))
    gate = namespace["ForkMeshDiscordGate"]()
    storage = Storage()
    gate.ctx = SimpleNamespace(storage=storage)
    gate.env = SimpleNamespace(DISCORD_BOT_TOKEN="must-never-persist")

    first = await gate.fetch(Request())
    assert first["data"]["status"] == 429
    assert len(calls) == 1
    persisted = storage.values["rate_state"]
    assert "private-message-marker" not in persisted
    assert "must-never-persist" not in persisted
    assert "messages|channels:" in persisted

    second = await gate.fetch(Request())
    assert second["data"]["status"] == 429
    assert second["data"]["retryAfterMs"] == 2_000
    assert len(calls) == 1


def test_production_and_dev_bind_the_single_discord_rate_gate():
    wrangler = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
    assert wrangler.count('name = "FORKMESH_DISCORD_GATE"') == 2
    assert wrangler.count('class_name = "ForkMeshDiscordGate"') == 2
    assert 'tag = "v15"' in wrangler
    assert 'new_sqlite_classes = ["ForkMeshDiscordGate"]' in wrangler


def test_secret_setup_task_requires_a_freshly_rotated_bot_token():
    module_source = (SRC / "organization_discord.py").read_text(encoding="utf-8")
    assert "freshly rotated or reissued DISCORD_BOT_TOKEN" in module_source


def _organization_delete_handler(db):
    """Load the actual production org-delete body against a small D1 double."""

    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    handler_node = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "org_handler")

    async def ensure_schema(_env):
        return None

    async def org_row(_env, org):
        return "org-bi", {"name": org, "data": "sealed"}

    async def account_session_record(_env, _request, _data=None):
        return "account-owner", {"name": "owner"}

    async def org_role(_env, _org_bi, account):
        return "owner" if account == "owner" else ""

    async def d1_all(_env, _sql, *_args):
        return []

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def scope_bi(_env, _scope):
        return "scope-bi"

    async def audit(*_args, **_kwargs):
        return None

    async def purge_org(*_args, **_kwargs):
        return None

    async def bounded_json(_request):
        return {}

    def response(payload, status=200, **_kwargs):
        return {"status": status, "data": payload}

    namespace = {
        "ensure_schema": ensure_schema,
        "method_name": lambda request: request.method,
        "_org_row": org_row,
        "_org_management_session": account_session_record,
        "_org_role": org_role,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "_ap_digest_scope_bi": scope_bi,
        "_ORG_ALIAS_MEMO": SimpleNamespace(clear=lambda: None),
        "_audit_sensitive_action": audit,
        "bounded_json_request": bounded_json,
        "plugin_data": SimpleNamespace(purge_org=purge_org),
        "org_office_chat": SimpleNamespace(dissolve=purge_org),
        "_PluginPurgeRuntime": lambda env: env,
        "json_response": response,
        "_org_rbac": organization_rbac,
    }
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[handler_node], type_ignores=[])), str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace["org_handler"]


class _DeleteRequest:
    method = "DELETE"


@run_async_test
async def test_org_delete_purges_discord_marker_task_and_all_child_rows():
    """Exercise the real deletion handler, not just its SQL source text."""

    db = sqlite3.connect(":memory:")
    db.executescript("""
        CREATE TABLE org_repos (org_bi TEXT, repo TEXT);
        CREATE TABLE org_team_members (org_bi TEXT);
        CREATE TABLE org_team_collaborators (org_bi TEXT);
        CREATE TABLE org_teams (org_bi TEXT);
        CREATE TABLE ap_digest_queues (scope_bi TEXT);
        CREATE TABLE organization_discord_connectors (org_bi TEXT);
        CREATE TABLE organization_discord_setup_tasks (org_bi TEXT, task_id TEXT);
        CREATE TABLE organization_discord_oauth_grants (org_bi TEXT);
        CREATE TABLE organization_discord_oauth_states (org_bi TEXT);
        CREATE TABLE organization_tasks (task_id TEXT, org_bi TEXT);
        CREATE TABLE organization_task_checkins (task_id TEXT);
        CREATE TABLE organization_task_qa_reviews (task_id TEXT);
        CREATE TABLE organization_task_responses (task_id TEXT);
        CREATE TABLE organization_task_attachments (task_id TEXT);
        CREATE TABLE org_members (org_bi TEXT);
        CREATE TABLE ap_org_digest_settings (org_bi TEXT);
        CREATE TABLE orgs (org_bi TEXT);
    """)
    marker_task = "a" * 32
    unrelated_task = "b" * 32
    db.execute("INSERT INTO organization_discord_connectors VALUES (?)", ("org-bi",))
    db.execute(
        "INSERT INTO organization_discord_setup_tasks VALUES (?, ?)",
        ("org-bi", marker_task))
    db.execute("INSERT INTO organization_discord_oauth_grants VALUES (?)", ("org-bi",))
    db.execute("INSERT INTO organization_discord_oauth_states VALUES (?)", ("org-bi",))
    for task_id in (marker_task, unrelated_task):
        db.execute("INSERT INTO organization_tasks VALUES (?, ?)", (task_id, "org-bi"))
        for table in (
                "organization_task_checkins", "organization_task_qa_reviews",
                "organization_task_responses", "organization_task_attachments"):
            db.execute(f"INSERT INTO {table} VALUES (?)", (task_id,))
    db.execute("INSERT INTO org_members VALUES (?)", ("org-bi",))
    db.execute("INSERT INTO ap_org_digest_settings VALUES (?)", ("org-bi",))
    db.execute("INSERT INTO orgs VALUES (?)", ("org-bi",))
    db.commit()

    response = await _organization_delete_handler(db)(
        None, _DeleteRequest(), "acme")
    assert response["status"] == 200
    for table in (
            "organization_discord_connectors", "organization_discord_setup_tasks",
            "organization_discord_oauth_grants", "organization_discord_oauth_states"):
        assert db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0] == 0
    assert db.execute(
        "SELECT COUNT(*) FROM organization_tasks WHERE task_id=?", (marker_task,)
    ).fetchone()[0] == 0
    for table in (
            "organization_task_checkins", "organization_task_qa_reviews",
            "organization_task_responses", "organization_task_attachments"):
        assert db.execute(
            f"SELECT COUNT(*) FROM {table} WHERE task_id=?", (marker_task,)
        ).fetchone()[0] == 0
        # The selector is limited to the marker task; unrelated organization
        # work/history is not silently deleted by a connector cleanup.
        assert db.execute(
            f"SELECT COUNT(*) FROM {table} WHERE task_id=?", (unrelated_task,)
        ).fetchone()[0] == 1
