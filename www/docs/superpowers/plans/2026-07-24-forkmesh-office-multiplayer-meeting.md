# ForkMesh Office Multiplayer Meeting Implementation Plan

> **Historical record (2026-07-24).** This design or plan was written before
> the repository layout moved (`cloudflare_worker/` became `app/`, the
> marketing Worker became `www/`, World code moved to `world/`, `qt_client/`
> became `desktop/`), so its paths and step lists are out of date. It is kept
> for the reasoning. The shipped code is the authority:
> `world/public/world/world-office*.js`, `world/src/world_office_routes.py`,
> `world/src/world_durable_objects.py` (`ForkMeshOfficeRoom`),
> `app/src/chat_channels_api.py`, and `app/public/chat.js`. See
> `docs/world/README.md` in the repository for the current World architecture.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Office's dock-first interaction with authorized spatial meeting rooms where live avatars can claim seats and encrypted messages appear as verified in-world bubbles.

**Architecture:** Keep global World presence coarse and introduce a separate authorized `ForkMeshOfficeRoom` Durable Object for room-local avatars and server-owned chairs.
Extract reusable chat cryptography, attachment, and room transport modules so the full chat page and native Office meeting client use one encrypted protocol implementation.
Join decrypted chat events to live avatars only in the browser through ephemeral P-256 meeting proofs.

**Tech Stack:** Python Cloudflare Worker, Cloudflare Durable Objects and WebSocket Hibernation, D1 channel authorization, vanilla ES modules, Web Crypto AES-GCM and ECDSA P-256, Three.js, pytest, Node syntax checks, and Playwright.

---

## Preconditions and Working Rules

- Work on branch `feat/chat-world` in the existing user-selected workspace.
- Run commands from `cloudflare_worker` unless a command explicitly changes into `browser_tests`.
- Follow `docs/superpowers/specs/2026-07-24-forkmesh-office-multiplayer-meeting-design.md` as the product and security contract.
- Preserve the user's existing `wrangler.toml` database-id change while adding only the new Durable Object binding and migration.
- Do not stage or rewrite the database-id line as part of feature commits.
- Do not run `no-mistakes axi` unless the user explicitly asks.
- Start each behavior slice with the closest end-user E2E or executable protocol test and confirm it fails for the intended missing behavior.
- Keep global World frames free of channel, message, attachment, key, membership, and typing fields.
- Keep Office meeting-presence frames free of message text, attachments, room keys, session tokens, and retained history.
- Keep the compact iframe mode as a fallback until the native meeting path passes the complete E2E matrix.
- Inspect every updated desktop, portrait, landscape, and 320 CSS pixel screenshot before accepting it.

## File Map

### New files

- `public/chat-crypto.js` contains shared byte conversion, PBKDF2, AES-GCM, attachment digest, and meeting-proof helpers.
- `public/chat-attachments.js` contains attachment limits, validation, encoding, normalization, and object URL lifecycle.
- `public/chat-room-transport.js` contains authorized room access, encrypted WebSocket lifecycle, replay handling, reconnect, and normalized events.
- `public/world/world-office-meeting.js` contains the Office lobby, dual-socket meeting lifecycle, room selection, seating requests, composer, transcript, and avatar-proof verification.
- `tests/test_world_office_protocol.py` covers pure Office meeting protocol validation.
- `tests/test_world_office_backend.py` covers ticket, route, Durable Object, revocation, and production binding contracts.
- `tests/test_chat_shared_modules_frontend.py` covers shared crypto, attachment, and transport modules.
- `tests/test_world_office_meeting_frontend.py` covers the meeting controller and scene integration contracts.
- `browser_tests/tests/world-office-meeting.spec.js` covers two-browser participant, seating, message, attachment, authorization, and responsive journeys.

### Existing files changed

- `src/world.py` gains pure Office presence validation and seat allocation helpers.
- `src/entry.py` gains Office tickets, routes, revocation fan-out, and `ForkMeshOfficeRoom`.
- `src/chat_channels_api.py` adds Office meeting access data to authorized room access and invokes combined room revocation.
- `wrangler.toml` gains `FORKMESH_OFFICE_ROOM` bindings and migration `v11` without changing the user's database id.
- `public/chat.html` loads `chat.js` as a module.
- `public/chat.js` consumes shared crypto, attachment, and transport modules while preserving full-chat behavior.
- `public/world/world-office.js` becomes the exterior and fallback controller rather than the primary iframe lifecycle.
- `public/world/world-scene.js` gains the Office lobby, meeting interior, room avatars, seats, seated poses, and bubble anchors.
- `public/world/world.js` wires global presence to the meeting controller and publishes only coarse Office activity.
- `public/world/world.css` gains lobby, room board, composer, transcript, bubble, seat, and responsive presentation.
- `public/world/index.html` preloads the meeting module through the existing module graph.
- Existing Python and Playwright tests are updated to replace dock-first assertions with meeting-room behavior while retaining fallback coverage.

