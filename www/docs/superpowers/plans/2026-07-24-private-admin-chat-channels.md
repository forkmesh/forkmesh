# Private Admin Chat Channels Implementation Plan

> **Historical security note (August 28, 2026):** This plan records the first
> relay-derived private-channel design. The shipped protocol now adds automatic
> X25519 recipient encryption to private channels and direct messages inside
> that outer room cipher. Public channels remain controller-readable for
> integrations. See [`security/sealed-chat.md`](../../security/sealed-chat.md)
> for the current boundary and its active-controller limitations.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add private encrypted chat channels that platform administrators create and manage, while invited registered users receive immediate access, all other users remain excluded, and every chat surface supports encrypted clipboard images and document attachments that survive refresh.

**Architecture:** A focused Python API module will own channel and membership policy over encrypted D1 records, while `entry.py` supplies existing session, encryption, audit, and Durable Object services. Each channel and key version receives a distinct relay-derived passphrase and Durable Object namespace, and a short-lived signed ticket gates browser WebSocket upgrades. The `/chat` client will replace insecure fixed authenticated labels with server-authorized private channels while preserving public World `#general`.

**Tech Stack:** Python 3.12, Cloudflare Python Workers, D1 SQLite, Durable Objects, Web Crypto AES-GCM, vanilla JavaScript, pytest, Playwright.

---

## File Map

- Create `cloudflare_worker/migrations/0069_private_chat_channels.sql` for channel, membership, index, and atomic key-rotation trigger DDL.
- Modify `cloudflare_worker/src/schema.py` so lazy schema creation matches migration `0069` exactly.
- Modify `cloudflare_worker/src/urls.py` to define the private-channel API and WebSocket route patterns.
- Create `cloudflare_worker/src/chat_channels_api.py` for normalization, authorization, channel CRUD scope, membership management, and response shaping.
- Modify `cloudflare_worker/src/entry.py` to adapt existing Worker services to the channel API, derive keys, issue and verify tickets, and gate Durable Object upgrades.
- Modify `cloudflare_worker/public/chat.js` to load authorized channels, switch isolated encrypted rooms, refresh membership, and manage admin actions.
- Modify `cloudflare_worker/public/chat.html` to add accessible create-channel and member-management controls.
- Modify `cloudflare_worker/public/dashboard-chat.js` to add encrypted image and document sending, rendering, clipboard paste, and dynamically mounted attachment controls without rewriting generated dashboard pages.
- Modify `cloudflare_worker/src/entry.py` to retain bounded attachment frames under an aggregate per-room byte budget.
- Modify `cloudflare_worker/public/dashboard/chat/index.html` to link the dashboard's public-only chat view to the full channel directory.
- Modify `cloudflare_worker/public/docs/protocol/index.html` to document private channel access and relay-readable encryption semantics.
- Create `cloudflare_worker/tests/test_private_chat_channel_schema.py` for schema and route contracts.
- Create `cloudflare_worker/tests/test_chat_channels_api.py` for executable API and authorization behavior.
- Create `cloudflare_worker/tests/test_chat_channel_room_access.py` for key, ticket, and pre-Durable-Object admission behavior.
- Create `cloudflare_worker/tests/test_private_chat_channels_frontend.py` for browser-source contracts.
- Create `cloudflare_worker/browser_tests/tests/private-chat-channels.spec.js` for administrator and invited-user end-to-end behavior.
- Create `cloudflare_worker/browser_tests/tests/chat-attachments.spec.js` for clipboard-image, document, encryption, and refresh-persistence behavior.

### Task 2A: Add Compatible Encrypted Attachments

**Files:**

- Create: `cloudflare_worker/tests/test_chat_attachments_frontend.py`
- Create: `cloudflare_worker/tests/test_chat_history_byte_budget.py`
- Create: `cloudflare_worker/browser_tests/tests/chat-attachments.spec.js`
- Modify: `cloudflare_worker/public/chat.js`
- Modify: `cloudflare_worker/public/chat.html`
- Modify: `cloudflare_worker/public/dashboard-chat.js`
- Modify: `cloudflare_worker/src/entry.py`

- [ ] **Step 1: Reproduce clipboard-image and document gaps in Playwright**

Drive the full chat as a user, paste a real PNG through `ClipboardEvent.clipboardData`, and select a text or PDF document through the hidden file input.
Confirm that the current UI sends neither attachment and therefore cannot replay either after refresh.

- [ ] **Step 2: Add failing source and retention tests**

Pin the desktop-compatible `fileName`, `fileMime`, and `file` fields, the 1 MiB decoded-file limit, safe basename handling, full-chat and dashboard paste listeners, attachment rendering, and a retained-room byte budget below D1's row and database abuse boundaries.

- [ ] **Step 3: Implement shared attachment behavior in both browser clients**

Add file reading, clipboard image extraction, encrypted send, image preview, document card, object URL cleanup, size feedback, and accessible file controls.
Use existing room keys and durable chat frames without introducing plaintext upload endpoints or persistent browser keys.

