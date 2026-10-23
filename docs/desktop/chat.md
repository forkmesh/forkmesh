# Desktop: chat encryption (protocols v3 and v4)

Part of [the desktop docs](README.md). The protocols themselves are
[../app/chat-protocol-v3.md](../app/chat-protocol-v3.md) and, for direct
messages, [../app/chat-protocol-v4.md](../app/chat-protocol-v4.md); this page
says how the desktop implements them and where it chose between readings of
the spec.

## Code

| Piece | Where |
| --- | --- |
| Primitives, bundles, seal/open, safety numbers, link crypto | `src/RoomSeal.*` (`forkmesh::chatv3`) |
| v4 KDFs, signed strings, ratchet session JSON | `forkmesh::chatv4` in `src/RoomSeal.*` |
| v4 seal/open and the ratchet steps | `RoomSeal::sealV4` / `RoomSeal::openV4` in `src/RoomSeal.cpp` |
| Identity, prekeys, trust, counters, seen sets, held frames, ratchet state | `forkmesh::chatv3::ChatDevice` in `src/RoomSeal.*` |
| Relay rooms, office rooms, server DMs | `src/ServerNode.*` (one `RoomSeal` per room) |
| DM rooms, direct delivery, focus, route, approvals | `src/OfficeChannelMirror.*` |
| Mesh links, `#mesh`, direct frames on the wire | `src/LocalMeshNode.*` |
| Route label, pending banner, details dialog | `src/MainWindowMessages.cpp`, `src/MainWindowIssues.cpp` |

Tests: `forkmesh-chat-v3-tests` (`tests/test_chat_v3.cpp`, the wire vectors of
`conformance/chat/v3/vectors.json` plus round trips),
`forkmesh-chat-v4-tests` (`tests/test_chat_v4.cpp`: the `kdf` vectors, the
vector `sequence` replayed as `app/tests/test_chat_v4_vectors.py` does, state
import/export, and round trips: shuffled turns, a late message from an old
chain, simultaneous first messages, reset recovery, `commit = false`,
`MAX_SKIP`, session limits, v3/v4 room separation, persistence, the ordered
state writer and its send gate) and
`forkmesh-chat-transport-tests` (relay rooms, DMs, mesh links,
`testLocalMeshLinkV3`, `testDirectMessagesOverLocalMesh`,
`testDirectRoomRatchetRecovery`).

## Keys and storage