## Task 1: Add Pure Office Meeting Protocol and Seat Allocation

**Files:**

- Create: `tests/test_world_office_protocol.py`
- Modify: `src/world.py`

- [ ] **Step 1: Write the failing protocol tests.**

Create tests for a bounded Office state, public-field projection, P-256 public JWK validation, movement rejection while seated, allowlisted chair ids, one-winner seat allocation, release, and forbidden field dropping.

```python
def test_office_presence_drops_chat_and_private_room_fields():
    current = world.default_office_presence("participant-1", 1000)
    kind, updated = world.sanitize_office_message({
        "type": "presence",
        "name": "Alice",
        "bindingKey": VALID_P256_JWK,
        "text": "secret",
        "channelId": "a" * 32,
        "roomKey": "secret-key",
        "attachment": {"name": "private.pdf"},
    }, current, 2000, trusted_name="Alice")

    assert kind == "presence"
    assert updated["name"] == "Alice"
    assert updated["bindingKey"] == VALID_P256_JWK
    serialized = repr(world.public_office_presence(updated))
    for forbidden in ("secret", "channelId", "roomKey", "attachment"):
        assert forbidden not in serialized


def test_two_participants_cannot_claim_the_same_chair():
    first = world.default_office_presence("first", 1000)
    second = world.default_office_presence("second", 1000)
    granted, first = world.allocate_office_seat(first, "chair-1", {})
    denied, second = world.allocate_office_seat(
        second, "chair-1", {"chair-1": "first"})

    assert granted == "granted"
    assert first["chairId"] == "chair-1"
    assert first["pose"] == "seated"
    assert denied == "denied"
    assert second["chairId"] == ""
    assert second["pose"] == "standing"
```

- [ ] **Step 2: Run the focused test and confirm the missing API failure.**

Run:

```bash
uv run pytest tests/test_world_office_protocol.py -q
```

Expected: failure because the Office protocol helpers do not exist.

- [ ] **Step 3: Add bounded Office protocol constants and state.**

Add these public contracts to `src/world.py`:

```python
OFFICE_MESSAGE_MAX_BYTES = 4096
OFFICE_RATE_WINDOW_MS = 1000
OFFICE_RATE_MAX_PER_WINDOW = 8
OFFICE_BROADCAST_WINDOW_MS = 1000
OFFICE_BROADCAST_MAX_PER_WINDOW = 64
OFFICE_CLIENT_STALE_MS = 90 * 1000
OFFICE_MAX_CONNECTIONS = 16
OFFICE_CHAIR_IDS = frozenset("chair-%d" % index for index in range(1, 9))
OFFICE_POSES = frozenset({"standing", "seated"})
OFFICE_PUBLIC_FIELDS = (
    "id", "name", "accountStatus", "x", "y", "z", "yaw",
    "moving", "pose", "chairId", "bindingKey", "updatedAt",
)
```

Implement `default_office_presence`, `public_office_presence`, `_office_binding_key`, `sanitize_office_message`, `allocate_office_seat`, `office_movement_delta`, `advance_office_rate_window`, `advance_office_broadcast_window`, and `office_presence_is_stale`.
Accept only uncompressed P-256 public JWK objects with exact `kty`, `crv`, `x`, and `y` fields, no private `d`, and bounded base64url coordinates.
Ignore movement frames while `pose == "seated"`.
Release uses an empty chair id and restores `pose == "standing"`.

- [ ] **Step 4: Run the protocol tests and the existing World backend tests.**

```bash
uv run pytest tests/test_world_office_protocol.py tests/test_world_backend.py -q
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit the protocol slice.**

```bash
git add tests/test_world_office_protocol.py src/world.py
git commit -m "feat: add Office meeting presence protocol"
```

## Task 2: Add Meeting Tickets, Authorized Routing, and Production Binding

**Files:**

- Create: `tests/test_world_office_backend.py`
- Modify: `src/entry.py`
- Modify: `wrangler.toml`

- [ ] **Step 1: Write failing ticket and binding tests.**

Cover ticket expiry, signature tampering, scope mismatch, authorization before `idFromName`, same-origin WebSocket requirements, opaque server-owned instance names, production and dev bindings, and migration `v11`.

```python
def test_office_ticket_is_short_lived_and_tamper_evident():
    token = namespace["_office_meeting_ticket"](
        env, "world-general", 1, "", "Guest 1234")
    claims = namespace["_office_meeting_ticket_claims"](env, token)
    assert claims["scope"] == "world-general"
    assert claims["version"] == 1
    assert namespace["_office_meeting_ticket_claims"](
        env, token[:-1] + ("0" if token[-1] != "0" else "1")) is None