- [ ] **Step 4: Make bounded attachment messages durable**

Raise the per-frame retained limit to 1,900,000 bytes, safely below D1's 2,000,000-byte maximum row size.
Add a 16 MiB per-room retained-byte budget and prune oldest encrypted frames after inserts while preserving the existing seven-day and 500-frame caps.

- [ ] **Step 5: Run focused Python and Playwright tests**

Run the attachment source tests, chat-history budget tests, existing self-message persistence test, and new clipboard/document Playwright test.
Confirm image and document attachments remain visible after a simulated retained-frame reload.

### Task 1: Persist Private Channels and Define Routes

**Files:**

- Create: `cloudflare_worker/tests/test_private_chat_channel_schema.py`
- Create: `cloudflare_worker/migrations/0069_private_chat_channels.sql`
- Modify: `cloudflare_worker/src/schema.py`
- Modify: `cloudflare_worker/src/urls.py`

- [ ] **Step 1: Write the failing schema and route contract test**

```python
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import schema  # noqa: E402
import urls  # noqa: E402


def test_private_chat_channel_migration_and_lazy_schema_match():
    migration = ROOT / "migrations" / "0069_private_chat_channels.sql"
    assert migration.exists()
    sql = migration.read_text(encoding="utf-8")
    joined = "\n".join(schema.SCHEMA_STATEMENTS)
    for marker in (
        "CREATE TABLE IF NOT EXISTS chat_channels",
        "CREATE TABLE IF NOT EXISTS chat_channel_members",
        "idx_chat_channel_members_member",
        "trg_chat_channel_member_remove_rotate",
    ):
        assert marker in sql
        assert marker in joined

    db = sqlite3.connect(":memory:")
    db.executescript(sql)
    db.execute(
        "INSERT INTO chat_channels "
        "(channel_id,name_bi,data,created_by_bi,created_at,updated_at,key_version) "
        "VALUES ('a' || printf('%031d',0),'name-bi','sealed','admin-bi',1,1,1)"
    )
    channel_id = db.execute("SELECT channel_id FROM chat_channels").fetchone()[0]
    db.execute(
        "INSERT INTO chat_channel_members "
        "(channel_id,member_bi,invited_by_bi,joined_at) VALUES (?,?,?,?)",
        (channel_id, "alice-bi", "admin-bi", 2),
    )
    db.execute(
        "DELETE FROM chat_channel_members WHERE channel_id=? AND member_bi=?",
        (channel_id, "alice-bi"),
    )
    version = db.execute(
        "SELECT key_version FROM chat_channels WHERE channel_id=?", (channel_id,)
    ).fetchone()[0]
    assert version == 2


def test_private_chat_channel_routes_are_exact_and_opaque():
    channel_id = "a" * 32
    assert urls.CHAT_CHANNELS_RE.fullmatch("/api/chat/channels")
    assert urls.CHAT_CHANNEL_MEMBERS_RE.fullmatch(
        f"/api/chat/channels/{channel_id}/members"
    )
    assert urls.CHAT_CHANNEL_ROOM_ACCESS_RE.fullmatch(
        f"/api/chat/channels/{channel_id}/room-access"
    )
    assert urls.CHAT_CHANNEL_WS_RE.fullmatch(
        f"/api/chat/channels/{channel_id}/ws"
    )
    assert not urls.CHAT_CHANNEL_WS_RE.fullmatch(
        "/api/chat/channels/release-team/ws"
    )
```

- [ ] **Step 2: Run the test and confirm the missing migration and regexes fail**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channel_schema.py -q`

Expected: FAIL because migration `0069` and `CHAT_CHANNELS_RE` do not exist.

- [ ] **Step 3: Add migration `0069` with atomic removal rotation**

```sql
-- Private administrator-created chat channels and direct user memberships.

CREATE TABLE IF NOT EXISTS chat_channels (
  channel_id    TEXT PRIMARY KEY,
  name_bi       TEXT NOT NULL UNIQUE,
  data          TEXT NOT NULL,
  created_by_bi TEXT NOT NULL,
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL,
  key_version   INTEGER NOT NULL DEFAULT 1 CHECK (key_version >= 1)
);

CREATE TABLE IF NOT EXISTS chat_channel_members (
  channel_id    TEXT NOT NULL,
  member_bi     TEXT NOT NULL,
  invited_by_bi TEXT NOT NULL,
  joined_at     INTEGER NOT NULL,
  PRIMARY KEY (channel_id, member_bi)
);

CREATE INDEX IF NOT EXISTS idx_chat_channel_members_member
  ON chat_channel_members(member_bi, channel_id);

CREATE TRIGGER IF NOT EXISTS trg_chat_channel_member_remove_rotate
AFTER DELETE ON chat_channel_members
BEGIN
  UPDATE chat_channels
     SET key_version = key_version + 1,
         updated_at = CAST(strftime('%s','now') AS INTEGER) * 1000
   WHERE channel_id = OLD.channel_id;