`ChatDevice::local()` is the installation's one device. Its identity, prekeys
and counters live in QSettings, where the v2 key lived:
`chat/e2ee/ed25519IdentityV3`, `chat/e2ee/prekeysV3`,
`chat/e2ee/ctrV3/<b64u(room)>`. The per-scope and per-room state that grows
with use, `chat/e2ee/trustV3/<b64u(scope)>` (records, held frames, last safety
number), `chat/e2ee/ratchetV4/<b64u(room)>` and `chat/e2ee/seenV3/<b64u(room)>`,
lives under the same keys in its own ini file, `chat-seal/state.ini` in the
core data folder (`storedStateFile()`, `data/core/chat-seal/state.ini` in the
projects layout), so a chat write no longer rewrites `ForkMesh.conf` and no
other settings write rewrites the chat state. In report #5 (2026-10-19) these
keys were 684 KB of a 1029 KB `ForkMesh.conf`; every unrelated settings write
cost 42 ms (fsync included) against that file and 21 ms without them, and a
QSettings sync of it stalled the window 622 ms under IO load.
`ChatDevice::local()` first moves any of these keys still in `ForkMesh.conf`
(`migrateStoredState()`): it copies each key the state file lacks, syncs,
checks every key reads back, and only then removes them from `ForkMesh.conf`.
Until the move succeeds, reads fall back to `ForkMesh.conf` and a write that
cannot reach the state file goes to `ForkMesh.conf` as before. The state file
sits in the data folder, so the configuration backup, its restore and the
complete-system bundle carry it with the rest of the data. Tests reset both
stores with `clearStoredState()` and read through `storedSetting()`
(`testStoredStateLeavesSettingsFile`).
Since report #8 (2026-10-20) each of those keys is its own file beside
`state.ini`: `chat-seal/<trustV3|ratchetV4|seenV3>/<b64u(name)>`
(`storedStateKeyFile()`), written with `QSaveFile` (fsync, owner-only
permissions). `state.ini` had grown to 748 KB (three direct-room trust
scopes with held frames at about 140 KB each, one 91 KB ratchet), and the
synchronous ratchet save after every v4 seal rewrote and fsynced all of it on
the GUI thread (`ChatDevice::saveRatchets`, eight stalls in report #8). On the
same loaded box a 91 KB ratchet save went from 122 ms to 33 ms.
`migrateStoredState()` moves every `state.ini` key into its file, reads the
file back, and only then drops the key from `state.ini`, at every start, so a
key that an older build wrote back into `state.ini` replaces the file. An
older build therefore finds no ratchet in `state.ini` and starts a new session
instead of reusing message keys. Reads take the file, then `state.ini`, then
`ForkMesh.conf`; a write that cannot reach the file removes the file and goes
to `state.ini` as before (`testStoredStateKeysHaveTheirOwnFiles`).
Held frames are capped per room at 64 frames and 128 KB (`kMaxHeldBytes`),
newest first, and a scope saved before the byte cap is trimmed when it loads:
on 2026-10-19 three direct rooms held 64 frames of about 8 KB each from
pending devices, 950 KB of a 1.5 MB `ForkMesh.conf`, which every settings sync
rewrote (`testHeldBudget`).
The old `chat/e2ee/x25519PrivateV2` value is left in place and unused.

`state.ini` itself is owner-only: `openStoredState()` creates it through
`forkmesh::ownerfile::write` and restricts an existing one to 0600 before
QSettings opens it, and QSettings' `QSaveFile` keeps that mode on every sync.
The same header-only helper (`src/OwnerOnlyFile.h`, `QSaveFile` with 0600
before the first byte) now writes the per-key files, the conversation history
(`MainWindow::saveChatHistory`, DM and private-channel text, which used a plain
`QFile` at the default umask and was truncated in place), the `#mesh` history
(`LocalMeshNode::saveHistory`) and `paired-devices.json`. A crash mid-write
leaves the previous file, and other local users cannot read message text.

- Prekeys rotate when the newest is 24 h old (checked at start, before each
  seal and before each bundle) and are wiped once expired; at most ten are
  kept. A rotation re-advertises the bundle on every relay room and mesh link.
- Counters are reserved 64 at a time: the reserved high mark is written
  synchronously before the first frame that uses it, so a crash skips
  counters instead of reusing them (a reused counter would be dropped as a
  replay).
- Seen sets keep at most 256 senders per room; open scopes keep at most 512
  records. Both drop the least recently seen.
- An open room with more than 64 devices seals to itself and the 63 most
  recently seen.
- The v4 ratchet state of a DM room is at `chat/e2ee/ratchetV4/<b64u(room)>`
  in the spec's canonical JSON (the same shape `exportRatchets` returns and
  `importRatchets` reads, so the vector states load unchanged). It is kept on
  `ChatDevice`, per room, so the mirror's direct seal and the room's
  `ServerNode` share one ratchet. A seal queues it on the ordered state
  writer at once and the frame waits at the transport until that write is on
  disk; an open writes it with the delayed writer, like the seen sets (see
  "State writes off the GUI thread" below).

## State writes off the GUI thread (2026-10-22)

The stall log of 2026-10-21/22 had about 60 reports in this code: the ratchet
save after every v4 seal (`ChatDevice::saveRatchets` <- `RoomSeal::sealV4`,
mostly from `ServerNode::sendPresence`) waiting in `fdatasync` for 0.5 to 2.5 s
under IO load, the trust scope re-serialised with all its held frames on every
held frame (`ChatDevice::saveScope` <- `ChatDevice::hold` <- `RoomSeal::openV4`,
20 reports, about 1 s each), and the delayed writer's own `QSaveFile` commits on
the GUI thread. Now:

- **One ordered writer.** Every write of a `trustV3`, `ratchetV4` or `seenV3`
  key runs on one worker thread (a `QThreadPool` of one thread), first in,
  first out. The GUI thread only takes a snapshot of the in-memory state (an
  implicitly shared copy, no serialising); the worker turns it into JSON and
  writes it with `QSaveFile` (fsync and rename). Snapshots are taken from the
  live state when a write is queued, and the queue never reorders, so a later
  snapshot of a key never lands before an earlier one: the file only moves
  forward.
- **Coalescing.** A delayed save (`hold`, `setHeld`, `markSeen`, the open side
  of the ratchet, policy and record changes) only marks the key dirty; the
  snapshot is taken once, when the 2 s timer fires. Forty held frames from a
  pending device are one write instead of forty full serialisations
  (`testOrderedWriter`).
- **Durability before a frame leaves.** A save that must not be lost (the
  send side of the ratchet in `sealV4`, `resetRatchet`, `importRatchets`,
  approve/block, verify) is queued at once and returns a write ticket
  (`ChatDevice::writeTicket()`). `ServerNode::sendTextFrame` and
  `LocalMeshNode::writeLink` pass every outgoing frame through a
  `forkmesh::chatv3::SendGate`: while the device's latest ticket is not on
  disk, frames wait in order (unsealed ones too, so nothing overtakes) and go
  out when the worker reports the write done (`ChatDevice::whenWritten`). So
  the guarantee of the old synchronous save holds: no sealed frame reaches a
  peer before the ratchet step that produced it is on disk, and a crash before
  that point loses only frames that were never sent, so a restart cannot
  reuse a message key a peer has seen. The link counter of a mesh link is
  taken when the frame is released, so link frames stay in sequence. The
  gate follows the node's own device; a direct frame handed to the mesh must
  carry that device's key id (`sendDirectSealed`), and in the app every
  component uses `ChatDevice::local()`.
- **What a crash can still lose** is what it could before: the delayed saves
  of the last 2 s (the receive side of the ratchet, seen sets, held frames).
  A failed write is handled as before (fallback to `state.ini`, then the
  settings file); the gate opens once the write has been attempted.
- **Shutdown.** `aboutToQuit` and a `qAddPostRoutine` hook flush the dirty
  keys and wait for the worker; `ServerNode::shutdown` and
  `LocalMeshNode::stop` wait for pending writes and release their gated
  frames (the `bye`) before closing sockets. A closed or replaced socket drops
  its gated frames, as bytes still in its buffer would have been.
- Reading a key that has a write queued (`storedSetting`, `migrateStoredState`,
  `clearStoredState`, `ChatDevice::flush`) first waits for the writer, so a
  read never sees an older file than memory.
- A gated frame can reach the relay in the same read as the frame queued
  behind it, so the direct-room transport tests pick the sealed frame out of
  what arrived instead of taking the last one.

Two other GUI-thread costs from the same log:

- `MainWindow::startLocalMesh` derives the `#mesh` room key (210k PBKDF2
  rounds in `RoomCrypto`, 1.2 s in four reports) on a worker first
  (`RoomCrypto::warmDefaultKey`) and builds the `LocalMeshNode` when it is
  cached.
- `MainWindow::saveChatHistory` (1.4 s in three reports) copies the history
  (implicitly shared) and serialises and writes it on a one-thread writer, in
  order. `loadChatHistory` waits for that writer first, and the save from the
  close event waits for it, so nothing is lost on quit.