def test_wrangler_registers_office_room_in_prod_and_dev():
    parsed = tomllib.loads(WRANGLER.read_text(encoding="utf-8"))
    assert {item["name"] for item in parsed["durable_objects"]["bindings"]} >= {
        "FORKMESH_OFFICE_ROOM",
    }
    assert {item["name"] for item in parsed["env"]["dev"]["durable_objects"]["bindings"]} >= {
        "FORKMESH_OFFICE_ROOM",
    }
    assert parsed["migrations"][-1] == {
        "tag": "v11",
        "new_sqlite_classes": ["ForkMeshOfficeRoom"],
    }
```

- [ ] **Step 2: Run the backend tests and confirm the missing route and class failures.**

```bash
uv run pytest tests/test_world_office_backend.py -q
```

- [ ] **Step 3: Add signed Office meeting tickets.**

Add `OFFICE_MEETING_TICKET_TTL_MS = 60 * 1000`, `_office_meeting_ticket`, and `_office_meeting_ticket_claims` beside the current chat-channel ticket helpers.
Use HMAC-SHA256 with the domain separator `office-meeting-ticket-v1`.
Claims contain only version tag, scope digest, key version, account blind index when authenticated, bounded display name, expiry, nonce, and signature.
Use URL-safe base64 for the bounded display name so dots cannot alter token fields.

- [ ] **Step 4: Add external access and WebSocket routes.**

Add these routes before generic static handling:

```text
GET /api/world/office/general/access
GET /api/world/office/general/ws?ticket=...
GET /api/world/office/channels/<channel-id>/ws?ticket=...
```

The access response is `no-store` and returns:

```json
{
  "room": {"id": "general", "name": "general", "visibility": "public"},
  "meetingWebSocketUrl": "/api/world/office/general/ws?ticket=...",
  "expiresAt": 1700000060000
}
```

The channel WebSocket handler validates the ticket, active account, channel key version, public visibility or membership, and same-origin upgrade before calling `FORKMESH_OFFICE_ROOM.idFromName`.
Forward a trusted internal claim header to the Durable Object rather than accepting identity from client JSON.

- [ ] **Step 5: Register the Durable Object without staging the user's database-id edit.**

Add prod and dev bindings:

```toml
[[durable_objects.bindings]]
name = "FORKMESH_OFFICE_ROOM"
class_name = "ForkMeshOfficeRoom"

[[env.dev.durable_objects.bindings]]
name = "FORKMESH_OFFICE_ROOM"
class_name = "ForkMeshOfficeRoom"

[[migrations]]
tag = "v11"
new_sqlite_classes = ["ForkMeshOfficeRoom"]
```

Stage only these hunks with a cached patch so the unrelated `database_id` line remains unstaged.

- [ ] **Step 6: Run backend and configuration tests.**

```bash
uv run pytest tests/test_world_office_backend.py tests/test_cloudflare_bootstrap_automation.py tests/test_pywrangler_bootstrap.py -q
```

- [ ] **Step 7: Commit the routing and binding slice.**

```bash
git add tests/test_world_office_backend.py src/entry.py
git diff --cached --check
git commit -m "feat: authorize Office meeting rooms"
```

Include only the new `wrangler.toml` binding and migration hunks in that commit.

## Task 3: Implement the Ephemeral Office Meeting Durable Object

**Files:**

- Modify: `tests/test_world_office_backend.py`
- Modify: `src/entry.py`

- [ ] **Step 1: Add failing Durable Object lifecycle tests.**

Use the established source-contract and fake-socket patterns to cover welcome snapshots, joins, bounded presence, movement, one-winner chair claims, release, seat cleanup on close, stale cleanup, rate limiting, capacity, binary rejection, and text-field rejection.

```python
def test_office_room_uses_hibernation_attachments_without_storage():
    source = _class_source("ForkMeshOfficeRoom")
    assert 'acceptWebSocket(server, to_js(["office"]))' in source
    assert 'getWebSockets("office")' in source
    assert "serializeAttachment" in source
    assert "ctx.storage.put" not in source
    assert "ctx.storage.get" not in source


def test_office_room_has_server_authoritative_seat_conflict_response():
    source = _class_source("ForkMeshOfficeRoom")
    assert "allocate_office_seat" in source
    assert '"type": "seat-denied"' in source
    assert '"message": "Seat just taken."' in source
