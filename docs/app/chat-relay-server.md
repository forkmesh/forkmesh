# Chat relay hardening: the server side

How the App implements the server half of
[chat-relay-hardening.md](chat-relay-hardening.md) (R3 to R6), and why.
Home page: [README.md](README.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[chat-direct-calls.md](chat-direct-calls.md),
[chat-unread-reminders.md](chat-unread-reminders.md).
Tests: `app/tests/test_chat_relay_hardening.py`.

## R3. Server frames are marked `srv`

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

## R4. Node owners

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
- Rate limit: 120 lookups a minute per client address per isolate
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

## R5. Blocks

- `GET /api/chat/blocks` returns `{"accounts": [...]}`; `PUT /api/chat/blocks`
  with `{"accounts": [...]}` replaces the list and returns
  `{"ok": true, "accounts": [...]}` (trimmed, lowercased, sorted, without
  duplicates or the caller's own name). At most 500 names
  (`400 too_many_accounts`); anything that is not a list of valid account
  names is `400 invalid_accounts`. Names need not exist, so the endpoint
  reveals nothing about which accounts do.
- Storage: table `chat_blocks` (migration 0284), one row per account:
  `account_bi` and the list sealed in `data`. An empty list deletes the row.
  `tools/next_rekey.py` and [REKEY.md](../REKEY.md) list the table.
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

## R6. Room admission

- `room_key_from_path` admits only `mainnode/forkmesh` rooms named in
  `CHAT_RELAY_ROOMS`: `world-general`, `general` and `world-<space>` for the
  dashboard's public spaces (`CHAT_RELAY_SPACES`: sky-campus, space-station,
  code-planet, organization-region, planet-atlas, neighborhood, broadcast),
  plus channel and direct-message rooms. Every other `/api/repo/.../rooms/...`
  path is `404` before any Durable Object is touched, and the room object
  itself refuses a socket for a path it does not know. No client used another
  repository's room, and an open name space let anyone create unbounded
  rooms with retained history.
- The legacy `/api/room/<name>/ws` route is gone (`ROOM_RE` removed from
  `urls.py`, the router, the WebSocket handoff and status bucketing; the
  `compatWebsocket` field left `/api/mainnode`). No client called it.
- Socket caps: the Worker hands the room a keyed address token
  (`_chat_socket_address`: HMAC of the `cf-connecting-ip` the host sets, cut to
  the /64 for IPv6, never the raw address) as `addr`. One address may hold 8
  sockets in one room (`ROOM_SOCKETS_PER_ADDRESS`) and 32 across the rooms of
  one process (`CHAT_SOCKETS_PER_ADDRESS`); more is `429 too_many_sockets`.
  The room limit of 128 (`MAX_CONNECTIONS`) otherwise let one address fill a
  room. The numbers leave room for a household or office behind one NAT.
- The Worker rebuilds the socket URL for the room object
  (`_chat_socket_target`): only `account` (resolved by the Worker) and `addr`
  survive, so a client cannot claim an account or an address in its query.
  The WebSocket handoff validator accepts only those two keys
  (`_chat_socket_query_ok`).
- For `general` and the `world-*` rooms the Worker binds the socket to an
  account when the upgrade carries a session (bearer or cookie) or a desktop's
  `forkmesh-room-key-v2` signature (`node`, `ts`, `sig`, redacted from the
  access log). History outside `world-general` is retained only for frames
  from a socket bound to an account; a guest's frames there are relayed live
  but not kept, so a guest cannot fill a room's 16 MiB history.
- A frame that is not a JSON object of kind `cipher`, `forkmesh-received` or
  `forkmesh-superseded` is dropped before the rate limiter, so it neither
  counts nor refreshes the socket's last-seen time: junk frames used to keep a
  ghost socket alive and counted in the roster.
- Room keys: `GET /api/chat/room-key` serves only the admitted rooms (others
  `404`), and every `mainnode/forkmesh` room except `world-general` has its own
  key, `SHA-256(DATA_KEY + ":room-chat-passphrase-v2:mainnode/forkmesh:room:"
  + room)`; the response carries `"version": 2` (`world-general` keeps its
  public key and `"version": 1`). Before, `general` and every other room
  shared one key, so a key fetched for one room read all of them. Clients
  fetch the key from the server; none derives it locally (the baked-in
  `forkmesh-shared-room-key-v1` in the phone and desktop `RoomCrypto` is only
  the LAN `#mesh` room).

## Push and email

- Call pushes no longer name the caller: the alert body is "Incoming call"
  and neither the alert nor the VoIP payload has a `caller` field
  ([chat-direct-calls.md](chat-direct-calls.md)).
- Unread reminder emails name no private room and no sender
  ([chat-unread-reminders.md](chat-unread-reminders.md)).
