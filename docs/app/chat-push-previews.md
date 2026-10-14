# Message text in chat push notifications

A push for a direct message or a channel message now shows who wrote it and
what they wrote, and the server still never sees the text. Home page:
[app/README.md](README.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[mobile push notifications](../../www/docs/operations/mobile-push-notifications.md).

## Why (2026-10-13)

Asked: "make it so that chat text shows in the push notifications - signal
has a way of doing it securely so lets do it like they do". Before this, a
chat push read "Direct message from alice" / "New message in #design" with
the body "End-to-end encrypted — open Chat in ForkMesh to read it.", because
the relay only holds ciphertext.

## The Signal model

Signal's push carries no message content. It only wakes the phone. The
phone then fetches the still-encrypted message from the server, decrypts it
on the device and shows the text. Apple and the server see neither the text
nor the sender's words.

ForkMesh does the same:

1. A sender's client writes a sealed frame into the room. The relay stores
   it in `chat_history` (direct messages until the recipient's device
   receives it, channels for the retention window).
2. The relay sends a push that names only where the ciphertext is: the
   conversation, the relay `ref` of the frame, and the room key version.
3. The phone's notification code fetches that one frame from a read-only
   route, derives the room key, unseals the frame with its own device key
   and replaces the banner text.
4. If any step fails (row already gone, frame not sealed for this device,
   timeout, the user turned previews off), the banner stays the generic text
   the server sent.

Nothing derived from plaintext is added to the push; the server has none.

## Push payload (APNs)

Built by `direct_message_alert`, `chat_message_alert` and `alert_payload` in
`app/src/apns.py`.

Direct message (`topic` `direct-messages`):

```json
{
  "aps": {
    "alert": {
      "title": "📨 Direct message from alice",
      "body": "End-to-end encrypted — open Chat in ForkMesh to read it."
    },
    "sound": "default",
    "mutable-content": 1,
    "thread-id": "direct:<32 hex>"
  },
  "conversation": "direct:<32 hex>",
  "ref": "<40 lowercase hex>",
  "keyVersion": 1,
  "sender": "alice",
  "topic": "direct-messages",
  "origin": "https://app.forkmesh.com"
}
```

Channel message (`topic` `chat`), only when the server channel id is known:

```json
{
  "aps": {
    "alert": {"title": "💬 New message in #design", "body": "End-to-end encrypted — open Chat in ForkMesh to read it."},
    "sound": "default",
    "mutable-content": 1,
    "thread-id": "#office/design"
  },
  "conversation": "#office/design",
  "channelId": "<32 hex>",
  "ref": "<40 lowercase hex>",
  "keyVersion": 2,
  "topic": "chat"
}
```

| Field | Rule |
| --- | --- |
| `ref` | 40 lowercase hex, the relay ref of the frame that triggered the push (SHA-256 of the frame text, first 40 hex). Dropped when malformed |
| `keyVersion` | Integer 1..1000000000, the key version of the room the frame is in. Dropped when malformed |
| `channelId` | 32 hex, the server channel id. Channel pushes only |
| `conversation` | `direct:<id>` or `#office/<name>`, as before (2026-10-03) |
| `thread-id` | The conversation, so iOS groups notifications per conversation. A channel push without a conversation uses `channel:<id>`. A ping keeps its own `threadId` |
| `aps.alert` | The generic fallback: title as before, body "End-to-end encrypted …". Shown only when the phone cannot decrypt |

`ref`, `keyVersion` and `channelId` are only ever added for the two
conversation topics (`APNS_CONVERSATION_TOPICS`). A "started a direct
conversation" push has no frame behind it and carries neither `ref` nor
`keyVersion`. The public World room push carries no `channelId` and no `ref`,
because that room has no server channel to fetch from.

### No collapsing for direct messages

Each message now shows its own text, so a direct message push is never
folded into the previous one:

- `collapse_id_for("direct-messages")` is `""` (`APNS_COLLAPSE_TOPICS` is only
  `chat`), so iOS keeps one notification per message, grouped by
  `thread-id`.
- `direct-messages` is in `ACCOUNT_EVENT_PUSH_EVERY_TOPICS` and
  `ACCOUNT_EVENT_SOCKET_EVERY_TOPICS` in `app/src/entry.py`, so the 2-second
  per-account coalescing in `notify_account_event` no longer drops the second
  message of a burst, neither the push nor the socket frame.
- Channel pushes are unchanged: still collapsed under `chat` and still one per
  room every 2 seconds (`CHAT_ROOM_PUSH_COALESCE_MS`).

### Where the fields come from

- `ForkMeshRoom._maybe_retain` knows the stored row id. A direct message row
  is `<ref>:<sender bi16>[:n]` in room `chat-direct:<id>:v<n>`, a channel row
  is the bare ref in `chat-channel:<id>:v<n>`. It passes `msg_id[:40]` and the
  socket's `direct_version` / `channel_version` on.
- `_chat_direct_message_retained(..., ref=, key_version=)` passes them to
  `direct_message_alert` and so to `notify_account_event`.
- `push_chat_room_apns(..., ref=, key_version=)` passes them, with the
  channel id, to `chat_message_alert`.

## Account event socket frame (Android)

Android has no push transport. `BackgroundLinkService` holds the account
event socket instead. For `direct-messages` the frame now carries the same
handles:

```json
{"type":"event","topic":"direct-messages","repo":"",
 "conversation":"direct:<32 hex>","ref":"<40 hex>","keyVersion":1}
```

`notify_account_event` adds `&conversation=&ref=&keyVersion=` to the internal
`/api/nodes/notify` call (`_account_event_query`, built from the push alert),
and the `ForkMeshNodes` object copies the valid ones into the frame
(`_account_event_direct_fields`). A "started a conversation" event has no
`ref`. Every other topic's frame is unchanged.

## Pending-frame routes

Both routes only read. They do not mark anything read, do not delete or
acknowledge the stored row, do not move `last_read_count` or
`chat_channel_reads`, and do not announce a read to the sender. A phone that
shows a preview has not "received" the message: the app still receives it
over the room socket later and sends `forkmesh-received` then.

`GET /api/chat/direct-messages/<id>/pending?ref=<40 hex>`
(`chat_direct_messages_api._pending`)

- Session auth like the other DM routes (browser or phone session; the desktop
  signed proof also works through `_ChatDirectMessagesRuntime.session`).
- Participants only: anyone else, and an unknown conversation, gets
  404 `{"error":"not_found"}`.
- `ref` must be 40 hex (uppercase is lowered), otherwise 400
  `{"error":"invalid_ref"}`. Only `GET` (405 otherwise).
- Looks up the row in the current key version's room whose id starts with
  `<ref>:` (range query on the primary key).
- 200:
  `{"conversation":"direct:<id>","room":"chat-direct:<id>:v<n>","keyVersion":n,"passphrase":"<hex>","frame":"<stored outer envelope JSON string>"}`.
- 404 `{"error":"not_found"}` when the row is gone, which for a direct message
  means a device of the recipient already received it (or it expired, or the
  sender withdrew it with `forkmesh-superseded`).

`GET /api/chat/channels/<id>/pending?ref=<40 hex>`
(`chat_channels_api._pending`)

- Same session auth as the other channel routes.
- `_readable_channel`: members and the creator of a private channel, anyone
  signed in for a public channel; administrators get no extra access to
  private channels. Otherwise 404.
- Reads row `<ref>` in `chat-channel:<id>:v<n>`.
- 200: `{"channel":"<id>","room":"chat-channel:<id>:v<n>","keyVersion":n,"passphrase":"<hex>","frame":"<stored outer envelope JSON string>"}`.
- 404 when the row is gone (pruned or expired).

Routing: `CHAT_DIRECT_MESSAGE_PENDING_RE` and `CHAT_CHANNEL_PENDING_RE` in
`app/src/urls.py`, matched in `app_runtime.py`. Error logs record the path as
`/api/chat/direct-messages/[opaque]` / `/api/chat/channels/[opaque]`
(`_privacy_safe_log_path`), and Sentry URLs drop the query string, so the ref
is not logged.

`frame` is the outer room envelope exactly as stored (`{"kind":"cipher",...}`
with the relay's `ref`, `recipients` stamps). The client decrypts it with the
room key derived from `passphrase` and `room` (PBKDF2, the same derivation as
the room socket), then unseals the inner frame with its device key (RoomSeal)
for sealed frames. Public channel frames are plain inside the room layer.

## Clients

| Client | What it does |
| --- | --- |
| iOS | `native/ios/NotificationService` decrypts when the payload has `ref` and either a `direct:` conversation or a `channelId`: GET the pending route with the shared session, derive the room key (derived keys cached in the shared keychain group so later pushes skip the 210k PBKDF2 iterations), unseal with the device X25519 key, parse the `chat` / `thread-reply` plain. Direct message: title is the sender, body the text (or "📎 <fileName>"). Channel: title "<sender> in #<name>", body the text. Sets `threadIdentifier` to the conversation and `userInfo["messageId"]` to the message id. On any failure the server's generic alert stays. The extension never acknowledges, marks read or sends anything to the relay. The app shares the device key name and the preview setting through `PushCredentialStore`. A delete-for-everyone removes the delivered notification for that `messageId`. Details: [native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md) |
| Android | `BackgroundLinkService` reads the `direct-messages` socket frame. With `conversation` and `ref` it fetches the pending route, decrypts with the same code as `DirectMessageService` and posts one notification per message (sender as title, text as body), grouped per conversation. Falls back to the earlier text when decrypting fails. Details: [native/ANDROID.md](../native/ANDROID.md) |
| Desktop | No push. Its own notifications already show the decrypted text, because the desktop holds the room sockets itself. Nothing changed |
| Web and Office | No push. Unchanged |

Notification content setting on both phones, like Signal's "Notification
content": "Name and message" (default), "Name only", "No name or message".
The phone applies it after decrypting; the server always sends the same
generic payload.

## Limits

- A direct message row is deleted as soon as one of the recipient's devices
  receives it. If the app is open on another device, the row can be gone
  before the phone's extension asks for it, and that banner stays generic.
- A preview needs the frame to be sealed for the phone's device key. A frame
  sealed before the phone's key reached the sender shows the generic banner
  (the sender's re-seal later sends a new frame, which is not pushed again).
- Only the current key version is served. A frame from before a key rotation
  shows the generic banner.
- Channel pushes are still coalesced per room, so the second message of a
  burst inside two seconds gets no push of its own.

Tests: `app/tests/test_apns_push.py` (payload fields, malformed handles,
collapse, coalescing, socket frame), `app/tests/test_chat_pending_routes.py`
(both routes: authorization, gone rows, no side effects, routing),
`app/tests/test_chat_recipient_count.py`
(`test_only_new_messages_raise_the_direct_unread_count` checks the ref and
key version handed to the push).
