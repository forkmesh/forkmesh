# Read state across devices

If you read a chat on one device, its unread count also clears on your other
devices: web `/chat`, Office, the desktop app, iOS and Android. Home page:
[app/README.md](README.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[chat-unread-reminders.md](chat-unread-reminders.md),
[polling-elimination](../../www/docs/operations/polling-elimination.md).

## Why (issue 1876, 2026-10-09)

Before this change, every client counted unread messages on its own, from the
live frames it received. Reading a conversation on the phone cleared the
phone's badge but left the desktop and browser badges in place. The server
has two read cursors, but neither could do the job:

- The direct-message cursor (`chat_direct_participants.last_read_count`) is
  also advanced by room-access, which runs whenever a device opens a socket.
  A desktop that keeps sockets open therefore marks messages read that nobody
  has seen.
- The channel cursor (`chat_channel_reads.read_at`) is stamped for every
  socket that is live when a message arrives. It only drives the reminder
  email.

The relay cannot tell what a person has seen, so the client that shows the
messages says so instead.

## Read marks

`chat_read_marks` (migration 0255, `app/src/schema.py`) holds one row for each
account and conversation:

| column | meaning |
| --- | --- |
| `account_bi` | blind index of the reader's account |
| `conversation` | cross-client key: `#<name>` (public channel), `private:<32 hex>` (private or office channel), `direct:<32 hex>` (direct message) |
| `read_ts` | the newest message the reader has seen, as the message's own `ts` (stamped by the sender inside the ciphertext, so identical on every device) |
| `updated_at` | server time of the last advance |

Marks only move forward (`MAX`). An account keeps its 500 most recently
advanced marks (`READS_KEEP_MAX`). `GET` returns the newest 200.

Conversations with no shared key are not synced. This covers the phones'
local `#mesh` room and mesh direct messages.

## API (`app/src/chat_reads_api.py`)

- `GET /api/chat/reads` returns `{"marks": {"<conversation>": <ts>},
  "readAt": {"<conversation>": <server ms>}}`. `readAt` is the server time the
  mark last advanced; the site header needs it (see below).
- `POST /api/chat/reads` takes `{"marks": {...}}` with 1 to 50 entries and
  returns `{"ok": true, "advanced": bool}`. A timestamp must be positive and
  at most 24 hours in the future. Any malformed entry rejects the whole
  request with `400 invalid_marks`.

Authentication is the same as for `/api/chat/direct-messages`
(`_ChatDirectMessagesRuntime`): a session for the browser and the phones, or
an Ed25519-signed query (`node`, `ts`, `sig`) for the desktop. The signed
canonical strings are:

- GET: `forkmesh-chat-reads-list-v1\n<node>\n<ts>`
- POST: `forkmesh-chat-reads-mark-v1\n<node>\n<ts>\n<digest>`. The digest is
  the lowercase hex SHA-256 of the cleaned marks, sorted by key, one
  `"<conversation> <ts>"` line each, joined with `\n`
  (`chat_reads_api.marks_digest`).

## The `chat-read` event

A POST that advances any mark calls `notify_account(actor, "chat-read")`. The
account's event socket then delivers the payload-free frame
`{"type":"event","topic":"chat-read"}` to every running client, including the
one that posted. The topic is not in `APNS_PUSH_TOPICS`, so it never produces
a phone banner.

It is in `ACCOUNT_EVENT_SOCKET_EVERY_TOPICS`, which means its socket frame
skips the two-second coalescing window. (`ACCOUNT_EVENT_PUSH_EVERY_TOPICS`
only exempts the APNs push; the socket frame of a coalesced event is still
dropped.) Suppose you read conversation A and then B
within two seconds. With coalescing, a second device could fetch the marks
after A was saved and then miss B, because B's event would be dropped. Each
client already waits about a second and sends its marks in one POST, so
skipping the window does not flood the socket.

## What each client does

All clients follow the same three rules:

1. **Post.** When the user is actually looking at a conversation and the
   client clears its unread count, it queues a mark: the `ts` of the newest
   message loaded in that conversation. Mark-all-read queues every
   conversation. Marks are batched for about a second and posted once. A
   failed post is not retried; the next view posts again.
2. **Apply.** On start, when the event socket reconnects or the app resumes,
   and on every `chat-read` event, the client fetches the marks. It then
   clears unread messages whose `ts` is at or before the mark.
3. **Arrivals.** A message from someone else whose `ts` is at or before the
   known mark does not count as unread. This covers a device that was offline
   and is now replaying history you already read somewhere else.

A client that only keeps a count clears it when the newest message loaded in
the conversation is at or before the mark. If some unread messages are newer
than the mark, the count stays as it is.

| Client | Code | Posts a mark | Fetches marks |
| --- | --- | --- | --- |
| Web `/chat` | `app/public/chat.js`: `queueReadMark`, `flushReadMarks`, `refreshReadMarks`; `channelMeta` entries carry `unreadTs` | opening a conversation, a frame landing in the open conversation, and the tab becoming visible (never while `document.hidden`) | every account-socket connect and every `chat-read` event; a `chat-read` event also re-reads the DM list |
| Office | `office/public/features/chat.js`: `postReadMark`, `loadReadMarks` | `markRead` while the chat panel is in view | with every DM-list read: on open, on every `direct-messages` or `chat-read` topic, and when its account event socket reconnects. Office opens the same `account-events.js` channel as web `/chat`, with the `cookie` session marker (Office is same-origin on `app.forkmesh.com`), so it replaced its 60 s DM-list poll |
| Desktop | `MainWindowNavigation.cpp` (`queueReadMark`, `flushReadMarks`, `applyReadMarks`), `OfficeChannelMirror` (`readMarkKey`, `fetchReadMarks`, `postReadMarks`) | switching to a conversation while chat is on screen, `clearActiveConversationUnread`, a message landing in the visible conversation, `markAllChatRead` | after every DM-list read, on the `chat-read` topic, and when the node event socket reconnects |
| iOS | `FMChat/ViewModels/ChatReadMarks.swift`; `ChatHub.queueReadMark` and `applyReadMarks`; `RelayService.isReadElsewhere` | `ChatHub.select`, a message in the selected conversation, `ChatHub.markAllRead` | at install, sign-in, app resume, `AccountEventChannel.onConnected` and the `chat-read` topic (`AppServices+Chat.swift`) |
| Android | `core/.../chat/ChatReadMarks.kt`; `ChatHub.queueReadMark` and `applyReadMarks`; `RelayRoom.isReadElsewhere` | `ChatHub.select`, a message in the selected conversation, `ChatHub.markAllRead` (the chat tab's unread banner and `ChatFeature.markAllRead` now go through it) | on session start and sign-in, `AccountEventChannel.onConnected` and the `chat-read` topic (`ChatDepth.kt`) |

Key mapping: the desktop's office mirror rooms map to `private:<id>` or
`direct:<id>`, and its `#name` channels keep their name. On the phones,
`#office/<name>` maps to `private:<room id>`, `direct:<id>` stays as it is,
and relay channels keep their `#name`. The local mesh room and `@peer`
conversations are not synced.

## Site header chat icon

The chat icon in the shared site header (`www/public/site-header.js`,
`renderChatBadge`) counts `#general` messages and new members since this
browser last opened `/chat`, from the public `/api/chat/activity` counters and
the `forkmesh.chat.activitySeen` baseline in `localStorage`. That baseline is
per browser, so on its own the icon stayed lit on every device you had not
opened `/chat` on.

When the icon would show a count and you are signed in, the header reads
`/api/chat/reads` (with `credentials: "include"`, since on forkmesh.com it is a
cross-origin call to the app) and clears the icon, writing a new baseline, when
both:

- `readAt["#general"]` is at or after `latestMessageTs` from
  `/api/chat/activity`. That value is the server's store time from
  `chat_history`, while marks hold the sender's clock from inside the
  encrypted message, so the mark alone cannot be compared with it.
- the mark itself is within 5 minutes (`CHAT_READ_CLOCK_SLACK_MS`) of
  `latestMessageTs`. That stops a device that posted a mark from an old,
  partly loaded view from clearing an icon for newer messages.

New-member counts are cleared with the messages: opening `/chat` already
treats them as seen. The header has no account socket of its own, so it checks
again when the tab becomes visible while the icon is lit, rather than live.

## Direct-message counts

The DM count that web, Office and the phones show still comes from the
server's `unreadCount`. Every client that reads a DM also posts
`/direct-messages/<id>/read`, and that call is what zeroes the count for the
other devices. The mark covers the counts that the clients keep themselves.

Tests: `app/tests/test_chat_reads_api.py`,
`app/tests/test_chat_desktop_room_and_dm_proofs.py`,
`app/tests/test_chat_read_marks_clients.py`, `office/tests/chat.test.mjs`,
`desktop/tests/test_chat_transport.cpp` (proof strings),
`native/ios/ForkMeshCore/Tests/FMChatTests/ChatReadMarksTests.swift` and
`native/android/core/src/test/.../chat/ChatReadMarksTest.kt`.
