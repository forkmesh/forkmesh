# Private rooms and direct messages

Who can see a chat conversation, where that rule is enforced, and how direct
messages are delivered. Related: [chat-everyone-channels.md](chat-everyone-channels.md),
[chat-push-previews.md](chat-push-previews.md),
[chat-direct-calls.md](chat-direct-calls.md),
[chat-unread-reminders.md](chat-unread-reminders.md).

## Access rule

- A **private room** (`chat_channels` row whose sealed record says
  `visibility: "private"`) is visible to, joinable by and readable by its
  creator (`created_by_bi`) and accounts with a `chat_channel_members` row.
  Platform administrators get no implicit access.
- Members are added only by explicit invitation: `POST /api/chat/channels`
  with `members`, `POST /api/chat/channels/<id>/members`, or the room's invite
  link.
- A **direct message** (`chat_direct_conversations`) belongs to its two
  participants (`chat_direct_participants`); `pair_bi` is unique per pair.
  There are no group DMs; a private room is the group conversation.
- A non-member gets `404 {"error":"not_found"}` from every channel endpoint,
  exactly as for an unknown id. A member who is not the creator gets
  `403 manager_required` from manager actions.
- Public channels are readable by every signed-in account and manageable by
  administrators.

| Surface | Code |
| --- | --- |
| Channel API | `app/src/chat_channels_api.py`: `_readable_channel`, `_can_manage`, `_require_manager`, `_list_channels` |
| Channel socket admission | `app/src/entry.py` `_chat_channel_socket_handler` |
| Live socket re-check | `ForkMeshRoom` access re-check (creator, public, or member row) |
| Office meeting socket | `world/src/world_office_routes.py`, `ForkMeshOfficeRoom._access_current` |
| Push audience | `_chat_channel_push_accounts` (creator plus member rows) |
| Direct messages | `app/src/chat_direct_messages_api.py`, `_chat_direct_socket_handler` |

## Desktop key-signed requests

The desktop has an account key, not a session token, so each write is a proof
bound to the resource and body (`_chat_channel_signed_session`,
`_chat_direct_signed_session`):

- `forkmesh-chat-channel-create-v1\n<node>\n<ts>\n<sha256(fields)>`, where
  `fields` is `name=<name>\nvisibility=<v>\nmembers=<a,b,...>`.
- `forkmesh-chat-channel-members-v1\n<node>\n<channel>\n<ts>\n<METHOD>\n<sha256(username)>`.
- `forkmesh-chat-direct-list-v1`, `-start-v1`, `-access-v1`, `-read-v1` with
  the node, timestamp and conversation or username digest.

`app/tests/test_chat_desktop_room_and_dm_proofs.py` and
`desktop/tests/test_chat_transport.cpp` pin the same digests.

## Invite links

- Table `chat_channel_invites`: one row per private room. `token_bi` is the
  blind index of the token; the token itself sits in sealed `data`.
- Manager endpoints `GET|POST|DELETE /api/chat/channels/<id>/invite` read,
  rotate or disable the link. Only private rooms take one
  (`409 private_channel_required`).
- Joining: `POST /api/chat/invites/preview` and
  `POST /api/chat/invites/accept`, each with the body `{"token": "<64 hex>"}`,
  preview or join, under the `MAX_CHANNEL_MEMBERS` cap. Removing any member,
  deleting the room, or changing its creator invalidates the link.