END;
```

- [ ] **Step 4: Mirror the same DDL in `SCHEMA_STATEMENTS`**

Add the two tables, index, and trigger to `src/schema.py` beside `chat_history`.
Use the exact names and columns from migration `0069` so first-request schema creation and deployed migrations cannot diverge.

- [ ] **Step 5: Add exact route patterns to `src/urls.py`**

```python
CHAT_CHANNELS_RE = re.compile(r"^/api/chat/channels/?$")
CHAT_CHANNEL_MEMBERS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/members/?$")
CHAT_CHANNEL_ROOM_ACCESS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/room-access/?$")
CHAT_CHANNEL_WS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/ws/?$")
```

- [ ] **Step 6: Run the schema test and the existing schema suite**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channel_schema.py tests/test_local_migrations.py tests/test_status_page.py -q`

Expected: PASS.

- [ ] **Step 7: Commit the persistence boundary**

```bash
git add cloudflare_worker/migrations/0069_private_chat_channels.sql cloudflare_worker/src/schema.py cloudflare_worker/src/urls.py cloudflare_worker/tests/test_private_chat_channel_schema.py
git commit -m "feat: add private chat channel schema"
```

### Task 2: Implement Channel and Membership Authorization

**Files:**

- Create: `cloudflare_worker/src/chat_channels_api.py`
- Create: `cloudflare_worker/tests/test_chat_channels_api.py`

- [ ] **Step 1: Write an executable SQLite runtime test harness**

Create `tests/test_chat_channels_api.py` with a `FakeRuntime` that executes migration `0069`, stores sealed values as deterministic JSON, returns active accounts from a dictionary, records audits, and exposes `method`, `now`, `new_id`, `response`, `json_body`, `session`, `is_admin`, `account`, `blind`, `seal`, `open`, `d1_all`, `d1_first`, and `d1_run`.

```python
class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0069_private_chat_channels.sql")
            .read_text(encoding="utf-8")
        )
        self.actor = ""
        self.request_method = "GET"
        self.request_data = {}
        self.clock = 1_800_000_000_000
        self.admins = {"admin"}
        self.accounts = {"admin", "alice", "bob"}
        self.audits = []
        self.ids = 0

    def use(self, method, actor="", data=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        return self

    def method(self):
        return self.request_method

    def now(self):
        return self.clock

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def response(self, data, status=200, cache_control=None, extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cache_control": cache_control,
            "headers": dict(extra_headers or {}),
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return (self.request_data, "") if isinstance(self.request_data, dict) else (None, "invalid_json")

    async def session(self, _data=None):
        if self.actor not in self.accounts:
            return "", None
        return f"bi:{self.actor}", {"name": self.actor, "status": "active"}

    async def is_admin(self, name):
        return name in self.admins

    async def account(self, name):
        normalized = str(name or "").strip().lower()
        return (f"bi:{normalized}", normalized) if normalized in self.accounts else ("", "")

    async def blind(self, value):
        return "blind:" + str(value).strip().lower()

    async def seal(self, value):
        return json.dumps(value, sort_keys=True)

    async def open(self, value):
        return json.loads(value)

    async def audit(self, actor, action, target_type="", target="", outcome="success", details=None):
        self.audits.append((actor, action, target_type, target, outcome, details or {}))

    async def room_access(self, channel_id, key_version, account_bi):
        return {
            "room": f"private-{channel_id}-v{key_version}",
            "passphrase": f"key:{channel_id}:{key_version}",
            "webSocketUrl": f"/api/chat/channels/{channel_id}/ws?ticket=ticket:{account_bi}",
        }

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return {"changes": cursor.rowcount}
```

- [ ] **Step 2: Write failing behavioral tests**

