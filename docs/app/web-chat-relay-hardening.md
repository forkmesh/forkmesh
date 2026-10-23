# Web chat: relay hardening

How the browser clients follow [chat-relay-hardening.md](chat-relay-hardening.md)
(R1 to R7). Home page: [README.md](README.md). Protocol:
[chat-protocol-v3.md](chat-protocol-v3.md).

The clients are the `/chat` page (`app/public/chat.js`), the dashboard chat
and the World's CHAT terminal (`app/public/dashboard-chat.js`, mounted by
`world/public/world/world.js`) and the office chat
(`office/public/features/chat.js`). All of them open room frames through
`app/public/chat-room-transport.js` or the same `chat-seal.js` calls, and all
of them read who wrote a frame from `app/public/chat-frame-auth.js`.

## One node key per browser and per account (R1)

- `createRoomSealStore({ profile })` keeps every key under
  `forkmesh.chat/e2ee/p/<b64u(profile)>/`, with `profile` `guest` for a
  signed-out browser and `acct:<name>` for each signed-in account. The guest
  and each account therefore have their own Ed25519 node key
  (`ed25519NodeV1`), identity key, prekeys and trust records, so nothing in a
  guest's World messages (node id, `kid`, `idk`) links them to the account
  the same browser signs in to later.
- The node key is stored like the identity key: a 32-byte seed in
  `localStorage`, wrapped by the non-extractable AES key in IndexedDB
  (`forkmesh-chat-e2ee`) where the browser has one.
- `senderId` is the node public key (`store.ready()`); the old random
  `forkmesh.chat.selfId` is deleted on load. A guest's `acct` is its node id.
- A profiled store deletes the keys a browser kept before profiles (anything
  under `forkmesh.chat/e2ee/` outside `p/`): their bundles named the old
  random id, which every client now refuses, so they could never be used
  again, and their prekey private halves would otherwise never be wiped.
- Every bundle carries `nsig`. A store opened without a `node` option uses its
  node key; tests that pass a fixed `node` get bundles without `nsig`, which
  only strict (direct-message) rooms accept.
- In open rooms `accept()` drops a bundle unless `node` is a 32-byte key and
  `nsig` verifies (`verifyNodeBinding`). Checked against
  `conformance/chat/v3/node-binding.json` in
  `app/tests/chat_relay_hardening_web.test.mjs`.

## Nothing private on a relay room (R2)

The web never sent relay `dm`, `@peer`, `private`, `invite` or cove frames:
direct messages and private channels already use the server's rooms. The
transport now refuses to send such a frame on any room that is not a direct
message room, and drops one it receives (`isPrivateRelayFrame`), sealed or
not, including inside a `history` frame. The web keeps no saved history of its
own, so there is nothing to clean up on first start.

## Only authenticated frames act (R3)

- Every person frame is sealed (v3) in every room, World and public channels
  included. Without a local device key a public room only listens. Sealing a
  public room authenticates the sender; it does not hide the room from the
  server, which can join as a device like anyone else, and the UI says so.
- An unsealed frame is opened only when its envelope has `srv: true`, in any
  room except a direct message: the shared relay rooms and every server
  channel room, public or private, notices (such as the `#email-log` mail log,
  [../MAIL_LOG.md](../MAIL_LOG.md)) included. The server writes those frames
  (`_chat_channel_deliver`) and strips `srv` from anything a client sends.
  `serverPlain()` marks it; `attributeFrame()` shows it as the
  integration it names (`accountKind: "integration"`, a `srv:`, `app:`,
  `system:` or `webhook:` sender id, a bot avatar, no profile link, no status
  badge). App blocks and `system:` notices render only from such frames. A
  frame with `serverVisible: true` (webhook deliveries carry it, with
  `webhook: true`) gets a small "server-visible" marker beside the name.
- Channel settings offer incoming webhooks for public and private channels
  (not notices, office or everyone channels) and say that webhook messages go
  through the server, which can read them. The URL shown is the one the server
  returns.
