# Message text in chat push notifications

Direct-message and channel pushes show the sender and text, while the server
never sees plaintext. Home page: [app/README.md](README.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[mobile push notifications](../../www/docs/operations/mobile-push-notifications.md).

## Model

1. The relay stores the sender's sealed frame in `chat_history`.
2. The push (`direct_message_alert`, `chat_message_alert` in `app/src/apns.py`)
   carries `conversation`, the frame's relay `ref` (40 hex), `keyVersion`
   and, for channels, `channelId`, with a fallback alert ("Direct message
   from X" / "New message") and `mutable-content`. A direct-message push also
   carries the sealed frame itself as `frame` (relay routing keys `ref`,
   `recipients`, `recipientNames`, `historyReplay` stripped) whenever the
   payload still fits Apple's 4 KB; a longer frame is left out and fetched.
3. The phone asks the pending route for the room passphrase (and the stored
   frame), derives the room key, unseals the frame with its device key and
   rewrites the banner to the sender and the message text, as Signal does.
4. On any failure the fallback alert stays.

Why the frame rides in the push (2026-10-15): the recipient's desktop or
browser is usually connected to the conversation, receives the message within
milliseconds and acknowledges it, and that acknowledgement deletes the relay's
copy. The phone's fetch came a second later, got 404 and the banner stayed
"End-to-end encrypted — open Chat in ForkMesh to read it." The frame is
ciphertext sealed to device keys, so Apple sees nothing more than before.

Android has no push transport; the account event socket frame for
`direct-messages` carries the same fields. Direct-message pushes are never
collapsed or coalesced; channel pushes are coalesced per room.

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

Limits: a direct-message frame too long for the push whose row is already
gone, frames not sealed for the phone, and frames from an older key version
show the fallback banner.

Tests: `app/tests/test_apns_push.py`, `app/tests/test_chat_pending_routes.py`.