```python
@run_async_test
async def test_admin_creates_and_non_admin_cannot_create_channel():
    runtime = FakeRuntime()
    denied = await api.handle(
        runtime.use("POST", "alice", {"name": "release-team"}),
        "/api/chat/channels",
    )
    assert denied["status"] == 403
    assert denied["data"] == {"error": "admin_required"}
    assert runtime.audits[-1][4] == "denied"

    created = await api.handle(
        runtime.use("POST", "admin", {"name": "release-team"}),
        "/api/chat/channels",
    )
    assert created["status"] == 201
    assert created["data"]["channel"]["name"] == "release-team"
    assert created["data"]["channel"]["canManage"] is True


@run_async_test
async def test_invited_user_lists_and_accesses_only_their_channel():
    runtime = FakeRuntime()
    first = await create_channel(runtime, "release-team")
    second = await create_channel(runtime, "security")
    channel_id = first["data"]["channel"]["id"]
    await api.handle(
        runtime.use("POST", "admin", {"username": "alice"}),
        f"/api/chat/channels/{channel_id}/members",
    )

    listed = await api.handle(runtime.use("GET", "alice"), "/api/chat/channels")
    assert [item["name"] for item in listed["data"]["channels"]] == ["release-team"]
    assert second["data"]["channel"]["id"] not in json.dumps(listed["data"])

    access = await api.handle(
        runtime.use("GET", "alice"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    assert access["status"] == 200
    assert access["data"]["passphrase"].startswith("key:")


@run_async_test
async def test_member_removal_is_idempotent_and_rotates_once():
    runtime = FakeRuntime()
    created = await create_channel(runtime, "release-team")
    channel_id = created["data"]["channel"]["id"]
    await api.handle(
        runtime.use("POST", "admin", {"username": "alice"}),
        f"/api/chat/channels/{channel_id}/members",
    )
    removed = await api.handle(
        runtime.use("DELETE", "admin", {"username": "alice"}),
        f"/api/chat/channels/{channel_id}/members",
    )
    assert removed["data"]["keyVersion"] == 2
    again = await api.handle(
        runtime.use("DELETE", "admin", {"username": "alice"}),
        f"/api/chat/channels/{channel_id}/members",
    )
    assert again["data"]["keyVersion"] == 2
    denied = await api.handle(
        runtime.use("GET", "alice"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    assert denied["status"] == 404


@run_async_test
async def test_all_admins_have_implicit_access_without_membership_rows():
    runtime = FakeRuntime()
    runtime.admins.add("bob")
    created = await create_channel(runtime, "release-team")
    channel_id = created["data"]["channel"]["id"]
    access = await api.handle(
        runtime.use("GET", "bob"),
        f"/api/chat/channels/{channel_id}/room-access",
    )
    assert access["status"] == 200
    count = runtime.db.execute("SELECT COUNT(*) FROM chat_channel_members").fetchone()[0]
    assert count == 0
```

- [ ] **Step 3: Run the API test and confirm the module import fails**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_chat_channels_api.py -q`

Expected: FAIL because `chat_channels_api` does not exist.

- [ ] **Step 4: Implement `chat_channels_api.py`**

Implement these exact public constants and entry point:

```python
CHANNEL_BODY_MAX_BYTES = 1024
MAX_PRIVATE_CHANNELS = 100
MAX_CHANNEL_MEMBERS = 500
CHANNEL_NAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$")
USERNAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")


async def handle(runtime, path):
    await runtime.ensure_schema()
    target = _target(path)
    if target is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    data = {}
    if method in ("POST", "DELETE"):
        data, error_response = await _body(runtime)
        if error_response is not None:
            return error_response
    account_bi, account = await runtime.session(data)
    actor = str((account or {}).get("name") or "").strip().lower()
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    is_admin = await runtime.is_admin(actor)
    kind, channel_id = target
    if kind == "collection":
        if method == "GET":
            return await _list_channels(runtime, account_bi, is_admin)
        if method != "POST":
            return _response(runtime, {"error": "method_not_allowed"}, status=405)
        if not is_admin:
            await _denied_audit(runtime, actor, "create", "new")
            return _response(runtime, {"error": "admin_required"}, status=403)
        return await _create_channel(runtime, account_bi, actor, data)
    if kind == "room-access":
        if method != "GET":
            return _response(runtime, {"error": "method_not_allowed"}, status=405)
        return await _room_access(runtime, channel_id, account_bi, is_admin)
    if not is_admin:
        await _denied_audit(runtime, actor, "member_mutation", channel_id)
        return _response(runtime, {"error": "admin_required"}, status=403)
    if method == "GET":
        return await _list_members(runtime, channel_id)
    if method == "POST":
        return await _add_member(runtime, channel_id, account_bi, actor, data)
    if method == "DELETE":
        return await _remove_member(runtime, channel_id, account_bi, actor, data)
    return _response(runtime, {"error": "method_not_allowed"}, status=405)
```

Implement helpers named `_response`, `_target`, `_body`, `_channel_payload`, `_authorized_channel`, `_list_channels`, `_create_channel`, `_list_members`, `_add_member`, `_remove_member`, and `_room_access`.
Use only parameterized SQL.
Seal `{ "name": normalized_name, "createdBy": actor }` before insertion.
Return `404 not_found` from `_authorized_channel` when the channel is absent or the caller is neither an administrator nor a member.
Use `INSERT OR IGNORE` for idempotent membership grants and rely on `trg_chat_channel_member_remove_rotate` for atomic rotation on a real deletion.
Audit `chat.channel.create`, `chat.channel.member_grant`, and `chat.channel.member_revoke` with channel ID as the target and metadata-only `details`.

- [ ] **Step 5: Add validation and failure-path tests**

Add tests proving invalid names, duplicate names, missing or inactive users, non-admin member listing, wrong methods, member caps, and missing-versus-unauthorized room access return their specified status and error values without leaking sealed data or account blind indexes.

- [ ] **Step 6: Run the focused API tests**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_chat_channels_api.py -q`

Expected: PASS.

- [ ] **Step 7: Commit the API policy**