- `authenticatedPlain()` drops any `frameAuth` the sender wrote, forces
  `senderId` to the sealed node and adds
  `frameAuth: { sealed, node, acct, kid, self }`. Only the transport can set
  it.
- Edits and deletes apply only when `mayChangeMessage()` holds: a sealed frame
  whose node is the target's `senderId`. Reactions, stickers and thread
  replies need a sealed frame. A `history` frame contributes only its sender's
  own lines (`historyEntries()`).
- Reaction and sticker names come from the attributed sender, not
  `reactorName`.
- Bot lines (`bot: "forkbot"`, `claude`, `codex`, `agent`) stay as the v3
  page describes: a device posts the bot's answer under its own node and names
  the bot. Any sealed sender can do that, guests included, so `botLine()`
  shows such a line as `forkbot · via <confirmed account or Guest ######>` in
  the transcript, thread, office chat and World board, and the full name is
  never cut to the 32-character name limit. Via lines stay out of the people
  pane and the @mention list and are not sent back to ForkBot as context. Only
  the browser that asked ForkBot shows the plain `forkbot` reply, because it
  got that text from `/api/forkbot/chat` itself.

## Account names come from the server (R4)

- `createNodeOwners()` asks `GET /api/chat/node-owner?node=` for every sealed
  sender that claims an account, before the frame is shown, and keeps answers
  for an hour (`localStorage` `forkmesh.chat.nodeOwners.v1`, at most 512); a
  failed lookup is retried after a minute and shows the sender as a guest
  meanwhile.
- An answer that names no account is kept for only that minute too, and is
  never saved to `localStorage`: a device that posts before its
  `node-bind` lands (a new phone or browser) would otherwise stay
  `Guest ####` for an hour (a signed-in person's #general announcement showed
  as a guest this way on 2026-10-23, while `node-owner` already named their
  account for that node).
- `createGuestRelabeler()` remembers the ids of lines shown as a guest whose
  seal claims an account (`unconfirmedClaim`), history entries included,
  rechecks that node a minute later (at most ten times), and when the server
  confirms the claimed account renames those lines (`/chat` rows, thread
  replies and the people pane, the dashboard chat's rows, side list and thread
  dialog, the office chat's conversations). A claim the server does not
  confirm stays a guest.
- `attributeFrame()` replaces `sender` with the confirmed account, or with
  `Guest <first six characters of the node>`; it sets `accountKind` and
  `verifiedAccount`. A direct-message room uses the bundle `acct`, which the
  strict policy already restricts to the two participants.
- Only confirmed accounts update a person's away and custom status, get a
  status badge, or speak over a World avatar. The World floats a line over an
  avatar only when the line is the browser's own or from a confirmed account,
  and over ForkBot, Claude or Codex only for an integration frame or this
  browser's own answer from the ForkBot endpoint, never for a via line.
  Guests no longer get remote speech bubbles, because a guest name cannot be
  checked.
- After sign-in each client calls `POST /api/chat/node-bind` with
  `store.nodeBindProof(account)` (`sig` over
  `forkmesh-chat-node-bind-v1\n<account>\n<node>\n<ts>`), at most once a day
  per account and node (`forkmesh.chat.nodeBound.v1`).

## Blocks (R5)

The web has no block feature for accounts (only the Approve/Block choice for a
new device in a direct message), so it does not call `/api/chat/blocks`.

## Less about you (R7)

Web frames never carried `timeZone`, a Solana address, a host name or `dg`.
Hello and presence frames carry the display name, `accountKind` and, when
signed in, the user's own away/custom status; receivers ignore the name and
kind and use the attributed ones.

## Served files

`/chat-frame-auth.js` is one of the App files the World origin proxies
(`world/worker.js`, `world/wrangler.toml`, `deploy/core/caddy_routes.py`,
`tools/cloudflare_bootstrap.py`).