```

- [ ] **Step 2: Confirm the new lifecycle tests fail.**

```bash
uv run pytest tests/test_world_office_backend.py -q
```

- [ ] **Step 3: Implement `ForkMeshOfficeRoom`.**

Mirror the proven hibernation lifecycle of `ForkMeshWorld`, but use the Office protocol helpers and tag sockets with `office`.
The socket attachment must hold only public Office presence, trusted account blind index and room scope for revalidation, liveness and rate counters, and departed state.
Derive occupied chairs only from currently live socket attachments.
Never use Durable Object storage.

Seat handling must follow this order:

```python
occupied = {
    _ws_attr(peer, "chairId"): _ws_attr(peer, "id")
    for peer in self._live_sockets(cleanup=True)
    if peer is not ws and _ws_attr(peer, "chairId", "")
}
result, next_state = world_protocol.allocate_office_seat(
    state, requested_chair, occupied)
if result == "denied":
    self._safe_send(ws, {
        "type": "seat-denied",
        "chairId": requested_chair,
        "message": "Seat just taken.",
    })
    return
```

Broadcast the resulting public participant state after a grant or release.
Mark an attachment departed before close so a hibernated closed socket cannot remain in snapshots or retain a seat.

- [ ] **Step 4: Add active-account and current-membership revalidation.**

Before processing a mutating frame, verify that an authenticated participant still has an active user record and current room access.
On failure close that socket with code `1008` and reason `room access revoked`.
Guest World `#general` participants skip D1 membership queries but retain normal liveness and abuse limits.

- [ ] **Step 5: Run the protocol and backend suites.**

```bash
uv run pytest tests/test_world_office_protocol.py tests/test_world_office_backend.py tests/test_world_backend.py -q
```

- [ ] **Step 6: Commit the Durable Object slice.**

```bash
git add tests/test_world_office_backend.py src/entry.py
git commit -m "feat: add ephemeral Office meeting presence"
```

## Task 4: Add Channel Meeting Access and Combined Revocation

**Files:**

- Modify: `tests/test_chat_channels_api.py`
- Modify: `tests/test_chat_channel_room_access.py`
- Modify: `src/chat_channels_api.py`
- Modify: `src/entry.py`

- [ ] **Step 1: Write failing authorized access and revocation tests.**

Extend the fake runtime so `room_access` returns `meetingWebSocketUrl` and so `revoke_room` records both encrypted chat and Office meeting revocation.

```python
assert access["data"]["meetingWebSocketUrl"].startswith(
    "/api/world/office/channels/" + channel_id + "/ws?ticket="
)
assert runtime.revoked_rooms == [(channel_id, 1)]
```

Add a source-order assertion that channel and membership authorization occurs before `FORKMESH_OFFICE_ROOM.idFromName`.

- [ ] **Step 2: Run the channel tests and confirm the missing field and revoke path failures.**

```bash
uv run pytest tests/test_chat_channels_api.py tests/test_chat_channel_room_access.py -q
```

- [ ] **Step 3: Extend `_ChatChannelsRuntime.room_access`.**

Issue an Office meeting ticket after the existing channel authorization succeeds and add `meetingWebSocketUrl` to the response.
Do not expose an internal Durable Object name or raw HMAC scope.

- [ ] **Step 4: Add Office room revocation fan-out.**

Implement `_revoke_office_channel_room` with the same two-attempt internal fetch pattern as `_revoke_chat_channel_room`.
Change `_ChatChannelsRuntime.revoke_room` and account membership cleanup to revoke both rooms with `asyncio.gather` or sequential awaited calls that fail closed.
The Office internal revoke path closes every live meeting socket in that channel version with code `1008`.

- [ ] **Step 5: Run channel, account-cleanup, and backend tests.**

```bash
uv run pytest \
  tests/test_chat_channels_api.py \
  tests/test_chat_channel_room_access.py \
  tests/test_account_device_cleanup.py \
  tests/test_world_office_backend.py -q
```

- [ ] **Step 6: Commit authorization integration.**

```bash
git add tests/test_chat_channels_api.py tests/test_chat_channel_room_access.py src/chat_channels_api.py src/entry.py
git commit -m "feat: connect channels to Office meetings"
```

## Task 5: Extract Shared Chat Crypto and Attachment Modules

**Files:**

- Create: `tests/test_chat_shared_modules_frontend.py`
- Create: `public/chat-crypto.js`
- Create: `public/chat-attachments.js`
- Modify: `public/chat.html`
- Modify: `public/chat.js`

- [ ] **Step 1: Write failing module and compatibility tests.**

Use Node Web Crypto to verify the existing PBKDF2/AES-GCM envelope round trip, deterministic normalized attachment digest, valid P-256 proof, altered-text proof rejection, private-key non-exportability, and exact attachment size limits.

