# Web chat

Part of [the App docs](README.md). The website has one chat page, `/chat`
(`app/public/chat.html`, `app/public/chat.js`, `app/public/chat.css`): World
`#general`, the `#general`/`#random`/`#welcome` channels, private channels,
direct messages, calls, threads and channel settings. This page covers its
layout and styling, the retired `/dashboard/chat`, and how every browser
chat client follows the [relay hardening](chat-relay-hardening.md) contract.
Related: [web-composer.md](web-composer.md),
[chat-private-rooms.md](chat-private-rooms.md),
[chat-member-stats.md](chat-member-stats.md) (the activity line under each
person).

## The retired `/dashboard/chat`

- `/dashboard/chat` and `/dashboard/chat/` 308 to `/chat`, keeping the query
  (`LEGACY_PAGE_PATHS` in `app/src/static_routes.py`), as does
  `/dashboard?section=chat`. They are permanent redirects, so the entries stay.
- The dashboard sidebar's Chat link, the `?section=` shim and World's page
  switcher and "open full chat" button open `/chat`. World's `#general` board,
  and an old `/dashboard/chat` link inside World, open World's own chat panel.
- The page is not in `dashboard_shell.PAGES`, so `/dashboard/chat` is not
  excluded from `run_worker_first` and reaches the app for the redirect.
- `app/public/dashboard-chat.js` is still the World chat client
  (`world/public/world/world.js` loads it), with the `#world-*` space
  channels, avatar emotes, the task and issue composer actions and
  organization Discord channels. Its rail, member list and day grouping
  do nothing without the old dashboard markup.

## Layout

- **Channel rail**: a filter (Ctrl/⌘ + K), channels, direct messages.
- **Conversation**: header (settings, add person, retention, calls,
  encryption), transcript, composer.
- **People pane**: Online, Offline, World guests and Nodes. Each account row
  links to the profile, has a message button and carries the activity line.
  Below 900px it opens as a drawer over the conversation.

`?embed=office` renders the same page inside the Office
([../office/README.md](../office/README.md)).

## Colours and event-only parts

`app/public/chat.css` has no shell classes, so the theme roles are written
there with its own tokens: `--primary` is blue (join, save, send, thread reply,
unread count), `--cta` is green and used only by the Create button in the
new-channel dialog, and `--online` is the green for presence and the connected
status dot. Its dialogs have a bordered header and footer row with 28px
buttons; Delete confirms and channel deletion use red text that fills red on
hover, errors are red-tinted alerts, the webhook-created box is a blue info
alert, and members and webhooks in settings are hairline-divided rows instead
of boxes inside the panel. The encryption chip still shows the lock or globe
emoji that `app/public/chat.js` writes.

World loads `dashboard-chat.js` but none of the dashboard shell's `fm-*` CSS.
Built by `dashboard-chat.js`: the empty and signed-out logs are `fm-blankslate`
blocks, the mention and slash suggestion lists are `fm-popover fm-menu` with
30px rows and a green dot for people active in the room, the thread dialog
uses the `fm-dialog-header` and `fm-dialog-footer` rows with an invisible icon
close, and message actions are small invisible buttons (Delete is
`fm-btn-danger`). The blankslate icon carries `width="24" height="24"` and the
close icons `width="16" height="16"`, and `world/public/world/world.css` gives
`#fullChatMessages > .fm-blankslate` one muted line with no icon in the HUD
island feed and a dashed box with a 20px icon in the native chat panel. The
World thread dialog's title rule matches `.fm-dialog-title` as well as the old
`<strong>`.

## Clearing the empty state

The "Type below to join" blankslate and the rail's "Type to join" line carry
`data-chat-empty`, and `clearEmptyState()` looks for that child before each
new row. Never read the logs' whole `textContent` per message: its cost grows
with the history: on a long log a Discord refresh append slice (meant to stop
after 6 ms) takes up to 1.3 s.

## Links and the World embed

`/chat#room=<channel id>` and `/chat#direct=<conversation id>` open that
conversation. When the conversation is not known yet the page loads the
channel or direct-message list first, so a fresh page does not drop the hash
and fall back to #general.

`/chat?embed=world` is the World's conversation pane: the same page without
the site header, the room list and the people column, set on `<html>` before
first paint like `embed=office`. The World's channel list
([../world/voice.md](../world/voice.md)) opens channels and direct messages in
it, with every chat feature and call button working.