Not changed: `RoomCrypto::decryptObject` in `ServerNode::processFrame` (three
reports, all with the watchdog's machine-wide memory/IO starvation verdict;
AES-GCM on a 4 MB frame takes milliseconds). Moving it off the thread would
need an ordered pipeline for every relay frame type, because acks, receipts
and `history-end` depend on arriving in order, and the seal open after it
mutates the shared device on the GUI thread. The `RoomSeal::open` and
`refreshChannelList` -> `conversationForChannel` reports were also starvation
verdicts with stale frames (`setParticipants` and `conversationForChannel`
are a few string operations); the real work in them was the per-frame saves
above.

## Identities per room

| Room | Trust scope | Policy | `acct` | `node` (`senderId`) |
| --- | --- | --- | --- | --- |
| World relay room, office channels | room name | open | signed-in account | profile key |
| Server DM `direct:<id>` | `direct:<id>` | strict, participants = the two accounts | account | profile key |
| `#mesh` | `forkmesh-lan-mesh-v1` | open | node id | node id |

`ServerNode::setDirectRoom(true)` derives the scope from the room name
(`chat-direct:<id>:vN` → `direct:<id>`); the mirror passes the account
(`setChatAccount`) and the DM's two members (`setDirectParticipants`).

## Who can open a direct message

Server DMs and private rooms go through `OfficeChannelMirror`. It used to start
only from a successful signed heartbeat (or offline mode) and only when this
desktop holds the account's own key, so two signed-in users still saw "Sign in
with your account key": anyone right after sign-in, before the next heartbeat,
and anyone who signed in with email and password on a desktop whose key is not
the account's (`ownsDesktopKey` false in `activateAccountSession`).