```javascript
const pair = await createMeetingBinding();
const proof = await signMeetingProof(pair.privateKey, {
  participantId: "participant-1",
  messageId: "message-1",
  senderId: "sender-1",
  ts: 1700000000000,
  text: "hello",
  attachment: null,
});
const valid = await verifyMeetingProof(pair.publicJwk, proof, {
  messageId: "message-1",
  senderId: "sender-1",
  ts: 1700000000000,
  text: "hello",
  attachment: null,
});
const altered = await verifyMeetingProof(pair.publicJwk, proof, {
  messageId: "message-1",
  senderId: "sender-1",
  ts: 1700000000000,
  text: "altered",
  attachment: null,
});
process.stdout.write(JSON.stringify({ valid, altered }));
```

Expected: `{ "valid": true, "altered": false }` after implementation.

- [ ] **Step 2: Run the shared-module tests and confirm files are missing.**

```bash
uv run pytest tests/test_chat_shared_modules_frontend.py -q
```

- [ ] **Step 3: Extract shared crypto without changing wire format.**

Move byte conversion, PBKDF2 key derivation, AES-GCM envelope encryption, and envelope decryption into `public/chat-crypto.js` as named exports.
Keep the existing `{kind:"cipher",v:1,nonce,tag,body}` wire shape byte-for-byte compatible.

Add `createMeetingBinding`, `normalizeMeetingContent`, `meetingContentDigest`, `signMeetingProof`, and `verifyMeetingProof`.
Use ECDSA P-256 with SHA-256.
Generate the private key with `extractable: false` and usages `sign`; export only the public JWK.
Reject participant ids, message ids, sender ids, and timestamps outside explicit bounds before signing or verifying.

- [ ] **Step 4: Extract attachment normalization.**

Move the current attachment byte, MIME, filename, image, document, and object URL rules into `public/chat-attachments.js`.
Export the exact limits used by both full chat and Office.
Normalize a digest record containing filename, MIME type, byte length, and SHA-256 content digest for meeting proofs.

- [ ] **Step 5: Convert full chat to an ES module consumer.**

Change the final script to:

```html
<script type="module" src="/chat.js"></script>
```

Import the shared helpers at the top of `chat.js`, remove only duplicated implementations, and retain existing function names through imports or thin wrappers so established chat rendering code remains stable.

- [ ] **Step 6: Run focused Python, Node, and existing chat E2E tests.**

```bash
uv run pytest \
  tests/test_chat_shared_modules_frontend.py \
  tests/test_chat_attachments_frontend.py \
  tests/test_dashboard_chat_persist_frontend.py \
  tests/test_chat_office_embed_frontend.py -q
node --check public/chat-crypto.js
node --check public/chat-attachments.js
node --check public/chat.js
cd browser_tests && npx playwright test tests/chat-persistence.spec.js tests/chat-attachments.spec.js
```

- [ ] **Step 7: Commit the shared crypto and attachment slice.**

```bash
git add tests/test_chat_shared_modules_frontend.py public/chat-crypto.js public/chat-attachments.js public/chat.html public/chat.js
git commit -m "refactor: share encrypted chat primitives"
```

## Task 6: Extract the Shared Authorized Room Transport

**Files:**

- Modify: `tests/test_chat_shared_modules_frontend.py`
- Create: `public/chat-room-transport.js`
- Modify: `public/chat.js`

- [ ] **Step 1: Add failing transport lifecycle tests.**

Use a fake WebSocket and fetch implementation to cover public room-key access, private room access, encrypted replay, durable flags, reconnect, explicit suspend, authorization close, no duplicate socket, and disposal.

```javascript
const transport = createChatRoomTransport({
  fetchImpl,
  WebSocketImpl: FakeWebSocket,
  onPlain: (plain) => events.push(plain),
  onState: (state) => states.push(state),
});
await transport.connect({ kind: "public", id: "general" });
await transport.send({ type: "chat", id: "m1", channel: "#general", text: "hello" });
transport.suspend();
process.stdout.write(JSON.stringify({ events, states, sockets: FakeWebSocket.instances.length }));
```

- [ ] **Step 2: Confirm the module-missing failure.**

```bash
uv run pytest tests/test_chat_shared_modules_frontend.py -q
```

- [ ] **Step 3: Implement `createChatRoomTransport`.**

Expose `connect(room)`, `send(plain, {persist})`, `suspend()`, `resume()`, `dispose()`, and read-only `state`, `room`, and `socket` accessors.
Keep room-key material and decrypted messages in closure memory only.
Normalize states to `idle`, `authorizing`, `connecting`, `connected`, `reconnecting`, `unauthorized`, and `disposed`.
Never reconnect after `suspend` or `dispose`.
Fetch new private authorization before reconnect after code `1008`.

- [ ] **Step 4: Migrate `chat.js` socket ownership.**

