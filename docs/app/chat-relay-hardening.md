# Chat relay hardening (2026-10-22)

Shared rooms such as `world-general` are joined by every device on an
instance, guests included, and run the `open` trust policy, so a frame sealed
"to the room" is readable by everyone in it. This page is the contract every
client (iOS, Android, desktop, web) and the server follow so that a shared
room only ever carries public content, and so that nobody can speak, edit or
delete as somebody else. Home page: [README.md](README.md). Protocol pages:
[chat-protocol-v3.md](chat-protocol-v3.md), [chat-protocol-v4.md](chat-protocol-v4.md).

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
recipient's keys only (already true since 2026-10-22).

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
  `#email-log` notices channel, [../MAIL_LOG.md](../MAIL_LOG.md)), never in
  direct-message rooms; a frame with `serverVisible` is labelled
  server-visible.

## R4. Account names are verified by the server

A frame's `acct` / `ownerUser` is shown as an account only after the client
checked it with `GET /api/chat/node-owner?node=<b64u>`, which returns
`{"account": "<name>"}` for a node key bound to an account (a phone's or
desktop's device bind at sign-in, or a browser's
`POST /api/chat/node-bind`), or `{"account": ""}`. A claim that does not
match is shown as a guest. Answers are cached per node for an hour.

`POST /api/chat/node-bind` (signed-in session) takes
`{"node": "<b64u>", "ts": <ms>, "sig": "<b64u>"}`, `sig` by the node key over
`forkmesh-chat-node-bind-v1\n<account>\n<node>\n<ts>`, `ts` within 5 minutes.

## R5. Blocks are kept by the server

`GET /api/chat/blocks` returns `{"accounts": [...]}`; `PUT /api/chat/blocks`
with `{"accounts": [...]}` (at most 500 lowercased account names) replaces
the list. Clients push their list on every block or unblock and read it at
sign-in. The server refuses to start a direct conversation with someone who
blocked the caller (404, as if the account did not exist), and sends no
message push, call ring or VoIP push from a blocked account.

## R6. The relay admits only known rooms

The relay routes only the room names it serves (`world-general`, `general`
and the public `world-*` spaces) plus the server's channel, office and
direct-message rooms; any other room name is a 404, and the legacy
`/api/room/<name>/ws` path is gone. It caps sockets per client address, does
not refresh a socket's last-seen time for a frame that is not a valid
envelope, and retains history outside `world-general` only for sockets bound
to an account. Room keys under `mainnode/forkmesh` are derived per room name
(version 2).

## R7. Less about you goes to strangers

In shared rooms clients send no `timeZone`, no Solana address unless the user
turned tips on, no host name as a node name (only a name the user set), and
no diagnostics (`dg`). `#mesh`: bundles carry `acct` = node id, frames carry
no `ownerUser`, a device does not re-seal its earlier messages to keys it has
not seen before, the beacon id is random per network, the desktop beacon does
not carry its node key, and the UI says that anyone nearby can read `#mesh`.
