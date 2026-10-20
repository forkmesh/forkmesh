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
`MAX_SKIP`, session limits, v3/v4 room separation, persistence) and
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
Held frames are capped per room at 64 frames and 128 KB (`kMaxHeldBytes`),
newest first, and a scope saved before the byte cap is trimmed when it loads:
on 2026-10-19 three direct rooms held 64 frames of about 8 KB each from
pending devices, 950 KB of a 1.5 MB `ForkMesh.conf`, which every settings sync
rewrote (`testHeldBudget`).
The old `chat/e2ee/x25519PrivateV2` value is left in place and unused.

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
  `ServerNode` share one ratchet. A seal writes it synchronously before the
  frame is returned (like the counter reservation); an open writes it with
  the delayed writer, like the seen sets.

## Identities per room

| Room | Trust scope | Policy | `acct` | `node` (`senderId`) |
| --- | --- | --- | --- | --- |
| World relay room, office channels | room name | open | signed-in account | profile key |
| Server DM `direct:<id>` | `direct:<id>` | strict, participants = the two accounts | account | profile key |
| `#mesh` | `forkmesh-lan-mesh-v1` | open | node id | node id |

`ServerNode::setDirectRoom(true)` derives the scope from the room name
(`chat-direct:<id>:vN` → `direct:<id>`); the mirror passes the account
(`setChatAccount`) and the DM's two members (`setDirectParticipants`).

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
- Relay `@peer` DMs are sealed to the devices whose bundle `node` is the
  recipient plus this account's approved devices.
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