Replace direct socket construction, room-key caches, reconnect timers, and `onFrame` ownership with the shared transport callbacks.
Keep chat-specific channel selection, roster, UI rows, reactions, edits, deletes, ForkBot, and admin controls in `chat.js`.
Ensure the Office iframe fallback still maps `office-chat-suspend` to `transport.suspend()`.

- [ ] **Step 5: Run the complete existing chat verification matrix.**

```bash
uv run pytest \
  tests/test_chat_shared_modules_frontend.py \
  tests/test_private_chat_channels_frontend.py \
  tests/test_chat_attachments_frontend.py \
  tests/test_dashboard_chat_persist_frontend.py \
  tests/test_chat_office_embed_frontend.py \
  tests/test_web_chat_connect_diagnostics_frontend.py -q
node --check public/chat-room-transport.js
node --check public/chat.js
cd browser_tests && npx playwright test \
  tests/chat-persistence.spec.js \
  tests/chat-attachments.spec.js \
  tests/private-chat-channels.spec.js
```

- [ ] **Step 6: Commit the transport slice.**

```bash
git add tests/test_chat_shared_modules_frontend.py public/chat-room-transport.js public/chat.js
git commit -m "refactor: share authorized chat transport"
```

## Task 7: Build the Office Lobby, Interior, Avatars, and Seats

**Files:**

- Create: `tests/test_world_office_meeting_frontend.py`
- Create: `public/world/world-office-meeting.js`
- Modify: `public/world/world-office.js`
- Modify: `public/world/world-scene.js`
- Modify: `public/world/world.js`
- Modify: `public/world/world.css`

- [ ] **Step 1: Write a failing end-user lobby and seat E2E test.**

Start `browser_tests/tests/world-office-meeting.spec.js` with mocked global and meeting sockets.
Walk to the Office, press `E`, assert the lobby appears without the chat iframe, choose `#general`, assert the interior and remote avatar appear, click `Chair 1`, and assert the seated state only after the server `seat` frame.

- [ ] **Step 2: Add failing static scene and controller contracts.**

Require these narrow APIs:

```javascript
scene.enterOfficeLobby()
scene.enterOfficeMeeting({ roomName, participants })
scene.setOfficeParticipants(participants)
scene.setOfficeSeatState({ participantId, chairId, pose })
scene.showOfficeBubble(participantId, bubble)
scene.leaveOfficeInterior()
```

Require the controller to expose `openLobby`, `joinRoom`, `requestSeat`, `stand`, `leaveRoom`, `leaveOffice`, and `destroy`.

- [ ] **Step 3: Run the new tests and confirm the lobby/interior APIs are absent.**

```bash
uv run pytest tests/test_world_office_meeting_frontend.py -q
cd browser_tests && npx playwright test tests/world-office-meeting.spec.js --grep "join and sit"
```

- [ ] **Step 4: Convert the exterior controller.**

Keep `nextOfficeZoneState`, exterior focus, `E` entry, focus restoration, and fallback iframe validation in `world-office.js`.
Replace automatic dock opening with a callback to `meeting.openLobby()`.
Open the iframe only from an explicit `Open accessible chat fallback` action and never while the native transport is connected.

- [ ] **Step 5: Add the lobby controller and room board.**

`world-office-meeting.js` fetches `/api/world/office/general/access` for guests and `/api/chat/channels` for registered room listings.
It renders World `#general` first, then authorized public and private channels with non-color visibility labels.
Joining a channel fetches existing room access and consumes `meetingWebSocketUrl` from that authorized response.

- [ ] **Step 6: Build the reusable interior scene.**

Add a dedicated Office interior group with walls, windows, warm lights, meeting table, eight chair meshes, room display, entrance, and exit.
Use a scene mode rather than placing the interior on top of the town-square coordinate space.
Hide town landmarks and town avatars while interior mode is active, and restore them on exit.

Each chair mesh receives:

```javascript
chair.userData.officeChairId = `chair-${index + 1}`;
chair.userData.interactive = "office-chair";
```

Remote meeting participants use the existing avatar factory but a separate `officeParticipants` map.
Seated participants are pinned to deterministic chair transforms and use a bent-leg seated pose.

- [ ] **Step 7: Wire room-local presence and seat requests.**

The controller connects the meeting WebSocket only after room authorization.
It handles `welcome`, `join`, `presence`, `move`, `seat`, `seat-denied`, and `leave` frames.
It sends only allowlisted meeting presence, movement, seat request, and ping frames.
Global presence changes only the coarse activity category to `visiting-office` when consent allows.

- [ ] **Step 8: Run lobby, scene, world, and join-and-sit E2E tests.**