```bash
git add cloudflare_worker/src/chat_channels_api.py cloudflare_worker/tests/test_chat_channels_api.py
git commit -m "feat: add private chat channel api"
```

### Task 3: Derive Channel Keys and Gate WebSockets

**Files:**

- Modify: `cloudflare_worker/src/entry.py`
- Create: `cloudflare_worker/tests/test_chat_channel_room_access.py`

- [ ] **Step 1: Write failing room-access helper tests**

Create an AST extraction harness following `tests/test_chat_room_key.py` and assert:

```python
def test_channel_passphrases_are_scoped_by_channel_and_version():
    first = asyncio.run(ns["_chat_channel_passphrase"](env, "a" * 32, 1))
    other_channel = asyncio.run(ns["_chat_channel_passphrase"](env, "b" * 32, 1))
    rotated = asyncio.run(ns["_chat_channel_passphrase"](env, "a" * 32, 2))
    assert len(first) == 64
    assert first != other_channel
    assert first != rotated
    assert env.DATA_KEY not in first


def test_ticket_round_trip_is_channel_version_account_and_expiry_bound():
    token = ns["_chat_channel_ticket"](env, "a" * 32, 3, "bi-alice")
    assert ns["_chat_channel_ticket_claims"](env, token) == {
        "channel_id": "a" * 32,
        "key_version": 3,
        "account_bi": "bi-alice",
    }
    assert ns["_chat_channel_ticket_claims"](env, token + "x") is None
    clock[0] += 60_001
    assert ns["_chat_channel_ticket_claims"](env, token) is None


def test_private_socket_authorization_precedes_durable_object_lookup():
    source = function_source("_chat_channel_socket_handler")
    assert source.index("_chat_channel_ticket_claims") < source.index("idFromName")
    assert source.index("chat_channel_members") < source.index("idFromName")
    assert "chat-channel:" in source
    assert ":v" in source
```

- [ ] **Step 2: Run the test and confirm the missing helpers fail**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_chat_channel_room_access.py -q`

Expected: FAIL because the passphrase, ticket, and socket helpers do not exist.

- [ ] **Step 3: Add ticket and key helpers in `entry.py`**

```python
CHAT_CHANNEL_TICKET_TTL_MS = 60 * 1000


async def _chat_channel_passphrase(env, channel_id, key_version):
    secret = (
        _require_data_secret(env)
        + ":private-chat-channel-v1:"
        + channel_id
        + ":"
        + str(int(key_version))
    )
    digest = await js_crypto.subtle.digest("SHA-256", _to_js(secret.encode()))
    return bytes(Uint8Array.new(digest).to_py()).hex()


def _chat_channel_ticket(env, channel_id, key_version, account_bi):
    expires = int(Date.now()) + CHAT_CHANNEL_TICKET_TTL_MS
    canonical = ".".join((
        "v1", channel_id, str(int(key_version)), account_bi, str(expires)
    ))
    signature = hmac.new(
        (_require_data_secret(env) + ":chat-channel-ticket-v1").encode(),
        canonical.encode(),
        "sha256",
    ).hexdigest()
    return canonical + "." + signature
```

Implement `_chat_channel_ticket_claims` with strict field count, lowercase 32-hex channel validation, positive integer version, bounded account blind-index validation, expiry, and `hmac.compare_digest`.
Return no raw reason on failure.

```python
def _chat_channel_ticket_claims(env, token):
    parts = str(token or "").split(".")
    if len(parts) != 6:
        return None
    version_tag, channel_id, key_version_raw, account_bi, expires_raw, signature = parts
    if version_tag != "v1" or not re.fullmatch(r"[0-9a-f]{32}", channel_id):
        return None
    if not re.fullmatch(r"[A-Za-z0-9:_-]{1,160}", account_bi):
        return None
    try:
        key_version = int(key_version_raw)
        expires = int(expires_raw)
    except (TypeError, ValueError):
        return None
    if key_version < 1 or expires < int(Date.now()):
        return None
    canonical = ".".join(parts[:5])
    expected = hmac.new(
        (_require_data_secret(env) + ":chat-channel-ticket-v1").encode(),
        canonical.encode(),
        "sha256",
    ).hexdigest()
    if not hmac.compare_digest(signature, expected):
        return None
    return {
        "channel_id": channel_id,
        "key_version": key_version,
        "account_bi": account_bi,
    }
```

- [ ] **Step 4: Add the runtime adapter and API route**

Import `chat_channels_api` and the four private-channel regexes.
Add `_ChatChannelsRuntime` as a small `_WorldCommunityRuntime` subclass with `room_access(channel_id, key_version, account_bi)` returning `room`, `passphrase`, and the URL-encoded ticketed WebSocket path.
Route the collection, members, and room-access paths to `chat_channels_api.handle` before generic repository room routing.

- [ ] **Step 5: Gate private channel WebSockets before Durable Object selection**

Implement `_chat_channel_socket_handler(env, request, channel_id)` to:

1. Require `Upgrade: websocket`.
2. Parse and verify the ticket.
3. Require the ticket channel ID to equal the path channel ID.
4. Query the current `chat_channels.key_version` and allow the ticket account when `users.is_admin = 1` or a matching `chat_channel_members` row exists.
5. Require the ticket version to equal the current version.
6. Route only the authorized request to `FORKMESH_MAINNODE_ROOM.idFromName(f"chat-channel:{channel_id}:v{key_version}")`.
7. Return the same private `404 not_found` response for invalid, expired, stale, removed, missing, and unauthorized access.

- [ ] **Step 6: Run room-access, existing room-key, and authorization tests**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_chat_channel_room_access.py tests/test_chat_room_key.py tests/test_security_controls.py -q`