## Relay hardening in the browser

How the browser clients follow [chat-relay-hardening.md](chat-relay-hardening.md)
(R1 to R7). Protocol: [chat-protocol-v3.md](chat-protocol-v3.md); signatures
and sealing in general: [encryption](../encryption.md).

The clients are the `/chat` page (`app/public/chat.js`), the dashboard chat
and the World's CHAT terminal (`app/public/dashboard-chat.js`, mounted by
`world/public/world/world.js`) and the office chat
(`office/public/features/chat.js`). All of them open room frames through
`app/public/chat-room-transport.js` or the same `chat-seal.js` calls, and all
of them read who wrote a frame from `app/public/chat-frame-auth.js`.

### One node key per browser and per account (R1)

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
- A profiled store deletes pre-profile keys (anything under
  `forkmesh.chat/e2ee/` outside `p/`): their bundles name a random id every
  client refuses, and their prekey private halves would otherwise never be
  wiped.
- Every bundle carries `nsig`. A store opened without a `node` option uses its
  node key; tests that pass a fixed `node` get bundles without `nsig`, which
  only strict (direct-message) rooms accept.
- In open rooms `accept()` drops a bundle unless `node` is a 32-byte key and
  `nsig` verifies (`verifyNodeBinding`). Checked against
  `conformance/chat/v3/node-binding.json` in
  `app/tests/chat_relay_hardening_web.test.mjs`.

### Nothing private on a relay room (R2)

The web sends no relay `dm`, `@peer`, `private`, `invite` or cove frames:
direct messages and private channels use the server's rooms. The transport
refuses to send such a frame on any room that is not a direct message room,
and drops one it receives (`isPrivateRelayFrame`), sealed or not, including
inside a `history` frame. The web keeps no saved history of its own.

### Only authenticated frames act (R3)

- Every person frame is sealed (v3) in every room, World and public channels
  included. Without a local device key a public room only listens. Sealing a
  public room authenticates the sender; it does not hide the room from the
  server, which can join as a device like anyone else, and the UI says so.
- An unsealed frame is opened only when its envelope has `srv: true`, in any
  room except a direct message: the shared relay rooms and every server
  channel room, public or private, notices (such as the `#email-log` mail log,
  [mail log](../operations/mail-log.md)) included. The server writes those frames
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
- An issue ForkBot files from chat names a person only through the session.
  Without one, the `sender` in the request is a claim anyone can type: the
  issue opens as ForkBot with an empty `reporter`, no contributor count or
  badge is credited, and the footer reads `Filed by ForkBot via chat for
  <name> (unverified).` (only when the claim is a valid account name). A
  signed-in request opens as the session's account.

### Account names come from the server (R4)

- `createNodeOwners()` asks `GET /api/chat/node-owner?node=` for every sealed
  sender that claims an account, before the frame is shown, and keeps answers
  for an hour (`localStorage` `forkmesh.chat.nodeOwners.v1`, at most 512); a
  failed lookup is retried after a minute and shows the sender as a guest
  meanwhile.
- An answer that names no account is kept for only that minute too, and is
  never saved to `localStorage`: a device that posts before its `node-bind`
  lands (a new phone or browser) would otherwise stay `Guest ####` for an
  hour.
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
  Guests get no remote speech bubbles, because a guest name cannot be
  checked.
- After sign-in each client calls `POST /api/chat/node-bind` with
  `store.nodeBindProof(account)` (`sig` over
  `forkmesh-chat-node-bind-v1\n<account>\n<node>\n<ts>`), at most once a day
  per account and node (`forkmesh.chat.nodeBound.v1`).

### Blocks (R5)

The web has no block feature for accounts (only the Approve/Block choice for a
new device in a direct message), so it does not call `/api/chat/blocks`.

### Less about you (R7)

Web frames carry no `timeZone`, Solana address, host name or `dg`.
Hello and presence frames carry the display name, `accountKind` and, when
signed in, the user's own away/custom status; receivers ignore the name and
kind and use the attributed ones.

### Served files

`/chat-frame-auth.js` is one of the App files the World origin proxies. The
cores route it in `deploy/core/caddy_routes.py`; the same path list is kept in
`world/worker.js`, `world/wrangler.toml` and `tools/cloudflare_bootstrap.py`.