- `startOfficeChannelMirror()` now also accepts an active account session that
  has a token for the relay (`accountSessionTokenForUrl(catalogApiUrl())`).
  The mirror sends that token as `Authorization: Bearer` on every relay request
  (`OfficeChannelMirror::apiRequest`, through `setSessionProvider`, which only
  hands the token to the session's own instance). The relay's
  `_ChatDirectMessagesRuntime.session` tries the session first and falls back
  to the node signature, so key-holding desktops behave as before.
- The mirror starts right after a sign-in (`activateAccountSession`) and on
  demand when a DM, a private room or a project invitation is started, instead
  of waiting for the heartbeat.
- When it still cannot start, the alert says why: signed out offers a "Sign in"
  button (`showSignInMenu()`); signed in but unreachable says the relay could
  not be reached.
- Signing out (`logout`) and switching to another server
  (`isolateAccountForServerSwitch`) stop the mirror, so a password session on
  a shared machine does not leave room WebSockets open and decrypted DMs
  arriving after the account has gone.

## Direct messages: v4

A `RoomSeal` with the strict policy (server DMs and their `direct-sealed`
mesh copies) seals and opens only v4 frames; open rooms (relay channels,
office channels, `#mesh`) only v3. A v3 frame in a DM room, or a v4 frame in
an open room, opens as `invalid` and is dropped. `RoomSeal::setRatchet(false)`
exists only so the v3 vector tests can open the v3 DM vectors.

- A v4 frame has no entry for its sender. A DM written before any other device
  is approved is still sent, with no entries, and the outbox re-seals it once a
  peer key is approved. Outbox completeness ignores this device's own key.
- `open` returns `Own` for this device's frames coming back from the relay.
  `ServerNode` ignores them: no `forkmesh-received`, no key advertisement; it
  only uses the relay's recipient count on the echo for the sent mark of a
  message it sent this session. Since own frames no longer open, the outbox
  is filled only when a frame is sent, not from the relay's replay after a
  restart.
- There is no history-replay exception: a frame already opened is `replay`.
- `NoSession` (missing session or prekey, too many skipped keys, a failed
  unwrap) calls `ServerNode::recoverSession(kid)`: at most once per room and
  peer every 60 s (the clock is kept on `ChatDevice`, so a reconnect does not
  restart it), it deletes the sessions with that `kid`, sends a persisted,
  non-notifying `{"type":"e2ee-reset","senderId":<node>}` sealed to that
  `kid` alone, and one persisted key advertisement. Recovery is skipped
  entirely (sessions kept, nothing sent) when the frame's `ctr` is below the
  highest `ctr` already opened from that `kid` in the room (the seen-set
  maximum, `RoomSeal::openedLaterThan`): the relay replays an unacknowledged
  `noSession` frame on every reconnect, and resetting again would break the
  session that already recovered. The 60 s clock reads `ChatDevice::now()`, so
  tests can move it. A `direct-sealed`
  mesh frame that gives `NoSession` asks the room's relay node to do the same
  when one is connected.
- An opened `e2ee-reset` is acknowledged but never shown; it re-seals every
  outbox entry to that device alone (same id, `persist`, `notify` for chat,
  `resend: true`) and sends no `forkmesh-superseded`, because the original
  relay copy still serves the other devices. Re-seals for a newly approved
  device keep the v3 behaviour (re-seal to all, supersede the old ref).
- Held frames released on approval are opened through the ratchet like any
  other frame. `open` takes a `commit` flag; with `commit = false` nothing is
  stored and frames are not held.

## Relay rooms

- Bundles go out on every connect (persisted once a day), on rotation, and
  once more when a frame from an unknown sender or a frame not addressed to
  this device arrives.
- Held frames are re-opened when the sender's bundle arrives or its trust
  changes; the `ServerNode` of that room re-opens them through the normal
  frame path, or the mirror does when no socket is open.
- Relay `@peer` DMs, relay private channels and relay cove frames are gone
  (see "Relay hardening" below). Any frame other than `hello` that carries a
  `to` (the `history` reply to a newcomer) is sealed to that peer only
  (`ServerNode::sealForRoom`, `targetedPeer`).
- A peer's `hello` channel list and `channel` frames only add names that start
  with `#`, are at most 80 characters, contain no control or format
  characters, and are not `#office/...`, `@...` or `direct:...` names
  (`adoptableChannelName`); adoption stops at 200 channels. Any member could
  otherwise plant a row that looks like a DM or an office room, or flood the
  channel list.
- The `history` reply carries only this device's own messages, and a
  received `history` frame is applied only for entries whose `senderId` is the
  frame's authenticated sender (`sendHistoryTo`, `handlePlain`): a peer could
  otherwise replay words under somebody else's name.
- Chat and thread replies are shown at the frame's signed `ts`, not the
  plaintext `ts`, so a replayed frame cannot be reordered.
- A frame from another approved device of this account remembers its relay
  ref (so delivered marks work) and is shown as that device's message.
- Edits and deletes are applied only when the frame's `senderId` (checked
  against the device record's `node`) equals the author of the target, and a
  reaction only when `reactorId` equals `senderId`. This is stricter than the
  spec's "same `acct`": editing a message from another of your own devices is
  refused.

## Direct delivery

The mirror seals each DM frame (chat, file, reply, reaction, edit, delete)
with the DM room's seal to one linked device at a time and hands it to the mesh
as `direct-sealed`. Only approved devices of the conversation get one, files
only up to 1 MiB on TCP and 256 KiB on Bluetooth. The receiver checks the
link's authenticated `kid` against `frame.kid`, opens it with the DM seal and
answers `direct-ack` with the message id. Mutations carry the same frame id on
the mesh and the relay, so the relay copy is dropped as a duplicate.

The DM seal needs the DM's crypto room name, which the mirror stores in the
direct directory (`chat/direct/meshDirectory`) once the relay has given it
room access. DM bundles do not travel over the mesh, so two devices that have
never met through the relay cannot use direct delivery.

**Focus** on the desktop is: the window is active, the chat section is shown
and that DM is the current conversation (`MainWindow::refreshDirectFocus`, run
on activation, section and conversation changes). A focus change is sent as a
`focus` frame on every link and repeated after each link authenticates.

**Direct only.** When this desktop has focus and a linked approved device of
the other person reported focus on the same DM, the frame goes only over the
mesh; the relay copy is queued after 3 s without a `direct-ack` from one of
the other person's devices, or at once when those links drop. Otherwise the
frame goes over the mesh and through the relay. A message sent over the mesh
is not reported as failed while the relay is unreachable.

The route label under the DM name reads "Direct · Wi-Fi" (any LAN link) or
"Direct · Bluetooth" while a linked approved device of the other person is
present, and "Direct only" while the focus rule holds.

## Approvals and safety numbers

A pending device (a second device of an account that already has one) adds a
"ForkMesh" line to the conversation and shows the banner with Approve, Block
and Safety number. The details dialog (the info button, or the encryption
badge, on a DM) shows the 60 digits in 12 groups, the QR code of the QR string
(`src/QrCode.*`) and the string itself, every device with its state, and
"Mark as verified". A change to the approved set after both accounts have a
device clears verified and adds "Your safety number with @name changed."

## Relay hardening (2026-10-22)

The desktop side of [chat-relay-hardening.md](../app/chat-relay-hardening.md)
(R1 to R7). Tests: `testNodeBinding` in `test_chat_v3.cpp`, and in
`test_chat_transport.cpp` `testPublicIntegrationsAndPrivateSeal`,
`testTargetedConversationSealing`, `testTeammateChatRelay`,
`testUserStatusOnPresenceFrames`, `testNodeOwnersAndBlocks`,
`testRelayRules`, `testLocalMeshNoResealToDirectJoiner`, the other `#mesh`
room tests and `testOfficeChannelMirror`.

- **Node binding (R1).** The desktop's relay node id is already the `b64u` of
  its 32-byte Ed25519 profile key (`ForkMeshIdentity::publicKey()`), in the
  World room, the office/channel/DM rooms and `#mesh`. `RoomSeal` takes a node
  signer (`setNodeSigner`); in open rooms `keyFrame()` adds `nsig` over
  `forkmesh-chat-e2ee-node-v1\n<room>\n<kid>\n<idk>\n<acct>`, and
  `acceptKeyFrame` drops a bundle whose `node` is not a 32-byte key or whose
  `nsig` does not verify (`verifyNodeBinding`). `MainWindow` signs with the
  profile identity for the World `ServerNode` and `#mesh`; the office mirror
  passes its signer to every room `ServerNode`
  (`OfficeChannelMirror::prepareRoomSender`, which also marks the room as not
  shared). `ServerNode` keeps the signer across every seal rebuild. Strict rooms send and accept
  bundles without `nsig`. Checked against
  `conformance/chat/v3/node-binding.json`.
- **Nothing private on the shared room (R2).** `ServerNode::relayForbidden`
  drops, in both directions, `dm`, `invite`, `cove-invite`, `cove-open`, any
  frame of an `@` conversation, and in the shared World room
  (`setSharedRoom(true)`, the default) any `private: true` frame. The office
  mirror's room nodes call `setSharedRoom(false)`, so its server private
  channels keep their `private` marks. `sendDirect` only posts a system line.
  `createPrivateChannel` on the shared room accepts only the device-local
  `#team`. "Message" on a person opens a server DM for the verified account
  and is not offered for a guest; there is no relay fallback any more. Cove
  invites, and the "opened your cove" notice, are server DMs
  (`OfficeChannelMirror::sendDirectText`, which opens the conversation
  without switching to it). On start `purgeRelayPrivateChannels` forgets the
  saved relay private channels (the `chat/privateChannels*` settings),
  removes them and every `@peer` conversation from memory, and
  `loadChatHistory` drops them from the saved file (`purgedChatHistory`),
  threads, unread counts, open DMs and DM names included. `#mesh`, `#team` and
  server DMs (`#office/@...`) stay.
- **Badges.** `refreshRoomSealButton` reads `relayrules::roomBadge`: Public
  (sealed to the room's devices, readable by anyone in the room), Nearby for
  `#mesh` ("anyone nearby can read it"), This device for `#team`,
  Server-visible for notices, and End-to-end only for server private channels
  and DMs, whose members the server checks. The old relay private-channel
  "End-to-end" badge is gone with those channels.
- **Authenticated frames only (R3).** `sendEncrypted` seals every person
  frame, public channels included; there is no controller-readable path.
  ForkBot and teammate lines keep this device's `senderId` and name the bot in
  `bot`; a received line whose `bot` is `forkbot`, `claude` or `codex` is
  shown under that bot's id but named "<bot> · via <poster>", the poster
  being the sending node's verified account or, unverified, `Guest NNNN`
  (from the node key's hash, `relayrules::guestLabel`), so any sealed sender,
  a guest included, can no longer pass as ForkBot. The name is set on the
  message itself, so the transcript, list previews and notifications all show
  it; the office mirror hands the same verifier to its room nodes. Such a line
  never renames the device. An
  unsealed frame is used only when the envelope has `srv: true`, and then only
  as a `chat` in a `#` channel, shown as the integration: its `senderId` keeps
  a `webhook:`, `system:` or `app:` prefix and anything else becomes
  `srv:<id>`, with no account, node name or Solana address
  (`ServerNode::serverFrameFromPlain`, also used for the office mirror's HTTP
  history). Edits, deletes, reactions and thread replies therefore only come
  from sealed frames, and edits and deletes still need the authenticated
  author.
- **Webhook-fed channels.** In server channel rooms (the office mirror's room
  nodes, `setSharedRoom(false)`) and their HTTP history, a `srv: true` chat is
  shown whether the channel is public or private, the `#email-log` notices
  channel included, even when it carries `private: true`; it is never
  accepted in a DM room (`m_directRoom`, `room.direct`). A frame with
  `serverVisible: true` gets " · server-visible" after the integration's name
  (names up to 96 characters for integrations). The channel settings dialog
  offers webhooks for private channels as well as public ones, not for
  notices (`relayrules::webhooksAllowed`), and says the server reads webhook
  messages (`relayrules::webhookNote`).
- **New-member notes.** The relay's plaintext `forkmesh-new-member`
  announcement counts only with `srv: true`; any other copy is dropped.
- **Verified accounts (R4).** `ChatNodeOwners` asks
  `GET /api/chat/node-owner?node=<b64u>` once per node key, caches an answer
  for an hour and a failure for a minute. `ServerNode` shows a peer's
  `ownerUser` on the roster only when it matches the answer (or the claim is
  empty); otherwise the peer is a guest. The profile dialog takes the account
  from the verified roster row, a server directory entry or the lookup, never
  from a display name.
- **Blocks (R5).** `ChatAccountBlocks` keeps the blocked accounts in
  `chat/blockedAccounts`, pushes the whole list with
  `PUT /api/chat/blocks` (Bearer session) on every block or unblock, and
  merges `GET /api/chat/blocks` when the office mirror starts after sign-in;
  a local block the server has not seen is pushed back. The profile dialog
  has Block/Unblock. A blocked account's messages and typing are dropped and
  its saved messages are not drawn.
- **Less about you (R7).** The shared room gets the node name only when the
  user set one (`sharedNodeName`, never the host name), a Solana address only
  when "Show this address in shared chat rooms so people can tip you" is on
  (`chat/shareTipAddress`, `ServerNode::setTipAddress`), and no `dg`.
  `#mesh`: bundles carry `acct` = node id, frames carry no `ownerUser` and a
  received one is ignored, the beacon id is 12 random bytes drawn on start and
  on every network change (`renewBeaconId`), the beacon has no `node`, and the
  start line says anyone nearby can read the room. A device no longer
  re-seals its earlier messages: a joiner (direct or through another device)
  gets only messages still queued for nobody (`resealOwnHistory`,
  `sendHistoryTo`); already delivered ones are forwarded only as their
  original sealed frames. Beacon peers are tracked by beacon id in
  `m_heard` and bound to a link's key id by address and port.