Expected: PASS.

- [ ] **Step 7: Commit Worker integration**

```bash
git add cloudflare_worker/src/entry.py cloudflare_worker/tests/test_chat_channel_room_access.py
git commit -m "feat: gate private chat channel rooms"
```

### Task 4: Replace Insecure Fixed Labels in the Full Chat Client

**Files:**

- Create: `cloudflare_worker/tests/test_private_chat_channels_frontend.py`
- Modify: `cloudflare_worker/public/chat.js`

- [ ] **Step 1: Write failing frontend source contracts**

```python
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHAT = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")


def test_chat_loads_authorized_private_channels_instead_of_fixed_labels():
    assert 'const PRIVATE_CHANNELS_ENDPOINT = "/api/chat/channels"' in CHAT
    assert "async function refreshPrivateChannels(" in CHAT
    assert "Authorization" in CHAT
    assert 'DEFAULT_CHANNELS = ["#general", "#welcome", "#random"]' not in CHAT
    assert "keyVersion" in CHAT


def test_private_channel_access_uses_server_room_and_ephemeral_ticket():
    assert '"/room-access"' in CHAT
    assert "access.webSocketUrl" in CHAT
    assert "access.passphrase" in CHAT
    assert "access.room" in CHAT
    assert "localStorage.setItem" not in room_access_source()


def test_removed_or_rotated_channel_closes_stale_room_and_falls_back():
    assert "function reconcilePrivateChannels(" in CHAT
    assert "switchChatRoom" in CHAT
    assert "keyVersion" in CHAT
    assert 'setActiveChannel("#general")' in CHAT
```

The `room_access_source` helper must slice from `async function fetchRoomAccess` to the next top-level function.

- [ ] **Step 2: Run the frontend test and confirm the old fixed labels fail**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channels_frontend.py -q`

Expected: FAIL because the private-channel endpoint and refresh logic do not exist.

- [ ] **Step 3: Introduce an authenticated channel API helper**

```javascript
const PRIVATE_CHANNELS_ENDPOINT = "/api/chat/channels";
const PRIVATE_CHANNEL_REFRESH_MS = 30000;
const privateChannels = new Map();

async function privateChannelRequest(path, options = {}) {
  const session = userSession();
  if (!session?.sessionToken) throw Object.assign(new Error("Sign in required"), { code: "auth" });
  const headers = new Headers(options.headers || {});
  headers.set("accept", "application/json");
  headers.set("authorization", `Bearer ${session.sessionToken}`);
  if (options.body) headers.set("content-type", "application/json");
  const response = await fetch(path, { ...options, headers, cache: "no-store" });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw Object.assign(new Error(data.error || "unavailable"), {
    code: data.error || "unavailable",
    status: response.status,
  });
  return data;
}
```

- [ ] **Step 4: Reconcile the server-authorized channel list**

Implement `refreshPrivateChannels` and `reconcilePrivateChannels` so channel records are keyed by opaque ID, rendered by their server-returned name, and removed immediately when absent from a successful refresh.
Keep `#general` as the only built-in channel.
If the active private channel disappears, close its socket, clear `roomKey`, clear any room-access object, and call `setActiveChannel("#general")`.
If its `keyVersion` changes, close and reconnect without retaining the old passphrase or CryptoKey.

- [ ] **Step 5: Switch encryption and WebSocket connection by room-access response**

Replace the old authenticated shared-room branch with `fetchRoomAccess(channel)`.
For `#general`, keep the existing public room-key endpoint and WebSocket path.
For a private channel, request `/api/chat/channels/{id}/room-access`, derive AES-GCM using `access.room` as the PBKDF2 salt namespace, and open `access.webSocketUrl` exactly once.
Do not write `passphrase`, `room`, ticket, or derived key into local storage.

- [ ] **Step 6: Preserve message protocol compatibility**

Continue sending the selected display label in the encrypted `channel` field.
Keep durable frame tagging, reactions, edits, deletes, admin deletes, mentions, roster rendering, reconnect backoff, and 60-second presence behavior unchanged.
Scope the decrypted message buffer and unread counters by opaque channel ID so two names can never alias in memory.