```bash
uv run pytest \
  tests/test_world_office_meeting_frontend.py \
  tests/test_world_office_frontend.py \
  tests/test_world_frontend.py -q
node --check public/world/world-office.js
node --check public/world/world-office-meeting.js
node --check public/world/world-scene.js
node --check public/world/world.js
cd browser_tests && npx playwright test tests/world-office-meeting.spec.js --grep "join and sit"
```

- [ ] **Step 9: Commit the spatial meeting shell.**

```bash
git add \
  tests/test_world_office_meeting_frontend.py \
  browser_tests/tests/world-office-meeting.spec.js \
  public/world/world-office.js \
  public/world/world-office-meeting.js \
  public/world/world-scene.js \
  public/world/world.js \
  public/world/world.css
git commit -m "feat: add spatial Office meeting rooms"
```

## Task 8: Add Native Composer, Verified Bubbles, Attachments, and Transcript

**Files:**

- Modify: `tests/test_world_office_meeting_frontend.py`
- Modify: `public/world/world-office-meeting.js`
- Modify: `public/world/world-scene.js`
- Modify: `public/world/world.js`
- Modify: `public/world/world.css`
- Modify: `browser_tests/tests/world-office-meeting.spec.js`

- [ ] **Step 1: Write failing two-browser message and forgery E2E tests.**

Use two browser contexts connected to one mocked meeting room and encrypted chat relay.
Send from participant A and assert participant B sees the bubble above A's avatar plus the transcript entry.
Alter the signed text in a forged encrypted frame and assert it appears only in the transcript with `Remote channel participant`, never above A.

- [ ] **Step 2: Write failing clipboard image and document E2E tests.**

Paste a PNG into the native composer, attach a PDF, and assert encrypted durable frames contain neither raw filenames nor plaintext before decryption.
After replay, assert the image thumbnail and document chip return in the transcript.

- [ ] **Step 3: Add the native composer and shared transport.**

Create the transport only after both chat and meeting authorization succeed.
Generate one ephemeral meeting binding per room join and publish its public JWK through meeting presence.
On send, create the normal chat plaintext, calculate and attach `meetingProof`, then call the shared transport with `persist: true`.
Clear the composer only after encryption succeeds.

- [ ] **Step 4: Verify proofs before scene attachment.**

For every decrypted chat message:

1. Render it in the authorized transcript using existing normalization rules.
2. Find the live meeting participant matching `meetingProof.participantId`.
3. Verify timestamp bounds, message fields, attachment digest, and ECDSA signature against that participant's published key.
4. Call `scene.showOfficeBubble` only when every check succeeds.

Never infer an avatar from display-name equality.

- [ ] **Step 5: Implement bounded bubbles and attachment presentation.**

Use DOM elements projected from avatar anchor positions.
Limit scene text to 180 characters, one visible bubble, one queued bubble, and seven seconds per bubble.
Image previews use object URLs from already decrypted validated bytes and a bounded thumbnail.
Document bubbles show only validated filename and file type.
Dispose object URLs when a transcript row, bubble, room, or controller is removed.

- [ ] **Step 6: Add the semantic transcript and moderation convergence.**

Mirror new messages, edits, deletes, reactions, and attachment state into one chronological transcript.
Use one polite live region that announces a message once.
Messages without a valid live participant proof remain in the transcript and never create synthetic avatars.

- [ ] **Step 7: Run frontend, crypto, and meeting E2E tests.**

```bash
uv run pytest \
  tests/test_world_office_meeting_frontend.py \
  tests/test_chat_shared_modules_frontend.py \
  tests/test_chat_attachments_frontend.py -q
node --check public/world/world-office-meeting.js
node --check public/world/world-scene.js
cd browser_tests && npx playwright test tests/world-office-meeting.spec.js
```

- [ ] **Step 8: Commit the conversation slice.**

```bash
git add \
  tests/test_world_office_meeting_frontend.py \
  browser_tests/tests/world-office-meeting.spec.js \
  public/world/world-office-meeting.js \
  public/world/world-scene.js \
  public/world/world.js \
  public/world/world.css
git commit -m "feat: show verified chat bubbles in Office meetings"
```

## Task 9: Complete Authorization, Recovery, Accessibility, and Visual E2E

**Files:**

- Modify: `browser_tests/tests/world-office-meeting.spec.js`
- Modify: `browser_tests/tests/world.spec.js`
- Add snapshots under: `browser_tests/tests/world-office-meeting.spec.js-snapshots/`
- Modify: `tests/test_world_office_meeting_frontend.py`
- Modify: `tests/test_world_frontend.py`

- [ ] **Step 1: Add the complete authorization matrix.**

Cover guest, registered public, invited private, uninvited private, admin, expired session, deleted channel, and disabled account.
Assert uninvited private rooms are absent rather than disabled.
Assert revocation closes both sockets, removes the avatar, releases the chair, clears bubbles, and returns the visitor to the lobby.

