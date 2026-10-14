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
- Joining: `GET|POST /api/chat/invites/<token>` previews or joins, under the
  `MAX_CHANNEL_MEMBERS` cap. Removing any member, deleting the room, or
  changing its creator invalidates the link.
- The web link is `/chat#invite=<token>`: a fragment, so the token never
  reaches the server in a page request or `Referer`. Request logs record
  `/api/chat/invites/[token]`.

## Server copies of direct messages

The relay keeps an encrypted direct message only until a recipient device has
it. Clients show "N messages on the server" or "Not stored on the server".

- For a `persist: true` cipher frame in a direct room the relay stamps
  `"ref":"<sha256(frame)[:40]>"` on the outer envelope and stores the frame
  before fan-out. `chat_history.msg_id` is `<ref>:<sender account_bi[:16]>`,
  plus `:n` when the sender marked the frame `notify` (a real message).
- A recipient client sends `{"kind":"forkmesh-received","v":1,"refs":[...]}`
  (at most 64 refs) after opening a frame; `chat_direct_received` deletes the
  matching rows not sent by the acknowledging account. A frame the device
  cannot open is not acknowledged.
- The relay reports `{"kind":"forkmesh-server-copies","v":1,"stored":N}`
  after a store, a receipt, and on join. Unacknowledged rows expire after 7
  days.
- Only a `notify` frame that is not a `resend` raises the unread count and
  sends a push.
- Channels have many readers: no `ref`, receipts ignored.

### Re-sealing

A device seals a DM's content key only for device keys it has learned in that
room (`e2ee-key` frames). Every client follows these rules in a DM room:

1. Never acknowledge an `e2ee-key` frame, so key advertisements stay on the
   relay for late devices.
2. When a sealed frame is not wrapped for this device, send one persisted key
   advertisement; this tells the sender to re-seal.
3. Keep an outbox of up to 64 own durable frames. When a new peer key is
   learned, reseal entries not wrapped for all known keys, send them with the
   same message id and `"resend": true`, then send
   `{"kind":"forkmesh-superseded","v":1,"refs":[...]}` for the old refs.
4. Receivers drop message ids already shown.

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
| Delivered | Two overlapping rings with ticks | `forkmesh-delivered` named its ref (DMs only) |
| Read | Two filled rings in the accent colour | `forkmesh-read` arrived after it was sent (DMs only) |

- `{"kind":"forkmesh-delivered","v":1,"refs":[...]}`: forwarded by
  `ForkMeshRoom._direct_received` to the other account's sockets when a
  device acknowledges.
- `{"kind":"forkmesh-read","v":1}`: sent when `_mark_read` moves the reader's
  `last_read_count`. A hidden tab defers its `/read` until visible.
- Channels reach only sending and sent. Ticks are not persisted and change
  only while the sender's client has the conversation open.

## Replies, reactions, edits and deletes in DMs

DMs use the same sealed frames as channels: `thread-reply` (persist and
notify), `reaction`, `edit` and `delete` (persist). Reply and react work on
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

## Live frames (touch, drawing, movement)

Sealed `type: "live"` frames carry taps, ink stroke chunks and motion samples
in normalized coordinates. The envelope has `"live": true` and no `persist`,
`notify` or `resend`, so nothing is stored, counted, pushed or acknowledged.
The relay routes them to `_relay_live` (DM sockets only, at most 16 KiB):
their own bucket `ROOM_LIVE_MAX_PER_WINDOW` (150 per 10 s), access re-checked
every `ROOM_LIVE_ACCESS_RECHECK_MS`. On join the relay announces
`{"kind":"forkmesh-live","v":1,"perWindow":150,"windowMs":10000}` and clients
pace themselves from it. A shared DM background is a sealed persisted `look`
frame.

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
`desktop/tests/test_chat_transport.cpp`, `office/tests/chat.test.mjs`.
