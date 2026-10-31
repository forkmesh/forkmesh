# Desktop: chat encryption (protocols v3 and v4)

Part of [the desktop docs](README.md). The public overview of what is
encrypted and how is [encryption](../encryption.md). The protocols
themselves are
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

`ChatDevice::local()` is the installation's one device. Its identity and
prekeys live in QSettings, where the v2 key lived:
`chat/e2ee/ed25519IdentityV3`, `chat/e2ee/prekeysV3`. The old
`chat/e2ee/x25519PrivateV2` value is left in place and unused.

The per-scope and per-room state is kept out of `ForkMesh.conf`, because it
grew to most of that file and every unrelated settings write (fsync included)
had to rewrite it on the GUI thread. Each key is its own file under the core
data folder (`data/core/chat-seal/`, `storedStateKeyFile()`), written with
`QSaveFile` (fsync, owner-only):

| Family | File |
| --- | --- |
| Send counters | `chat-seal/ctrV3/<b64u(room)>` |
| Trust records, held frames, last safety number | `chat-seal/trustV3/<b64u(scope)>` |
| v4 ratchet state | `chat-seal/ratchetV4/<b64u(room)>` |
| Seen sets | `chat-seal/seenV3/<b64u(room)>` |

- **Migration.** At every start `migrateStoredState()` moves any of these keys
  still in `ForkMesh.conf` or the older `chat-seal/state.ini` into their files,
  reads each file back, and only then removes the key from the old place. A
  key an older build wrote back into `state.ini` therefore replaces the file,
  and an older build finds no ratchet and starts a new session instead of
  reusing message keys. Reads take the file, then `state.ini`, then
  `ForkMesh.conf`; a write that cannot reach the file goes to `state.ini`.
  Counters moved out of `ForkMesh.conf` restart from zero on a build older
  than the move, and peers drop its frames as replays until the counter passes
  what they last saw. Tests: `testStoredStateLeavesSettingsFile`,
  `testStoredStateKeysHaveTheirOwnFiles`, `testCountersPersist`; tests reset
  the stores with `clearStoredState()` and read through `storedSetting()`.
- The files sit in the data folder, so the configuration backup, its restore
  and the complete-system bundle carry them.
- **Counter reservations** (64 counters a block) are written on the state
  writer. The next block is queued when half of the current one is used; a
  counter beyond the block on disk raises the device's write ticket, so
  `SendGate` holds that frame until the reservation is on disk and a crash
  cannot reuse a counter a peer has seen. A frame inside the stored block
  leaves at once. The first sealed frame in each room after a start therefore
  leaves one state write later, so transport tests call `settleSends()` (wait
  for the writer, then pump events) before they clear captured frames.
- **Held frames** are capped per room at 64 frames and 128 KB
  (`kMaxHeldBytes`), newest first; an older scope is trimmed when it loads
  (`testHeldBudget`).
- **Owner-only files.** `openStoredState()` creates `state.ini` through
  `forkmesh::ownerfile::write` and restricts an existing one to 0600 before
  QSettings opens it. The same helper (`src/OwnerOnlyFile.h`, `QSaveFile` with
  0600 before the first byte) writes the per-key files, the conversation
  history (`MainWindow::saveChatHistory`, DM and private-channel text), the
  `#mesh` history (`LocalMeshNode::saveHistory`) and `paired-devices.json`. A
  crash mid-write leaves the previous file, and other local users cannot read
  message text.

- Prekeys rotate when the newest is 24 h old (checked at start, before each
  seal and before each bundle) and are wiped once expired; at most ten are
  kept. A rotation re-advertises the bundle on every relay room and mesh link.
- Counters are reserved 64 at a time (see above), so a crash skips counters
  instead of reusing them (a reused counter would be dropped as a replay).
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

## State writes off the GUI thread

Ratchet saves after every v4 seal, trust scopes re-serialised with all their
held frames on every held frame, and `QSaveFile` commits used to block the GUI
thread in `fdatasync` for up to seconds under IO load. The rules:

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
  no sealed frame reaches a peer before the ratchet step that produced it is on disk, and a crash before
  that point loses only frames that were never sent, so a restart cannot
  reuse a message key a peer has seen. The link counter of a mesh link is
  taken when the frame is released, so link frames stay in sequence. The
  gate follows the node's own device; a direct frame handed to the mesh must
  carry that device's key id (`sendDirectSealed`), and in the app every
  component uses `ChatDevice::local()`.
- **What a crash can lose** is the delayed saves of the last 2 s (the receive
  side of the ratchet, seen sets, held frames). A failed write falls back to
  `state.ini`, then the settings file; the gate opens once the write has been
  attempted.
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

Two other GUI-thread costs handled the same way:

- `MainWindow::startLocalMesh` derives the `#mesh` room key (210k PBKDF2
  rounds in `RoomCrypto`, over a second) on a worker first
  (`RoomCrypto::warmDefaultKey`) and builds the `LocalMeshNode` when it is
  cached.
- `MainWindow::saveChatHistory` copies the history
  (implicitly shared) and serialises and writes it on a one-thread writer, in
  order. `loadChatHistory` waits for that writer first, and the save from the
  close event waits for it, so nothing is lost on quit.

`RoomCrypto::decryptObject` in `ServerNode::processFrame` stays on the GUI
thread: AES-GCM on a 4 MB frame takes milliseconds, and moving it would need an
ordered pipeline for every relay frame type (acks, receipts and `history-end`
depend on order, and the seal open after it mutates the shared device on the
GUI thread).

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

Server DMs and private rooms go through `OfficeChannelMirror`. It starts for a
desktop holding the account's own key, and also for a desktop signed in with
email and password whose key is not the account's (`ownsDesktopKey` false in
`activateAccountSession`):

- `startOfficeChannelMirror()` accepts an active account session that
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
  message it sent this session. Since own frames do not open, the outbox
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

## Relay hardening

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
  and is not offered for a guest; there is no relay fallback. Cove
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
  and DMs, whose members the server checks.
- **Authenticated frames only (R3).** `sendEncrypted` seals every person
  frame, public channels included; there is no controller-readable path.
  ForkBot and teammate lines keep this device's `senderId` and name the bot in
  `bot`; a received line whose `bot` is `forkbot`, `claude` or `codex` is
  shown under that bot's id but named "<bot> · via <poster>", the poster
  being the sending node's verified account or, unverified, `Guest NNNN`
  (from the node key's hash, `relayrules::guestLabel`), so any sealed sender,
  a guest included, cannot pass as ForkBot. The name is set on the
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
  start line says anyone nearby can read the room. A device does not
  re-seal its earlier messages: a joiner (direct or through another device)
  gets only messages still queued for nobody (`resealOwnHistory`,
  `sendHistoryTo`); already delivered ones are forwarded only as their
  original sealed frames. Beacon peers are tracked by beacon id in
  `m_heard` and bound to a link's key id by address and port.
