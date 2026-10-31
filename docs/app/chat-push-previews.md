# Message text in chat push notifications

Direct-message and channel pushes show the sender and text once the phone has
opened the frame, while neither the server nor Apple sees the text, the
sender or the channel name. Home page: [app/README.md](README.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[mobile push notifications](../../www/docs/operations/mobile-push-notifications.md).

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

Since 2026-10-17 the iPhone opens a DM push that carries its frame without any
request when it already holds that room's key from an earlier push (the room
name is `chat-direct:<id>:v<keyVersion>`, cached only as a hash), and retries
the pending fetch once on a network error, 429 or 5xx. A photo, voice, video
or view-once message with no text says what arrived instead of "New message".
Details: [native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md).

Why the sender and channel left the payload (2026-10-16, chat protocol v3,
[chat-protocol-v3.md](chat-protocol-v3.md#push-notifications)): the alert
text and custom keys reach Apple in the clear, so "Direct message from X",
"New message in #design" and `"sender":"x"` told Apple who wrote to whom and
where, even with previews hidden on the phone. The push for a newly started
conversation now says "New direct conversation" for the same reason. A
channel push carries `channelId` and no `conversation`; its `thread-id` is
`channel:<id>`. Until 2026-10-16 an office channel push also carried
`conversation: "#office/<name>"` and used it as the `thread-id`, which named
the room to Apple; clients now route an office push by `channelId`. Direct
messages keep `conversation: "direct:<id>"` (an opaque id) and the mesh wake
keeps `#mesh`.

A v3 frame no longer fits: a DM frame is sealed to at least two devices and
each carries a 1088-byte ML-KEM ciphertext, so the outer envelope is about
6 KB for two devices. `_sealed_frame` skips a frame string longer than the
4 KB payload without parsing it, and the phone always takes the pending
route. The recipient's desktop or browser usually acknowledges the frame
before the phone's fetch (the 2026-10-15 note below), so an acknowledged row
is kept for a preview grace instead of being deleted at once; see "Preview
grace" below.

Why the frame rides in the push (2026-10-15): the recipient's desktop or
browser is usually connected to the conversation, receives the message within
milliseconds and acknowledges it, and that acknowledgement deletes the relay's
copy. The phone's fetch came a second later, got 404 and the banner stayed
"End-to-end encrypted — open Chat in ForkMesh to read it." The frame is
ciphertext sealed to device keys, so Apple sees nothing more than before.

Android has no push transport; the account event socket frame for
`direct-messages` carries the same fields. Direct-message pushes are never
collapsed or coalesced; channel pushes are coalesced per room.

## Preview grace

`forkmesh-received` no longer deletes the matching DM rows: `chat_direct_received`
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
([native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md)) and Android
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
per-room coalescing memo. Before 2026-10-22 any room without a channel id
woke every registered phone, and legacy rooms take sockets without signing
in, so anyone could buzz every phone once per coalescing window per room name.

Tests: `app/tests/test_apns_push.py`, `app/tests/test_chat_pending_routes.py`.