- [ ] **Step 7: Run chat frontend regression tests**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channels_frontend.py tests/test_dashboard_chat_persist_frontend.py tests/test_web_chat_connect_diagnostics_frontend.py tests/test_public_chat_mobile_viewport_frontend.py -q`

Expected: PASS.

- [ ] **Step 8: Commit private-channel participation**

```bash
git add cloudflare_worker/public/chat.js cloudflare_worker/tests/test_private_chat_channels_frontend.py
git commit -m "feat: join authorized private chat channels"
```

### Task 5: Add Administrator Channel Controls

**Files:**

- Modify: `cloudflare_worker/public/chat.html`
- Modify: `cloudflare_worker/public/chat.js`
- Modify: `cloudflare_worker/tests/test_private_chat_channels_frontend.py`

- [ ] **Step 1: Add failing accessible-UI contracts**

```python
HTML = (ROOT / "public" / "chat.html").read_text(encoding="utf-8")


def test_admin_channel_controls_are_accessible_and_hidden_by_default():
    assert 'id="chat-channel-create"' in HTML
    assert 'aria-label="Create private channel"' in HTML
    assert 'id="chat-channel-dialog"' in HTML
    assert 'aria-labelledby="chat-channel-dialog-title"' in HTML
    assert 'id="chat-channel-manage"' in HTML
    assert 'id="chat-channel-members"' in HTML
    assert 'id="chat-channel-error"' in HTML
    assert "session?.isAdmin" in CHAT


def test_admin_controls_call_create_invite_list_and_remove_endpoints():
    assert 'privateChannelRequest(PRIVATE_CHANNELS_ENDPOINT, {' in CHAT
    assert 'method: "POST"' in CHAT
    assert '`/api/chat/channels/${channel.id}/members`' in CHAT
    assert 'method: "DELETE"' in CHAT
```

- [ ] **Step 2: Run the UI contract and confirm selectors are missing**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channels_frontend.py -q`

Expected: FAIL on the missing controls and API actions.

- [ ] **Step 3: Add accessible dialog markup and responsive styles**

Add a hidden Create private channel button to the rooms-pane heading.
Add a hidden Manage members button beside the selected private-channel title.
Add one native `<dialog>` with create and member-management sections, explicit labels, submit buttons, close control, loading status, and an `aria-live="polite"` error region.
Use existing CSS variables and make every grid or flex child `min-width: 0` so long names wrap or truncate without horizontal overflow at 320 CSS pixels.

- [ ] **Step 4: Show controls only from authoritative hydrated session state**

After `hydrateUserSession`, show creation and management controls only when `userSession()?.isAdmin === true`.
This UI condition is convenience only; every mutation still depends on server-side `users.is_admin`.

- [ ] **Step 5: Implement create and membership actions**

Create a channel with `POST /api/chat/channels`, refresh the channel list, select the returned channel, and keep the dialog open only when an error occurs.
List members when an administrator opens Manage members.
Invite with an exact username through `POST /members` and remove through `DELETE /members`.
Disable only the action in progress, render server validation messages inline, and preserve the current chat connection after a failed mutation.

- [ ] **Step 6: Run frontend tests and inspect both color schemes**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channels_frontend.py tests/test_dashboard_chat_persist_frontend.py -q`

Expected: PASS.

Open `/chat` at desktop and 320-pixel widths in both light and dark themes.
Confirm the rooms pane, dialog, member names, buttons, focus rings, status, and composer remain legible with no horizontal page overflow.

- [ ] **Step 7: Commit administrator controls**

```bash
git add cloudflare_worker/public/chat.html cloudflare_worker/public/chat.js cloudflare_worker/tests/test_private_chat_channels_frontend.py
git commit -m "feat: manage private chat channels"
```

### Task 6: Make Private Channels Discoverable and Document the Protocol

**Files:**

- Modify: `cloudflare_worker/public/dashboard/chat/index.html`
- Modify: `cloudflare_worker/public/docs/protocol/index.html`
- Modify: `cloudflare_worker/tests/test_private_chat_channels_frontend.py`

- [ ] **Step 1: Write failing discoverability and documentation contracts**

```python
DASHBOARD = (ROOT / "public" / "dashboard" / "chat" / "index.html").read_text(encoding="utf-8")
PROTOCOL = (ROOT / "public" / "docs" / "protocol" / "index.html").read_text(encoding="utf-8")


def test_dashboard_general_chat_links_to_private_channel_directory():
    assert 'href="/chat"' in DASHBOARD
    assert "Open private channels" in DASHBOARD


def test_protocol_documents_private_channel_membership_and_relay_readability():
    assert "/api/chat/channels" in PROTOCOL
    assert "users.is_admin" in PROTOCOL
    assert "relay can decrypt" in PROTOCOL
    assert "key version" in PROTOCOL.lower()
```

- [ ] **Step 2: Run the test and confirm the copy is absent**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channels_frontend.py -q`

Expected: FAIL on the dashboard link and protocol text.

- [ ] **Step 3: Add the dashboard link**

Place an `Open private channels` link in the existing encrypted-chat notice on `/dashboard/chat`.
Use the existing card and text classes and do not change the embedded World `#general` connection behavior.

