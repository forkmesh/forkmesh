# Message text in chat push notifications

Direct-message and channel pushes show the sender and text once the phone has
opened the frame, while neither the server nor Apple sees the text, the
sender or the channel name. Home page: [README.md](README.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[mobile push notifications](../operations/mobile-push-notifications.md).

## Model

1. The relay stores the sender's sealed frame in `chat_history`.
2. The push (`direct_message_alert`, `chat_message_alert` in `app/src/apns.py`)
   carries `conversation`, the frame's relay `ref` (40 hex), `keyVersion`
   and, for channels, `channelId`, with the fallback alert title
   "New message" (with the usual icon), body "New message", and
   `mutable-content`. There is no `sender` field and the title names no
   sender or channel. A direct-message push also carries the sealed frame
   itself as `frame` (relay routing keys `ref`, `recipients`,
   `recipientNames`, `historyReplay` stripped) whenever the payload still
   fits Apple's 4 KB; a longer frame is left out and fetched.
3. The phone asks the pending route for the room passphrase (and the stored
   frame), derives the room key, opens the v3 frame with its prekeys and
   trust records, and rewrites the banner to the sender and the message text
   according to its preview setting, as Signal does. A frame from a pending
   or unknown device keeps "New message".
4. On any failure the fallback alert stays.

The iPhone opens a DM push that carries its frame without any request when it
already holds that room's key from an earlier push (the room name is
`chat-direct:<id>:v<keyVersion>`, cached only as a hash), and retries the
pending fetch once on a network error, 429 or 5xx. A photo, voice, video or
view-once message with no text says what arrived instead of "New message".
Details: [docs/native/ios/FMChat.md](../native/ios/FMChat.md).

**Why no sender or channel in the payload**
([chat-protocol-v3.md](chat-protocol-v3.md#push-notifications)): alert text and
custom keys reach Apple in the clear, so "Direct message from X" or
`"sender":"x"` would tell Apple who wrote to whom and where, even with
previews hidden. A newly started conversation says "New direct conversation"
for the same reason. A channel push carries `channelId` and no
`conversation`; its `thread-id` is `channel:<id>`, and office pushes are routed
by `channelId` too (a `#office/<name>` conversation would name the room).
Direct messages keep `conversation: "direct:<id>"` (an opaque id) and the mesh
wake keeps `#mesh`.

**Why the frame rides in the push, and the grace period.** The recipient's
desktop or browser usually receives and acknowledges the message within
milliseconds, before the phone's fetch, and an acknowledgement would delete
the relay's copy. The frame is ciphertext sealed to device keys, so carrying
it shows Apple nothing more. But a v3 DM frame is sealed to at least two
devices, each with a 1088-byte ML-KEM ciphertext (about 6 KB for two), so it
rarely fits: `_sealed_frame` skips a frame string longer than the 4 KB payload
without parsing it, and the phone takes the pending route. So an acknowledged
row is kept for a preview grace instead of being deleted at once (below).

Android has no push transport; the account event socket frame for
`direct-messages` carries the same fields. Direct-message pushes are never
collapsed or coalesced; channel pushes are coalesced per room.

## Preview grace

`forkmesh-received` does not delete the matching DM rows: `chat_direct_received`
sets `chat_history.acked_at` (migration 0269) on them. For
`CHAT_DIRECT_ACKED_GRACE_MS` (120 s) such a row is readable only through
`GET /api/chat/direct-messages/<id>/pending?ref=` (`PENDING_ACKED_GRACE_MS`
in `chat_direct_messages_api.py`, kept equal by a test), and only while it is
also inside the 7-day retention window. Everything else treats it as gone:
history replay (`chat_history_recent`), HTTP polling (`chat_history_since`),
the "N messages on the server" count (`chat_direct_server_copies`), the
unread reminder email (`direct_reminders.py`), the per-room count and byte
budget (`_chat_history_prune_bounds`) and the delivery receipt route.

The row is then deleted:

- on the next receipt in that room (`chat_direct_acked_prune`);
- by the room's Durable Object alarm, set to 121 s after the first pending
  acknowledgement (`ForkMeshRoom._schedule_acked_prune`, `alarm`);
- by the `chat-prune` cron phase (`chat_history_prune_expired`), as a
  backstop if the alarm was lost with its process.

`forkmesh-superseded`, the 7-day expiry and the account and repository
deletions still delete at once, acknowledged or not. A view-once frame's
media parts are deleted by `consume` as before; the grace only keeps the
ciphertext frame (whose media key then opens nothing). Channels have no
receipts, so the channel pending route is unchanged.
`app/tests/test_chat_pending_routes.py`,
`app/tests/test_chat_history_byte_budget.py` and
`app/tests/test_chat_recipient_count.py` cover it.

## Pending-frame routes

- `GET /api/chat/direct-messages/<id>/pending?ref=<40 hex>` (participants)
- `GET /api/chat/channels/<id>/pending?ref=<40 hex>` (channel readers)

Both return `room`, `keyVersion`, `passphrase` and the stored `frame`; 404 for
non-members, 400 `invalid_ref`. The direct-message route answers a gone row
with `"frame": ""` so the phone can open the frame its push carried; the
channel route still 404s one. They are strictly read-only: no
read marks, acknowledgements, deletions or receipts. Logged paths are redacted
and query strings dropped.

## Delivery receipt route

`POST /api/chat/direct-messages/<id>/delivered?ref=<40 hex>` (participants) is
the one write a push handler makes. It needs a stored row for that ref sent by
the other participant, then asks the room to send
`{"kind":"forkmesh-delivered","v":1,"refs":[ref]}` to the sender's open sockets
(`_announce_chat_direct_delivered`, room path `.../v<n>/delivered`). It does not
delete the row, mark anything read or count as `forkmesh-received`: the phone
keeps no DM history, so the app still needs the server copy when it opens.
404 for non-members, own messages or gone rows; 400 `invalid_ref`.

Clients: `native/ios/NotificationService`
([docs/native/ios/FMChat.md](../native/ios/FMChat.md)) and Android
`BackgroundLinkService` ([native/ANDROID.md](../native/ANDROID.md)). Both
phones offer a notification-content setting applied after decryption.

Limits: a direct-message frame too long for the push (every v3 frame) whose
row is already gone, frames not sealed for the phone, frames from a pending
device, and frames from an older key version show the fallback banner.

## Which rooms wake phones

A retained room frame marked `notify: true` wakes phones through
`push_chat_room_apns` only for a channel room (the socket carries a
`channel_id`: a private channel's members, or every registered phone for a
public channel) or the public world room (`PUBLIC_WORLD_GENERAL_ROOM_KEY`).
Every other room key, such as `legacy:room:<name>` or
`repo:<owner>/<repo>:room:<name>`, wakes nobody and is not entered in the
per-room coalescing memo: legacy rooms take sockets without signing in, so
letting them wake phones would let anyone buzz every phone.

Tests: `app/tests/test_apns_push.py`, `app/tests/test_chat_pending_routes.py`.