- [ ] **Step 2: Add reconnect and failure journeys.**

Cover meeting-only failure, chat-only failure, reconnect backoff, refresh, room full, seat race, signature failure, invalid image, and WebGL fallback.
Assert fallback iframe and native transport never have simultaneous room sockets.

- [ ] **Step 3: Add keyboard and accessibility journeys.**

Cover `E`, Enter, Shift+Enter, Escape ordering, room-board focus, chair buttons, participant status, one live announcement per message, focus restoration, and reduced motion.

- [ ] **Step 4: Add visual snapshots and geometry assertions.**

Capture exterior, lobby, standing, seated, simultaneous bubbles, image bubble, document bubble, transcript, room-full, and reconnect states for:

- desktop `1440x900`
- portrait `390x844`
- landscape `844x390`
- narrow `320x568`

Assert no horizontal overflow, composer clear of safe areas, exit control visible, projected bubbles bounded to the viewport, and transcript within its percentage cap.

- [ ] **Step 5: Run and inspect the visual suite.**

```bash
cd browser_tests
npx playwright test tests/world-office-meeting.spec.js --update-snapshots
npx playwright test tests/world-office-meeting.spec.js
```

Open every generated snapshot and reject overlaps, clipping, unreadable signs, bubble collisions, hidden exits, covered avatars, and mobile safe-area problems.

- [ ] **Step 6: Run existing World regression E2E.**

```bash
cd browser_tests
npx playwright test tests/world.spec.js
```

- [ ] **Step 7: Commit E2E and visual baselines.**

```bash
git add browser_tests/tests/world-office-meeting.spec.js browser_tests/tests/world.spec.js browser_tests/tests/*-snapshots
git commit -m "test: cover multiplayer Office meeting journeys"
```

## Task 10: Final Verification, Privacy Audit, and Branch Completion

**Files:**

- Review all files changed by Tasks 1 through 9.
- Do not modify generated changelogs.

- [ ] **Step 1: Run focused Python verification.**

```bash
uv run pytest \
  tests/test_world_office_protocol.py \
  tests/test_world_office_backend.py \
  tests/test_world_backend.py \
  tests/test_world_frontend.py \
  tests/test_world_office_frontend.py \
  tests/test_world_office_meeting_frontend.py \
  tests/test_chat_shared_modules_frontend.py \
  tests/test_chat_channels_api.py \
  tests/test_chat_channel_room_access.py \
  tests/test_private_chat_channels_frontend.py \
  tests/test_chat_attachments_frontend.py \
  tests/test_dashboard_chat_persist_frontend.py \
  tests/test_chat_office_embed_frontend.py -q
```

- [ ] **Step 2: Run JavaScript syntax verification.**

```bash
node --check public/chat-crypto.js
node --check public/chat-attachments.js
node --check public/chat-room-transport.js
node --check public/chat.js
node --check public/world/world-data.js
node --check public/world/world-office.js
node --check public/world/world-office-meeting.js
node --check public/world/world-scene.js
node --check public/world/world.js
```

- [ ] **Step 3: Run Playwright regression verification.**

```bash
cd browser_tests
npx playwright test \
  tests/world.spec.js \
  tests/world-office-meeting.spec.js \
  tests/private-chat-channels.spec.js \
  tests/chat-attachments.spec.js \
  tests/chat-persistence.spec.js
```

- [ ] **Step 4: Audit privacy and obsolete behavior.**

```bash
rg -n "channelId|privateChannelId|roomKey|passphrase|plaintext|attachment|token|typing" \
  public/world src/world.py
rg -n "data-world-office-chat|openWorldChat|/dashboard/chat" \
  public/world tests browser_tests/tests
```

Review every match.
The first search may find local controller names and explicit forbidden-field tests, but no global World or Office presence payload may contain those values.
The second search may find only the explicit accessible fallback and no dock-first automatic flow.

- [ ] **Step 5: Check the diff and preserve unrelated work.**

```bash
git diff --check
git status --short --branch
git diff -- wrangler.toml
git log --oneline -16
```

Confirm the user's database-id change remains present and unstaged from feature commits.
Confirm no generated file or unrelated user change is included.

- [ ] **Step 6: Commit any final scoped corrections.**

```bash
git add -p
git commit -m "fix: harden multiplayer Office meetings"
```

Skip this commit when verification requires no correction.

- [ ] **Step 7: Leave the completed branch ready for user review.**

Do not merge, push, or run `no-mistakes axi` without explicit user direction.
Report the branch name, commits, verification commands and results, production Durable Object migration requirement, and the preserved unrelated `wrangler.toml` database-id change.