- The web link is `/chat#invite=<token>`: a fragment, so the token never
  reaches the server in a page request or `Referer`, and the API takes it in
  the request body, so it never appears in a URL either. Until 2026-10-22 the
  API path was `/api/chat/invites/<token>`, which put the token in every Caddy
  access log line ([NODE_LOGS.md](../NODE_LOGS.md#caddy-access-log)); that
  path now answers `410 token_path_retired`. The body was chosen over a
  Caddy `regexp` log filter because no Caddy binary is available to validate
  a new log config before it ships.
- Incoming webhook URLs are `/api/chat/webhooks?token=<64 hex>` (the
  `x-forkmesh-webhook-token` header works too). The access log replaces the
  `token` query value with `REDACTED` (`ACCESS_LOG_QUERY_KEYS` in
  `deploy/core/render.py`). The old `/api/chat/webhooks/<token>` URLs answer
  `410 token_path_retired`; their owners rotate the webhook to get a new URL.

## Private channel names

A private channel's name is unique only among its creator's private channels
(`name_bi` is the blind index of `chat-channel-private-name:<creator_bi>:<name>`);
public channel names stay unique among public channels
(`chat-channel-name:<name>`). Before 2026-10-22 every channel shared one name
space, so creating or renaming a channel answered `409 channel_name_taken`
when someone else's private channel, invisible to the caller, already had the
name: anyone could test whether a private channel with a given name existed.
Now the only `409` a caller can see names a public channel or one of their own
private channels. Switching visibility moves the row to the other name space
and is refused with `409` if that name is taken there. The `UNIQUE` constraint
on `chat_channels.name_bi` is unchanged, so no migration was needed; channels
made before the change keep their old global `name_bi` until they are renamed.

## Channel limits and webhook pings

- `POST /api/chat/channels` refuses a new channel with
  `429 too_many_own_channels` once its creator already holds
  `MAX_CHAT_CHANNELS_PER_CREATOR` (20) channels, and with
  `429 too_many_new_channels` once its creator made
  `CHANNEL_CREATE_MAX_PER_WINDOW` (5) channels in the last
  `CHANNEL_CREATE_WINDOW_MS` (one hour). Office and Everyone channels count
  toward neither. The hourly budget counts successful `chat.channel.create`
  rows in `sensitive_audit_log` (`channel_creations_since`), not live
  channels, so deleting a channel and making a new one does not reset it.
- There is no site-wide channel ceiling (removed 2026-10-28). The old
  `MAX_CHAT_CHANNELS` (100) was shared by everyone, so five accounts at their
  per-creator cap blocked channel creation for the whole site until they
  deleted their channels. Counting only recently active channels would not
  help, because the owner of a squatting channel can keep it active. Growth
  is bounded per account by the two limits above instead.
- An incoming webhook post to a public channel always pings the channel's
  creator, and pings the channel's readers (accounts with a
  `chat_channel_reads` row for it, which opening the channel writes; up to
  `MAX_CHANNEL_MEMBERS`, newest first) only when none of that channel's
  webhooks has posted in the last `WEBHOOK_PUBLIC_NOTIFY_WINDOW_MS`
  (10 minutes). Until 2026-10-28 that audience was the first
  `MAX_CHANNEL_MEMBERS` active users of the whole site, so any account could
  make a public channel and a webhook and push its own text to 500 phones,
  over and over by deleting and recreating the channel. The check and the `last_used_at` write are one `UPDATE ...
  WHERE NOT EXISTS ... RETURNING`, so parallel posts cannot all claim the
  window, and the gate needs no new table. Before 2026-10-22 every post made
  a ping (an uncoalesced phone banner) with the webhook's own title and text
  for every active user, so anyone holding a webhook URL could flood every
  phone. A steady stream of posts therefore pings the audience once, when it
  starts after a quiet window; the messages themselves are all delivered.
- Private channels take webhooks too (2026-10-22), so rooms fed by a tool
  (a deploy log, a mail log) can be private. The server writes a webhook
  message with the room cipher as it arrives (`_chat_channel_deliver`),
  marks the envelope `srv` and the frame `webhook` and `serverVisible`; every
  client accepts such frames in public and private channel rooms, never in
  direct messages, and labels them server-visible. A private channel's
  webhook notifies its creator and members only. Notices and office channels
  still take no webhooks.

## Server copies of direct messages

The relay keeps an encrypted direct message only until a recipient device has
it. Clients show "N messages on the server" or "Not stored on the server".

- For a `persist: true` cipher frame in a direct room the relay stamps
  `"ref":"<sha256(frame)[:40]>"` on the outer envelope and stores the frame
  before fan-out. `chat_history.msg_id` is `<ref>:<sender account_bi[:16]>`,
  plus `:n` when the sender marked the frame `notify` (a real message).
- A recipient client sends `{"kind":"forkmesh-received","v":1,"refs":[...]}`
  (at most 64 refs) after opening a frame; `chat_direct_received` marks the
  matching rows not sent by the acknowledging account as acknowledged
  (`acked_at`). From then on only the push preview route can read them, for
  120 s, and they are deleted after that
  ([chat-push-previews.md](chat-push-previews.md#preview-grace)); replay,
  polling, the server-copies count and reminders ignore them at once. A frame
  the device cannot open is not acknowledged.
- `forkmesh-received` and `forkmesh-superseded` on a DM socket have their own
  budget (`ROOM_RECEIPT_MAX_PER_WINDOW`, 40 per 10 s, frames up to 8 KiB,
  `chat_direct_receipt_envelope`) and are handled before the 20-frame message
  limit. Before 2026-10-15 they shared that limit: a reconnecting phone sends a
  key advertisement and a burst of resealed frames first, so its ack was
  dropped without a reply, and the copies stayed on the server while the
  sender already saw Delivered (from the push path) and Read.
- The relay reports `{"kind":"forkmesh-server-copies","v":1,"stored":N}`
  after a store, a receipt, and on join. Unacknowledged rows expire after 7
  days.
- Only a `notify` frame that is not a `resend` raises the unread count and
  sends a push.
- Channels have many readers: no `ref`, receipts ignored.
- A recipient whose copies wait more than a day gets one reminder email
  ([chat-unread-reminders.md](chat-unread-reminders.md#direct-messages-waiting-on-the-server)).

### Re-sealing

A device seals a DM's content key only for device keys it has learned in that
room (`e2ee-key` frames). Every client follows these rules in a DM room:

1. Never acknowledge an `e2ee-key` frame, so key advertisements stay on the
   relay for late devices.
2. When a sealed frame is not wrapped for this device, send one persisted key
   advertisement; this tells the sender to re-seal.
3. Keep an outbox of up to 64 own durable frames. When a new peer key is
   approved (protocol v3: a pending device gets nothing until the user approves
   it), reseal entries not wrapped for every approved device other than this
   one, send them with the same message id and `"resend": true`, then send
   `{"kind":"forkmesh-superseded","v":1,"refs":[...]}` for the old refs. A DM
   frame sealed before any other device is approved has no entries (protocol
   v4 seals nothing to its sender); it is still sent, and this step reseals it.
4. Receivers drop message ids already shown.
5. Protocol v4 ([chat-protocol-v4.md](chat-protocol-v4.md)): an own frame that
   comes back from the relay (`own`) is ignored, never acknowledged and
   triggers no key advertisement. A frame whose ratchet session is missing
   (`noSession(kid)`) is not acknowledged, so the relay replays it on every
   reconnect; when this device has already opened a later frame from that
   device (a higher `ctr`, reported by the web seal as `later: true`) the
   client does nothing, because the session already works again. Otherwise
   the client, at most once per room and device every 60 s, deletes its
   sessions with that device, sends it a persisted, non-notifying sealed
   `{"type":"e2ee-reset","senderId":<node>}` (sealed to that device only) and
   one persisted key advertisement. A client that opens
   an `e2ee-reset` shows nothing and reseals every outbox entry to that device
   alone, same id and `"resend": true`, without `forkmesh-superseded`: the
   original copy still serves the other devices.

`forkmesh-superseded` (`chat_direct_superseded`) deletes only rows the
sending account stored. "Received" means received by one of the recipient's
devices; other devices of that person cannot replay it.

## Delivery ticks

Every own message shows a status mark on every client (web, Office, desktop,
iOS, Android):

| State | Mark | Set when |
| --- | --- | --- |
| Sending | One dotted ring | On screen, relay has not acknowledged |
| Sent | One ring with a tick | `forkmesh-recipients` arrived, or the frame came back relayed or replayed |
| Delivered | Two overlapping rings with ticks | `forkmesh-delivered` named its ref (DMs only): a recipient device acknowledged it, or a recipient phone got its push |
| Read | Two filled rings in the accent colour | `forkmesh-read` arrived after it was sent (DMs only) |

- `{"kind":"forkmesh-delivered","v":1,"refs":[...]}`: forwarded by
  `ForkMeshRoom._direct_received` to the other account's sockets when a
  device acknowledges, and by the room's `delivered` path when a recipient
  phone posts `/api/chat/direct-messages/<id>/delivered?ref=` on receiving the
  push ([chat-push-previews.md](chat-push-previews.md#delivery-receipt-route)).
  Without the second path a recipient whose app was closed showed only Sent
  until they opened the conversation, while Signal showed Delivered for the
  same phone.
- `{"kind":"forkmesh-read","v":1}`: sent when `_mark_read` moves the reader's
  `last_read_count`. A hidden tab defers its `/read` until visible.
- Channels reach only sending and sent. Ticks change only while the sender's
  client has the conversation open. iOS saves delivered and read marks with its
  local DM history, and `forkmesh-read` marks every earlier own message read;
  other clients keep ticks in memory only.

## Replies, reactions, edits and deletes in DMs

DMs use the same sealed frames as channels: `thread-reply` (persist and
notify), `reaction`, `edit` and `delete` (persist).

A quoted reply (Signal style, iOS since 2026-10-15) is an ordinary `chat` frame
with `"replyTo": {"id", "senderId", "sender", "text"}`, the text cut to 280
characters. It appears in the conversation, not in a thread. Clients that do not
know the field show the message text alone. Reply and react work on
any message; edit and delete for everyone only on one's own. A receiver
applies an edit or delete only when the frame's `senderId` equals the target's
`senderId`. After a delete the sender also sends `forkmesh-superseded` for the
original ref, so an undelivered server copy is withdrawn; the delete frame
itself stays until delivered.

## Conversation list

Each row shows the newest decrypted top-level message ("You: text",
"Name: text"), a short time, and for a DM the peer's avatar with presence.
The line is built only from messages the client already holds; no server API
returns plaintext. Previews are repainted in a debounced batch.

### Deleting a conversation and blocking (iOS)

Long-press a DM row (or open its Details) for **Delete Conversation** and
**Block**. Both are per device and never reach the server:

- *Delete* hides the conversation's messages (`ChatExtras.hideMessage`) and
  records the time in `ChatWorkspace.removedConversations`. `directRows` skips
  the row until a message newer than that time arrives, or until you open the
  conversation again (`ChatHub.select` restores it). Before this, Delete only
  hid the messages, so an empty "No messages yet" row stayed in the list. A
  relay `@node` room whose peer is gone shows as "direct message"; Delete is
  how you get rid of it.
- *Block* adds the lowercased account name to `ChatWorkspace.blocked`. That
  person's DM row, messages (DMs and channels, by account name, node owner or
  sender name), typing indicator, in-app pings and `#mesh` local alerts are
  hidden. Messages still arrive and are kept, so unblocking (in the person
  sheet) shows them again. The server is not told, so the blocked person can
  still send. Push banners for their DMs are still drawn by the notification
  extension, which cannot read the app's block list.

### Typing in direct messages

The iOS composer sends a sealed `{type:"typing", active, conversation}` frame
over the DM room socket, at most every `ChatLimits.typingResend` seconds and
once with `active: false` when the draft empties, the message is sent or you
leave. The envelope has no `persist` or `notify`, so the relay forwards it to
live sockets only and stores nothing, like call signalling. The receiver shows
it only when `sender` is the room's other account (so your own other devices
are not shown as typing) and expires it after `ChatLimits.typingTimeout`; a
chat message from that sender clears it at once. The bubble names who is
typing ("pat is typing...", "pat and sam are typing...", "3 people are
typing...").

## Direct messages over the local mesh

A DM is also sent over the local mesh (LAN and Bluetooth,
[../desktop/networking.md](../desktop/networking.md)) to directly linked
devices whose key id is one of the DM room's keys, sealed to that one key.
The plain frame is the relay frame with `type: "direct"`; it is not stored,
relayed or queued on the mesh. A receiver accepts it only when the sender's key
id is one of the room's keys, and message ids are shared with the relay copy,
so the second copy is dropped. The relay copy is still sent. The desktop keeps
its DM directory in `chat/direct/meshDirectory` so DMs work after an offline
start. Text only; no files, edits or reactions over the mesh.

## Files in direct messages

All clients send files in DMs as sealed `{type:"chat", fileName, fileMime,
file}` frames. Limit 1 MiB per file (`kMaxDirectFileBytes`,
`DirectMessageFrames.maxFileBytes`, `MAX_ATTACHMENT_BYTES`); the relay's
retained-body cap `CHAT_HISTORY_MAX_BODY` is sized for that.

### Relay size limits for v3 frames

The relay never opens a sealed frame: it sees only the outer `kind:"cipher"`
envelope, whose `body` is the base64 of the v3 frame, so the only v3 changes
on the server are size limits. A v3 frame carries a 1088-byte ML-KEM
ciphertext per recipient (about 1.7 KB per `keys` entry, 2.2 KB once the outer
layer base64s it), and a 1 MiB file is base64'd three times (file, sealed
body, outer body). Measured on the wire (2026-10-16):

| Frame | Outer envelope |
| --- | --- |
| 1 MiB file, 2 devices | about 2.49 MB |
| 1 MiB file, 64 devices (the v3 maximum) | about 2.63 MB |
| Text, 64 devices | about 145 KB |
| Bundle (`e2ee-key` v3) | about 3.6 KB |

| Limit | Was | Now | Why |
| --- | --- | --- | --- |
| `CHAT_HISTORY_MAX_BODY` (`app/src/entry.py`) | 1,900,000 | 3 MiB | A file frame over the cap was neither stored nor pushed. 1.9 MB was D1's 2 MB row limit; the served stack stores rows in rqlite, which has none. Still below `MAX_TEXT_BYTES` (4 MiB). |
| `CHAT_HISTORY_REPLAY_MAX_BYTES` | 2 MiB | 4 MiB | The replay window adds rows newest first and stops at the budget, so a newest row bigger than the budget replayed nothing at all. It must be at least `CHAT_HISTORY_MAX_BODY`. |
| `ROOM_LIVE_MAX_BYTES` | 16 KiB | 192 KiB | 16 KiB held about 6 v3 recipients. 192 KiB fits 64 devices plus an 11 KiB ink chunk; 150 frames per 10 s at that size is still less than the 20 x 4 MiB message lane. |
| Host `ws_max_size` (`app/host/forkmesh_host/serve.py`) | 2 MiB | 4 MiB | uvicorn closed the socket (1009) before the app saw a file frame; it now matches `MAX_TEXT_BYTES`. |
| Mesh `MESH_WS_MAX_SIZE` (`durable_cluster.py`) | 2 MiB | 4 MiB + 64 KiB | The same frame proxied between core nodes, plus the relay's `ref` and `recipients` stamps. |

Unchanged: `MAX_TEXT_BYTES` (4 MiB), the 20-frame and 40-receipt buckets,
`ROOM_RECEIPT_MAX_BYTES` (receipts are plaintext ref lists),
`CHAT_HISTORY_INGRESS_MAX_BYTES` (8 MiB per 10 s, three 64-device file
frames) and the 16 MiB per-room retention budget. Every v3 frame is too big
for Apple's 4 KB push payload; see [chat-push-previews.md](chat-push-previews.md).
`app/tests/test_chat_history_byte_budget.py` builds the 64-device file and
live frames and checks them against these limits.

On iOS a received file lives in memory (`ChatExtras.files`). Sharing or
playing it writes the decrypted bytes to `tmp/chat-media/` with complete file
protection. `ChatAttachmentView.sweepTemporaryFiles` deletes those files when
the app goes to the background, and at launch also deletes leftover
`viewonce-*` and `messages-voice*` recordings from a crash. Before, nothing
ever deleted `tmp/chat-media/`.
The file in `tmp/chat-media/` is named by `ChatMediaFile` (`FMChat`): the
first 24 hex characters of the SHA-256 of the message id, a dash, and the file
name with `/`, `\` and `..` replaced. The message id comes from the sender, so
until 2026-10-28 an id such as `../../Documents/x` wrote attacker bytes into
the app's Documents folder (visible in the Files app) as soon as the message
was drawn. `ChatMediaFile.url` also refuses a path whose folder is not
`chat-media` itself. Guard: `FMChatTests/ChatMediaFileTests.swift`.

What is still not covered (audit 2026-10-14):

- Files posted in a public `#` channel are not sealed, so the server can read
  them (`RelayService.isControllerReadableFrame`).
- Device keys were trusted on first use with nothing tying a key to an account;
  chat protocol v3 ([chat-protocol-v3.md](chat-protocol-v3.md), 2026-10-16)
  binds each device key to its account in a signed bundle and holds any further
  device of a DM account until the user approves it.
- The server deletes a retained DM frame when one device of the recipient
  account acknowledges it, not when the file is viewed, and a view-once part
  can be downloaded again until the client calls consume.
- View-once tombstone rows (sender, recipient, size, times) are kept 30 days;
  self-hosted SQLite has no `secure_delete`.
- Chat history JSON (text and file names, no file bytes) uses
  until-first-unlock protection. Since v3 it is excluded from backups, so it
  survives app and iOS updates (the file stays in Application Support) but not
  a restore onto a new or erased phone. Keeping it out of backups is the v3
  privacy choice: an unencrypted iTunes/Finder backup or iCloud backup would
  otherwise carry readable chat text, and iOS has no per-file "encrypted
  backups only" flag. Why history used to vanish after an app update, and the
  fix, is in [../native/README.md](../native/README.md#architecture).
- The desktop writes received files up to 512 KB in plaintext into its chat
  history; Android leaves `cacheDir/chat-media/` behind.

## Live frames (touch, drawing, movement)

Sealed `type: "live"` frames carry taps, ink stroke chunks and motion samples
in normalized coordinates. The envelope has `"live": true` and no `persist`,
`notify` or `resend`, so nothing is stored, counted, pushed or acknowledged.
The relay routes them to `_relay_live` (DM sockets only, at most 192 KiB,
sized for a v3 frame sealed to 64 devices):
their own bucket `ROOM_LIVE_MAX_PER_WINDOW` (150 per 10 s), access re-checked
every `ROOM_LIVE_ACCESS_RECHECK_MS`. On join the relay announces
`{"kind":"forkmesh-live","v":1,"perWindow":150,"windowMs":10000}` and clients
pace themselves from it. A shared DM background is a sealed persisted `look`
frame.

On iOS the other person's phone shows in the DM header as a phone outline
that follows their gravity samples:

- The phone draws as its owner sees it. Roll turns it in its own plane first,
  then pitch tilts the top away. Tilting back leans the top away; tilting
  right leans it clockwise. Pitch is capped at about 69° so a phone lying flat
  still shows as a thin slab rather than disappearing edge-on.
- The icon has no glass circle so the outline can fill the 44 pt control.
  Tapping it opens a sheet with a large copy that moves the same way. Tapping
  the large copy sends a tap, and the sheet holds the Floating 3D Face toggle.
- Senders sample at 60 Hz. Receivers time each batch from its newest sample:
  that sample plays `motionPlaybackMarginMs` (100 ms) after the batch arrives,
  and older ones play before it. A new batch lands about one send interval
  (88 ms at 150 frames per 10 s) later, so playback stays smooth. The pose is
  about 100 ms behind the sender. Before this change, timing came from the
  oldest sample with a 120 ms margin, which put the pose a whole batch further
  behind. If a batch arrives late, the pose holds and then catches up; the
  delay does not build up.

## View-once video and voice messages

DMs only. The sender encrypts the recording with a fresh AES-256-GCM key,
uploads the ciphertext, then sends a sealed chat frame carrying
`viewOnce:{v, kind, media, key, iv, mime, durationMs, size, parts}`. Clients
that know `viewOnce` never offer download, save or forward and keep no copy.

API (`app/src/chat_direct_media_api.py`, under
`/api/chat/direct-messages/<conversation>/media`, participants only):

| Route | Who | Does |
| --- | --- | --- |
| `POST /media` | sender | Create an `uploading` row; `parts = ceil(size / 524288)` |
| `PUT /media/<id>/parts/<n>` | sender | Store a 512 KiB part |
| `POST /media/<id>/finish` | sender | Verify parts, mark `ready` |
| `GET /media?ids=` / `GET /media/<id>` | both | State: `uploading`, `ready`, `opened`, `expired`, `withdrawn` |
| `GET /media/<id>/parts/<n>` | recipient | Part data while `ready`; `410 media_gone` after |
| `POST /media/<id>/consume` | recipient | Delete all parts, mark `opened` |
| `DELETE /media/<id>` | sender | Withdraw an unopened message |

- Limits: video 12 MiB / 61 s, voice 4 MiB / 121 s; per sender at most 20
  unopened messages and 64 MiB (`429 media_quota_count` / `media_quota_bytes`).
- Storage: `chat_direct_media` and `chat_direct_media_parts` (parts sealed
  again with `DATA_KEY`). Unopened media expires after 7 days; the hourly
  `chat-prune` phase sweeps expired and abandoned uploads.
- `consume` relays `{"kind":"forkmesh-media","v":1,"id":...,"state":"opened"}`
  to the sender.
- The web holds the `forkmesh-received` receipt for a view-once frame until it
  is opened, expired or withdrawn, because it keeps no local DM history.
- Recording filters reuse the call engine's face, background and voice
  stages ([chat-direct-calls.md](chat-direct-calls.md)); if a requested stage
  cannot start, nothing is recorded rather than sending raw media.
- Screen capture cannot be prevented; platforms that report it blank the
  viewer.

## Recipient counts ("to N")

Every message in a server room shows "to N" (tooltip "Delivered to N people:
@a, @b"). N is the number of distinct accounts the message went to, excluding
the sender, computed by the relay (`chat_room_audience`,
`chat_room_recipients`) once per retained frame:

| Room | N |
| --- | --- |
| Direct message | participants minus the sender (always 1) |
| Private channel | creator plus members, minus the sender |
| Public channel | distinct accounts of live sockets, minus the sender |
| Anonymous rooms (World, repository rooms) | none shown |

The relay strips any client-supplied `recipients` / `recipientNames` and
stamps its own on the outer envelope (names sorted, at most 20,
`CHAT_RECIPIENT_NAMES_MAX`). The sender gets
`{"kind":"forkmesh-recipients","v":1,"ref":...,"recipients":N}`. The stamped
frame is what is stored and replayed, and
`GET /api/chat/channels/<id>/history` lifts the fields to
`messages[].recipients` and `messages[].recipientNames`. Device key lists are
never used as a source, because keys carry no account binding.

## Web client seal (protocol v3 and v4)

The browser speaks [chat protocol v3](chat-protocol-v3.md) in open rooms and
[v4](chat-protocol-v4.md) in strict rooms (server DMs, `direct:<id>`): a
strict room seals and opens only v4 frames, an open room only v3, and v2
frames are ignored everywhere. The v2 X25519 key and peer lists are deleted
from `localStorage` on load.

| File | Role |
| --- | --- |
| `app/public/chat-mlkem.js` | Keccak (SHA3-256/512, SHAKE128/256) and ML-KEM-768, ported from the Android `MlKem768.kt` |
| `app/public/chat-seal.js` | Identity key, prekeys, bundles, trust store, held frames, counters and replay window, v3 and v4 seal/open, the v4 ratchet, safety numbers |
| `app/public/chat-room-transport.js` | Advertises bundles on every connect and after a prekey rotation, opens frames, reseals DMs after an approval, sends and answers `e2ee-reset` |
| `app/public/chat.js`, `chat.html` | New-device banner (Approve, Block, Safety number), system lines, the Safety number view in the encryption panel |

- Ed25519, X25519, HKDF, AES-GCM and SHA-2 come from WebCrypto; private keys
  are imported non-extractable. The identity seed and prekeys sit in
  `localStorage` under `forkmesh.chat/e2ee/`, wrapped with a non-extractable
  AES-GCM key kept in IndexedDB (`forkmesh-chat-e2ee`) when the browser has
  one, plain otherwise. Trust records, counters, the replay window (a 1024-bit
  map per sender) and up to 64 held frames per room are stored next to them,
  keyed by relay host plus trust scope or room.
- `createRoomSealStore({ acct, node })`: `acct` is the signed-in account name
  (lowercased, read when a room is resolved; a guest falls back to `node`),
  `node` is the browser's `forkmesh.chat.selfId`, the `senderId` it already
  writes. `chat.js`, `dashboard-chat.js` and the Office chat pass both.
  `direct:<id>` rooms are strict with that trust scope and are resolved with
  the DM's two account names as `participants`, so a bundle naming anyone else
  is dropped; every other room is open, scoped by room name.
- Bot lines (ForkBot, Claude, Codex) keep the posting browser's `senderId` and
  name the bot in `bot`; `chat.js` and `dashboard-chat.js` render them as the
  bot from that field.
- The relay replays a room's backlog on every connect and the web keeps no
  local history, so v3 frames in the history batch skip the "already seen"
  check (everything else, signature and `ts` window included, still applies).
  Live frames that were already seen are dropped as replays. v4 frames have no
  such exception: once opened, their message key is gone.
- v4 ratchet state is stored per room at `forkmesh.chat/e2ee/ratchetV4/`
  (relay host plus room name, `b64u`), wrapped like the identity seed, in the
  canonical JSON of the spec; it and the counter are written before a sealed
  frame is returned.
- Every tab of a browser is the same device. Each v4 seal and open runs under
  the Web Lock `forkmesh-chat-ratchet` (`navigator.locks`; without it, as in
  Node, it runs unlocked) and reads the ratchet, counter and replay window
  from storage inside the lock, so two tabs never reuse a chain key. A tab
  that opens or seals a v4 frame keeps its plaintext for 10 minutes in
  `forkmesh.chat/e2ee/openedV4/` (wrapped, at most 256 entries, entries over
  16 KB and anything past 256 KB in total are not kept, so a file cannot
  crowd the ratchet out of `localStorage`), keyed by the SHA-256 of the frame
  JSON. Another tab that then gets `replay` for that frame, or `own` for a
  frame this browser sent, shows it from there instead; the tab that handled
  the frame itself still gets `replay` or `own`. An own frame shown this way
  is never acknowledged. This also brings back the browser's own last
  10 minutes of DM messages after a reload.
- An opened frame without `senderId` gets the sender's `node`; a frame whose
  `reactorId` is not the sender's `node` is dropped. Edits and deletes already
  require the original's `senderId`, which v3 now authenticates.
- Frames held for a pending or unknown device are opened when its bundle
  arrives or the user approves it, and go to the page as history, so they do
  not ring.
- Tests: `app/tests/chat_v3_web.test.mjs` (run by
  `app/tests/test_chat_v3_web_vectors.py`) checks the OpenSSL ML-KEM answers
  and every expectation in `conformance/chat/v3/vectors.json` (in a DM room
  resolved with `version: 3`). `app/tests/chat_v4_web.test.mjs` (run by
  `app/tests/test_chat_v4_web_vectors.py`) checks the v4 KDF vectors, replays
  the `sequence` of `conformance/chat/v4/vectors.json`, and runs two and
  three web devices through shuffled delivery, old-chain messages,
  simultaneous starts, resets, peeks, `MAX_SKIP`, two tabs under the lock, and
  the transport's `own`, `noSession` and `e2ee-reset` handling.

## Legacy mesh rooms

A "private room" label on frames in the shared World room is not access
control: every device in that room can decrypt them. Clients no longer create
such rooms; the phones still render old ones. Real private rooms are server
rooms created through the channel API.

## Everyone channels on web `/chat`

The server side is in [chat-everyone-channels.md](chat-everyone-channels.md).
On the web, `kind: "everyone"` channels connect like any server channel. The
shared relay room keeps its wire label `#general` but is displayed as "World"
(`channelDisplayLabel`; frames always use `channelWireLabel`). Signed-in
members land on server #general, guests on World. Management controls are
hidden for these channels, and the members pane reads
`GET /api/chat/channels/{id}/members`.

## Tests

`app/tests/test_chat_recipient_count.py`, `test_chat_channels_api.py`,
`test_chat_direct_messages_api.py`, `test_chat_direct_media_api.py`,
`test_chat_direct_receipts_frontend.py`, `test_chat_view_once_frontend.py`,
`test_chat_everyone_channels_frontend.py`,
`native/ios/ForkMeshCore/Tests/FMChatTests/DirectTypingAndBlockingTests.swift`,
`desktop/tests/test_chat_transport.cpp`, `office/tests/chat.test.mjs`.
