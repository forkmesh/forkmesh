# Chat relay hardening

Shared rooms such as `world-general` are joined by every device on an
instance, guests included, and run the `open` trust policy, so a frame sealed
"to the room" is readable by everyone in it. This page is the contract every
client (iOS, Android, desktop, web) and the server follow so that a shared
room only ever carries public content, and so that nobody can speak, edit or
delete as somebody else, followed for R3 to R6 by how the App implements the
server half. Part of [the App docs](README.md). Protocol pages:
[chat-protocol-v3.md](chat-protocol-v3.md), [chat-protocol-v4.md](chat-protocol-v4.md);
client sides: [web](web-chat.md#relay-hardening-in-the-browser),
[desktop](../desktop/chat.md), [Android](../native/ANDROID.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[chat-direct-calls.md](chat-direct-calls.md),
[chat-unread.md](chat-unread.md). Tests:
`app/tests/test_chat_relay_hardening.py`.

Nothing here keeps compatibility with older builds: a client without these
rules is not understood by one with them.

## R1. Bundles are bound to the node key

In every room whose policy is `open` (relay rooms, channel and office rooms,
`#mesh`), a device's node id (`senderId`, the bundle's `node`) is the `b64u`
of its 32-byte Ed25519 node public key, and its `e2ee-key` bundle carries one
more field:

```
"nsig": "<b64u 64>"   Ed25519 by the node key over
forkmesh-chat-e2ee-node-v1\n<room>\n<kid>\n<idk>\n<acct>
```

A receiver drops a bundle in an open room unless `node` decodes to exactly 32
bytes and `nsig` verifies under it. A `kid` already stored keeps its `node`, so
a second device cannot claim it. Strict (direct-message) rooms keep the v3
rules: the server only admits the two participants' devices.

Vectors: [`conformance/chat/v3/node-binding.json`](../../conformance/chat/v3/node-binding.json)
(seed, inputs, message, `nsig`, and three bundles that must be rejected).

Web browsers get their own Ed25519 node key, kept like the chat identity key
(IndexedDB, non-extractable where the browser allows), one per browser and
signed-in account; it is the browser's `senderId` from then on.

## R2. Shared rooms carry nothing private

Clients neither send nor accept, on a relay room:

- `type:"dm"`, or any frame whose `conversation` starts with `@`;
- any frame with `private: true`, and `invite` frames;
- `cove-invite` and `cove-open`.

Instead:

- **Direct messages** go through the server's direct-message rooms
  (`/api/chat/direct-messages`, strict policy, participants checked by the
  server). "Message" on a person opens that direct conversation for their
  account; a peer with no account (a guest) has no Message action.
- **Private channels** are the server's private channels
  (`POST /api/chat/channels` with `visibility: "private"`, members checked by
  the server). Creating a private channel calls that API.
- **Cove invites** are sent as a direct message to the invitee.

On first start with these rules a client drops relay private channels and
`@peer` conversations from its saved history, with their threads, reactions
and edits. The LAN `#mesh` room keeps its own sealed per-device direct
messages.

Every frame addressed with `to` other than a `hello` reply is sealed to the
recipient's keys only.

## R3. Only authenticated frames act for a person

- A person's frames are sealed (v3 or v4) in every room, public channels
  included; the sealed sender's stored `node` must equal `senderId`.
- Edits, deletes, reactions, stickers, looks and thread replies are applied
  only from sealed frames, and an edit or delete only when the authenticated
  sender is the target message's author.
- An unsealed frame is accepted only when the relay marks it as injected by
  the server (`"srv": true` on the envelope, set by the server for webhook,
  app and system frames; the relay strips `srv` from any client frame). It is
  shown as the integration it names, never as a person. This holds in public
  and private channel rooms (webhook-fed rooms such as the private
  `#email-log` notices channel, [mail log](../operations/mail-log.md)), never in
  direct-message rooms; a frame with `serverVisible` is labelled
  server-visible.

### Server: frames are marked `srv`

`ForkMeshRoom` (`app/src/entry.py`) sets `"srv": true` on every frame the
server itself puts into a room:

- channel webhook deliveries and the Everyone channel welcome
  (`_channel_webhook`, reached through `deliver_webhook` /
  `_chat_channel_deliver`);
- agent guestbook notes in `world-general` (`_agent_guestbook`);
- the "someone just joined" notice (`_new_member`).

The mark is stored with the retained copy, so a replayed webhook message keeps
it. Every client frame loses `srv` before it is relayed, retained or acked:
`CHAT_RELAY_OWNED_FIELDS` (`historyReplay`, `recipients`, `recipientNames`,
`ref`, `srv`) is stripped on the normal lane and on the live direct-message
lane. A client therefore cannot pass an unsealed frame off as an integration.
Relay control frames (`forkmesh-history-end`, `forkmesh-recipients`, server
copies, read and delivered notices) are not chat messages and carry no mark.

## R4. Account names are verified by the server

A frame's `acct` / `ownerUser` is shown as an account only after the client
checked it with `GET /api/chat/node-owner?node=<b64u>`, which returns
`{"account": "<name>"}` for a node key bound to an account (a phone's or
desktop's device bind at sign-in, or a browser's
`POST /api/chat/node-bind`), or `{"account": ""}`. A claim that does not
match is shown as a guest. Answers are cached per node for an hour.

### Server: node owners

- `GET /api/chat/node-owner?node=<b64u 43>` answers
  `{"account": "<name>"}` or `{"account": ""}` (`chat_node_owner_handler`).
  It needs no session, because guests show names too. A node key belongs to
  an account when exactly one account has it as an enabled, unrevoked
  `account_devices` row of kind `desktop_node`, `mobile_node` (the device bind
  a phone or desktop signs at login, `forkmesh-device-bind-v1`) or
  `chat_node`, or as a `nodes.pubkey` (the desktop's primary key). Two
  accounts claiming one key, a revoked row, a disabled account or a non-user
  account all answer `""`. Bad input is `400 invalid_node`. The answer is
  `Cache-Control: private, max-age=600`.
- Rate limit: 120 lookups a minute per client address per app process
  (`CHAT_NODE_OWNER_MAX_PER_WINDOW`), then `429` with `retry-after`. Clients
  cache answers for an hour, so a busy room costs one lookup per new device.
- `POST /api/chat/node-bind` (`chat_node_bind_handler`) takes
  `{"node", "ts", "sig"}` with a signed-in session (bearer token, or the
  session cookie from an allowed origin). `sig` is Ed25519 by the node key over
  `forkmesh-chat-node-bind-v1\n<account>\n<node>\n<ts>`; `ts` is milliseconds
  and must be within 5 minutes (`LOGIN_MAX_SKEW_MS`). It stores an
  `account_devices` row of kind `chat_node` with the single capability
  `chat_node`, so the key can name a chat sender but signs nothing else
  (`_git_push_account_for_pubkey` ignores it). An account keeps at most 20
  (`CHAT_NODE_MAX_DEVICES`); the least recently seen is revoked first. A key
  already bound to another account is `409 node_key_conflict`; a key the
  account already holds as a phone or desktop key answers `200` unchanged.
  Answers: `200 {"ok": true, "account", "node"}`, `401 stale_request`,
  `401 invalid_signature`, `401 device_revoked`, `400 invalid_node`.
  `chat_node` is listed in `ACCOUNT_DEVICE_KINDS`, so the account's device
  list can revoke it like any other device.

## R5. Blocks are kept by the server

Clients push their block list on every block or unblock and read it at
sign-in. The server refuses to start a direct conversation with someone who
blocked the caller (404, as if the account did not exist), and sends no
message push, call ring or VoIP push from a blocked account.

### Server: blocks

- `GET /api/chat/blocks` returns `{"accounts": [...]}`; `PUT /api/chat/blocks`
  with `{"accounts": [...]}` replaces the list and returns
  `{"ok": true, "accounts": [...]}` (trimmed, lowercased, sorted, without
  duplicates or the caller's own name). At most 500 names
  (`400 too_many_accounts`); anything that is not a list of valid account
  names is `400 invalid_accounts`. Names need not exist, so the endpoint
  reveals nothing about which accounts do.
- Storage: table `chat_blocks` (migration 0286), one row per account:
  `account_bi` and the list sealed in `data`. An empty list deletes the row.
  `tools/next_rekey.py` and [rekey](../operations/rekey.md) list the table.
- Desktops without a session sign `forkmesh-chat-blocks-list-v1\n<node>\n<ts>`
  for `GET` and `forkmesh-chat-blocks-put-v1\n<node>\n<ts>\n<sha256>` for
  `PUT`, where the digest is SHA-256 of the cleaned names joined by `\n`
  (`blocks_digest`), with `node`, `ts`, `sig` in the query like the other
  direct-message proofs.
- Enforcement: starting a direct conversation with someone who blocked the
  caller answers exactly what a missing account answers
  (`404 user_not_found`), also when the conversation already exists.
  `_chat_direct_message_retained` sends no message event or push and
  `_chat_direct_call_ring` sends no call event, alert or VoIP push when the
  recipient blocked the sender (`_chat_blocked_by`). The unread direct-message
  email leaves blocked senders out. Channel pushes are not filtered: a
  channel's audience is every member, and looking up each member's list on
  every message was not worth it while clients hide blocked senders anyway.
  A failed lookup counts as "not blocked", so a database hiccup never drops a
  real message's push.

## R6. The relay admits only known rooms

The relay routes only the room names it serves (`world-general`, `general`
and the public `world-*` spaces) plus the server's channel, office and
direct-message rooms; any other room name is a 404, and the legacy
`/api/room/<name>/ws` path is gone. It caps sockets per client address, does
not refresh a socket's last-seen time for a frame that is not a valid
envelope, and retains history outside `world-general` only for sockets bound
to an account. Room keys under `mainnode/forkmesh` are derived per room name
(version 2).

### Server: room admission

- `room_key_from_path` admits only `mainnode/forkmesh` rooms named in
  `CHAT_RELAY_ROOMS`: `world-general`, `general` and `world-<space>` for the
  dashboard's public spaces (`CHAT_RELAY_SPACES`: sky-campus, space-station,
  code-planet, organization-region, planet-atlas, neighborhood, broadcast),
  plus channel and direct-message rooms. Every other `/api/repo/.../rooms/...`
  path is `404` before any room object
  ([cron-and-durable-objects.md](cron-and-durable-objects.md)) is touched, and
  the room object itself refuses a socket for a path it does not know: an
  open name space would let anyone create unbounded rooms with retained
  history.
- There is no legacy `/api/room/<name>/ws` route (no `ROOM_RE` in `urls.py`,
  no `compatWebsocket` field in `/api/mainnode`).
- Socket caps: the App hands the room a keyed address token
  (`_chat_socket_address`: HMAC of the `cf-connecting-ip` header the host sets, cut to
  the /64 for IPv6, never the raw address) as `addr`. One address may hold 8
  sockets in one room (`ROOM_SOCKETS_PER_ADDRESS`) and 32 across the rooms of
  one process (`CHAT_SOCKETS_PER_ADDRESS`); more is `429 too_many_sockets`.
  Without them one address could fill a room's 128 (`MAX_CONNECTIONS`); the
  numbers leave room for a household or office behind one NAT.
- The App rebuilds the socket URL for the room object
  (`_chat_socket_target`): only `account` (resolved by the App) and `addr`
  survive, so a client cannot claim an account or an address in its query.
  The WebSocket handoff validator accepts only those two keys
  (`_chat_socket_query_ok`).
- For `general` and the `world-*` rooms the App binds the socket to an
  account when the upgrade carries a session (bearer or cookie) or a desktop's
  `forkmesh-room-key-v2` signature (`node`, `ts`, `sig`, redacted from the
  access log). History outside `world-general` is retained only for frames
  from a socket bound to an account; a guest's frames there are relayed live
  but not kept, so a guest cannot fill a room's 16 MiB history.
- A frame that is not a JSON object of kind `cipher`, `forkmesh-received` or
  `forkmesh-superseded` is dropped before the rate limiter, so it neither
  counts nor refreshes the socket's last-seen time, so junk frames cannot keep
  a ghost socket alive in the roster.
- Room keys: `GET /api/chat/room-key` serves only the admitted rooms (others
  `404`), and every `mainnode/forkmesh` room except `world-general` has its own
  key, `SHA-256(DATA_KEY + ":room-chat-passphrase-v2:mainnode/forkmesh:room:"
  + room)`; the response carries `"version": 2` (`world-general` keeps its
  public key and `"version": 1`), so a key fetched for one room reads no
  other. Clients
  fetch the key from the server; none derives it locally (the baked-in
  `forkmesh-shared-room-key-v1` in the phone and desktop `RoomCrypto` is only
  the LAN `#mesh` room).

## R7. Less about you goes to strangers

In shared rooms clients send no `timeZone`, no Solana address unless the user
turned tips on, no host name as a node name (only a name the user set), and
no diagnostics (`dg`). `#mesh`: bundles carry `acct` = node id, frames carry
no `ownerUser`, a device does not re-seal its earlier messages to keys it has
not seen before, the beacon id is random per network, the desktop beacon does
not carry its node key, and the UI says that anyone nearby can read `#mesh`.

## Push and email

- Call pushes do not name the caller: the alert body is "Incoming call"
  and neither the alert nor the VoIP payload has a `caller` field
  ([chat-direct-calls.md](chat-direct-calls.md)).
- Unread reminder emails name no private room and no sender
  ([chat-unread.md](chat-unread.md)).