- [ ] **Step 4: Document the protocol**

Add a concise private-channel subsection describing administrator creation, direct registered-user membership, room-access authorization, channel-and-version key derivation, 60-second WebSocket tickets, key rotation on removal, seven-day bounded history, and the fact that relay-derived shared keys remain relay-readable.

- [ ] **Step 5: Run the focused test and HTML link checks**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channels_frontend.py tests/test_static_route_canonicalization.py -q`

Expected: PASS.

- [ ] **Step 6: Commit discoverability and docs**

```bash
git add cloudflare_worker/public/dashboard/chat/index.html cloudflare_worker/public/docs/protocol/index.html cloudflare_worker/tests/test_private_chat_channels_frontend.py
git commit -m "docs: explain private chat channels"
```

### Task 7: Exercise the User Journey in Playwright

**Files:**

- Create: `cloudflare_worker/browser_tests/tests/private-chat-channels.spec.js`

- [ ] **Step 1: Write the failing administrator-to-member journey**

Build a deterministic route fixture for `/api/accounts/admin`, `/api/chat/channels`, member mutations, and room-access responses.
Use Playwright's `page.addInitScript` to install the administrator session before `/chat` loads and `page.routeWebSocket` to capture each room URL and encrypted frame.

```javascript
test("admin creates a private channel and directly adds a registered user", async ({ page }) => {
  await installChatFixture(page, { actor: "admin", isAdmin: true });
  await page.goto("/chat");
  await page.getByRole("button", { name: "Create private channel" }).click();
  await page.getByLabel("Channel name").fill("release-team");
  await page.getByRole("button", { name: "Create channel" }).click();
  await expect(page.getByRole("button", { name: /release-team/ })).toBeVisible();

  await page.getByRole("button", { name: "Manage members" }).click();
  await page.getByLabel("Registered username").fill("alice");
  await page.getByRole("button", { name: "Add member" }).click();
  await expect(page.locator("#chat-channel-members")).toContainText("alice");
});
```

- [ ] **Step 2: Add invited-user, non-member, removal, and rotation cases**

Add tests proving Alice sees the channel on refresh and can exchange a decrypted chat frame, Bob never receives the channel and gets no room-access call, and Alice falls back to `#general` after her membership disappears while the server's channel version increments.
Assert the private WebSocket path contains the opaque channel ID, never the channel name, and uses a different path from public World `#general`.

- [ ] **Step 3: Add desktop and mobile visual layout assertions**

At 1280 by 800 and 320 by 720 viewports, assert the rooms list, channel title, management dialog, and composer are visible, and assert `document.documentElement.scrollWidth <= document.documentElement.clientWidth`.
Capture failure-only screenshots through the existing Playwright configuration.

- [ ] **Step 4: Run the new browser test and observe the initial failure**

Run: `cd cloudflare_worker/browser_tests && npm test -- --grep "private channel"`

Expected before Tasks 4 and 5 are complete: FAIL on missing controls or private-channel fetches.
Expected after Tasks 4 and 5: PASS.

- [ ] **Step 5: Run all browser tests**

Run: `cd cloudflare_worker/browser_tests && npm test`

Expected: PASS with no console errors, layout overflow, or flaky retries.

- [ ] **Step 6: Commit the end-to-end coverage**

```bash
git add cloudflare_worker/browser_tests/tests/private-chat-channels.spec.js
git commit -m "test: cover private chat channel journey"
```

### Task 8: Verify the Complete Feature

**Files:**

- No production file changes are expected.

- [ ] **Step 1: Run targeted private-channel tests**

Run: `cd cloudflare_worker && python3 -m pytest tests/test_private_chat_channel_schema.py tests/test_chat_channels_api.py tests/test_chat_channel_room_access.py tests/test_private_chat_channels_frontend.py -q`

Expected: PASS.

- [ ] **Step 2: Run all Python tests**

Run: `cd cloudflare_worker && python3 -m pytest -q`

Expected: PASS with no warnings or flaky reruns.

- [ ] **Step 3: Run all Playwright tests**

Run: `cd cloudflare_worker/browser_tests && npm test`

Expected: PASS.

- [ ] **Step 4: Check formatting, generated-file safety, and the final diff**

Run: `git diff --check && git status --short && git diff --stat HEAD~7..HEAD`

Expected: no whitespace errors, no `CHANGELOG.md` or generated-file edits, and only the planned channel files.

- [ ] **Step 5: Confirm the acceptance criteria manually**

Run the Worker locally with a test D1 database.
Sign in as an account with `users.is_admin = 1`, create a channel, invite a second active user, exchange encrypted messages, remove the user, and confirm the removed session falls back to public `#general` and cannot reconnect to the rotated private room.

- [ ] **Step 6: Record final verification evidence**

Capture exact passing test counts and commands in the final handoff.
Do not claim completion if any targeted, full-suite, browser, lint, or manual authorization check fails.
