# Private rooms and direct messages

Who can see a chat conversation, and where that rule is enforced. Home page:
[app/README.md](README.md). Related: [desktop/README.md](../desktop/README.md),
[native/README.md](../native/README.md), [security/README.md](../security/README.md),
[chat-unread-reminders.md](chat-unread-reminders.md).

## The rule

- A **private room** (`chat_channels` row whose sealed record says
  `visibility: "private"`) is visible to, joinable by and readable by its
  creator (`created_by_bi`) and the accounts with a `chat_channel_members`
  row. Nobody else. Platform administrators get no implicit access, not even to
  the channel list, room access, history, the member list or the live socket.
- Every member is an explicit invitation: `POST /api/chat/channels` with
  `members`, `POST /api/chat/channels/<id>/members`, or the room's invite
  link (see [Invite links](#invite-links-2026-10-12)). Creating a room no
  longer drops administrators from `members`, and inviting one is an ordinary
  row, not the old `implicitAdmin` answer.
- A **direct message** (`chat_direct_conversations`) belongs to its two
  participants (`chat_direct_participants`). `pair_bi` is unique per pair, so
  every "Message" action for the same two people returns the same
  conversation. There are no group DMs: a private room with the chosen people
  is the group conversation.
- A non-member gets `404 {"error":"not_found"}` from every channel endpoint,
  exactly as for a channel id that does not exist: listing (the room is left
  out), `room-access`, `history`, `members` (GET, POST, DELETE), `PATCH`,
  `DELETE`, `webhooks` and `webhooks/<id>`. A member who is not the creator
  still gets `403 manager_required` from manager actions, because they already
  know the room exists.
- Public channels are unchanged: every signed-in account reads them, and an
  administrator may manage them.

## Where it is enforced

| Surface | Code |
| --- | --- |
| Channel API | `app/src/chat_channels_api.py`: `_readable_channel`, `_can_manage`, `_require_manager`, `_list_channels` |
| Channel socket admission | `app/src/entry.py` `_chat_channel_socket_handler` |
| Live socket re-check | `app/src/entry.py` `ForkMeshRoom` access re-check (creator, public, or member row) |
| Office (World) meeting socket | `world/src/world_office_routes.py` `_office_channel_socket_handler`; live re-check `world/src/world_durable_objects.py` `ForkMeshOfficeRoom._access_current` |
| Push audience | `app/src/entry.py` `_chat_channel_push_accounts` (creator plus member rows; unchanged) |
| Direct messages | `app/src/chat_direct_messages_api.py` (participant rows only) and `_chat_direct_socket_handler` |

The Office live re-check also used to refuse a room's creator, who has no
member row; it now admits the creator like every other check does.

## Desktop key-signed requests

The desktop has an account key, not a session token, so each write it makes
is a proof bound to the exact resource and body (`_chat_channel_signed_session`
and `_chat_direct_signed_session` in `app/src/entry.py`,
`forkmesh::office::*Proof` in `desktop/src/OfficeChannelMirror.cpp`):

- `forkmesh-chat-channel-create-v1\n<node>\n<ts>\n<sha256(fields)>` where
  `fields` is `name=<name>\nvisibility=<v>\nmembers=<a,b,...>` (names
  lower-cased, in the order sent). Changing the name, visibility or any
  invitee breaks the signature.
- `forkmesh-chat-channel-members-v1\n<node>\n<channel>\n<ts>\n<METHOD>\n<sha256(username)>`
  for GET (empty username), POST and DELETE on `/members`.
- `forkmesh-chat-direct-list-v1\n<node>\n<ts>`,
  `forkmesh-chat-direct-start-v1\n<node>\n<ts>\n<sha256(username)>`,
  `forkmesh-chat-direct-access-v1\n<node>\n<conversation>\n<ts>` and
  `forkmesh-chat-direct-read-v1\n<node>\n<conversation>\n<ts>`.

`app/tests/test_chat_desktop_room_and_dm_proofs.py` and
`desktop/tests/test_chat_transport.cpp` pin the same digest, so the two sides
cannot drift apart.

## Clients

- **Web chat** (`app/public/chat.js`, `chat.html`): the create dialog says who
  can see the room ("Only you and @a, @b can see or join this room. Nobody
  else, including administrators, is added.") and hides the invite field for
  public channels. In a private room or DM the people pane lists only that
  room's members ("Members · N") instead of everyone online. `?dm=<username>`
  opens (or reuses) a DM; the @mention card and the People pane have a
  Message button; public profiles (`04-account.js`) link Message to
  `/chat?dm=<name>`. The `private` flag on a frame is only a reason to drop it
  in public World #general (legacy mesh rooms); inside a server room the room
  itself is the access control, so private-room messages from the desktop and
  phones are no longer dropped by the browser.
- **Desktop** (`OfficeChannelMirror`, `MainWindowMessages.cpp`): "New private
  room" creates a server room with a people picker and the same audience line;
  "Invite" and the Settings dialog's "Who can see this room" list add and
  remove members through the signed members endpoint. Direct messages are
  server conversations under `#office/@<username>`: "New message" (sidebar
  and the New channel menu), the profile card's Message button and a click on
  an @mention all open or reuse one. They work when the other person is
  offline and interoperate with web DMs (frames carry `channel:
  "direct:<id>"`, as the browser sends them).
  A typed message shows in the conversation at once (2026-10-06). Before,
  `OfficeChannelMirror::sendMessage` only queued the text: it was not shown
  until the room's encrypted socket was up and `ServerNode::sendChat` echoed
  it. A DM room with no live socket (only the first 8 keep one) first has to
  fetch room access, derive the key and connect, so the reply seemed to vanish
  for seconds. Now `sendMessage` gives the queued text a message id and emits
  it as an own message straight away. `flushPending` sends it with that same
  id (`ServerNode::sendChatWithId`), so the socket's echo and the relay copy
  are dropped as already seen. If the send fails, `messageSendFailed` carries
  the id; the window removes that row and puts the text back in the composer.
- **iOS and Android**: the create sheet offers Public or Private; Private
  creates a server room with the listed invitees (it was a mesh room before,
  and the server option turned public whenever no member was typed). Not built
  or run here; the Swift and Kotlin toolchains are not on the agent box.

## Invite links (2026-10-12)

Issue #1888: the room's creator shares one link, and whoever opens it joins
the room and chats in ForkMesh, without the creator typing their username.

- **Table** `chat_channel_invites` (migration `0256`): one row per private
  room, keyed by `channel_id`. `token_bi` is the blind index of
  `chat-invite-token:<token>`; the token itself and the creator's name sit in
  the sealed `data`, so the creator can copy the same link again and a re-key
  recomputes `token_bi` instead of breaking shared links
  ([REKEY.md](../REKEY.md)).
- **Manager endpoints** `GET|POST|DELETE /api/chat/channels/<id>/invite`
  (`_require_manager`, so only the creator; a non-member gets the usual 404).
  GET returns the current link or `null`, POST makes a new one (replacing the
  old token, which stops working), DELETE turns it off. Only private rooms take
  a link (`409 private_channel_required`): a public channel is already open to
  every signed-in account, so its "Copy invite link" copies `/chat#room=<id>`.
- **Joining** `GET|POST /api/chat/invites/<token>` (any signed-in account).
  GET previews the room name, who invited, and whether the caller is already
  in it; POST adds a `chat_channel_members` row with `invited_by_bi` set to
  the link's creator, under the same `MAX_CHANNEL_MEMBERS` cap, and pushes
  `private-channels` to the joiner and the creator so both lists refresh. A
  link whose `created_by_bi` no longer matches the room's creator, or that
  points at a notices or Office room, answers 404 like an unknown token.
- **Revocation**: removing any member deletes the link (`inviteReset: true`
  in the DELETE answer), so the person removed cannot rejoin with the link
  they still hold; the creator makes a new one. Deleting the room, or deleting
  or renaming the creator's account, deletes it too.
- **Logs**: `_privacy_safe_log_path` records `/api/chat/invites/[token]`, so
  the token is never written to request logs.
- **Web**: the link is `https://<origin>/chat#invite=<token>`. A fragment, not
  a query, so the token never reaches the server in the page request or a
  `Referer`. The room settings panel has "Invite link" (Create link / Copy
  link, New link, Turn off), and the header's "Copy invite link" button copies
  it for the creator of a private room. Opening the link signed out goes to
  `/login?next=/chat#invite=<token>` and comes back; signed in, a "Join
  #name?" dialog shows who invited you, and "Join room" opens the room. A
  member who opens it again just lands in the room.
- Desktop and phones still add members by name; they can open a web link in
  the browser to join.

## Server copies of direct messages

The relay keeps an encrypted direct message only until the other person's
device has it. Both participants see the state at the top of the
conversation: "1 message on the server" ("N messages on the server") while a
copy is waiting, and "Not stored on the server" once every copy is gone.

**Why:** a direct message has exactly one reader, so the server copy exists
only to reach them while they are offline. Deleting it on receipt keeps the
relay holding as little as possible, and the indicator lets both people see
that it did.

### How the relay knows

- For a `persist: true` cipher frame in a direct room
  (`ForkMeshRoom.webSocketMessage`), the relay stamps the outer envelope with
  `"ref":"<sha256(frame)[:40]>"`, the hash of the frame as the client sent it.
  The client's own `ref` is stripped first, the same way `recipients` and
  `historyReplay` are.
- It stores the frame before fanning it out (`_retain_direct`), so a receipt
  can never arrive before the row exists. The row's `chat_history.msg_id` is
  `<ref>:<sender account_bi[:16]>`, plus `:n` when the sender marked the frame
  `notify` (a chat message or thread reply, not an edit, reaction or key
  advertisement). The sender tag keeps `chat_history` unchanged (no
  migration) and is what stops a sender from clearing their own message
  before it is delivered.
- A participant's client sends `{"kind":"forkmesh-received","v":1,"refs":[...]}`
  after it has opened a frame, live or from the history replay. It sends at
  most 64 refs per frame, below D1's 100 bound parameters. A frame it cannot
  open is not acknowledged, so another of that person's devices can still
  fetch it. `chat_direct_received` deletes the rows in that room whose ref
  matches and whose sender tag is not the acknowledging account.
- After a store, after a receipt, and when a socket joins, the relay sends
  `{"kind":"forkmesh-server-copies","v":1,"stored":N}`. N counts the room's
  rows ending in `:n` within the 7-day window. Edits and reactions are
  deleted the same way but are not counted. Key advertisements are never
  acknowledged (see below), so they stay for the 7-day window.
- Only a `notify` frame that is not a `resend` raises the other person's
  unread count and sends the DM push (`_chat_direct_message_retained`). Before
  2026-10-08 every retained frame did, so a key advertisement, an edit or a
  reaction counted as an unread message and woke the phone.
- Channels are unchanged: they have many readers, so `ref` is not stamped and
  receipts are ignored.

### Undeliverable copies and re-sealing (2026-10-08)

**Report:** the sender saw "9 messages on the server" for hours while the
other person was online.

**Root cause:** each device seals a DM's content key only for the device keys
it has already learned in that room (`RoomSeal` peers, learned from
`e2ee-key` frames or from a sealed frame's sender). Nothing outside the room
publishes device keys. In a new DM, or after the other person's device key
changed (new browser, cleared storage, new phone), the frames are sealed to
the sender alone. The recipient's device cannot open them, so it never sends
a receipt, and the copies stayed until the 7-day expiry. Nothing ever sealed
them again. Three things made it likely:

- every client acknowledged `e2ee-key` frames, so the relay deleted the
  recipient's key advertisement as soon as one of the sender's devices read
  it. The sender's other devices, and the same device after its local key
  list was lost, could not learn the key again;
- the desktop ignored the `direct-messages` account event, so it never
  connected to a DM it did not already watch (only the first 8 had a socket);
- Android never constructed its DM service, and iOS connected only to the
  open DM and never reconnected (see [native/README.md](../native/README.md)).

**Fix:** every client follows these rules in a direct-message room:

1. Never acknowledge an `e2ee-key` frame. Key advertisements stay on the relay
   for the 7-day window, so a device that comes back later can learn them.
2. When a sealed frame is not wrapped for this device, send one persisted key
   advertisement (`persist: true`, ignoring the once-a-day throttle) for that
   connection. This is what tells the sender to re-seal.
3. Keep an outbox of up to 64 of this device's own durable sealed frames:
   each own send, plus each own frame in the history replay (still on the
   relay, so not delivered yet). An entry holds the relay ref, the plaintext
   and the key ids the content key was wrapped for. After
   `forkmesh-history-end`, and whenever a new peer key is learned, every entry
   not wrapped for all known keys is sealed again and sent as a new frame with
   the same message id, `persist`, the original `notify`, and an outer
   `"resend": true`. Then the client sends
   `{"kind":"forkmesh-superseded","v":1,"refs":[...]}` with the old refs.
   Entries older than 7 days are dropped. The outbox is cleared when the relay
   reports `stored: 0`.
4. Receivers already drop a message id they have shown, so a resend of a
   message that did arrive shows nothing new.

`forkmesh-superseded` (`chat_direct_superseded`) deletes only rows whose
sender tag is the sending account, the mirror image of a receipt, so a sender
can withdraw its own unreadable copies but nobody else's. A `resend` frame is
stored and counted like any message, but it does not raise the unread count or
push again.

The desktop also re-lists DMs on the `direct-messages` account event
(`OfficeChannelMirror::refreshDirectMessages`). It keeps a socket to every DM
with unread messages, besides the first 8, and to every DM the user opens
(`watchConversation`).

A message written before the recipient's device was ever in the room still
waits for the sender to come back. When the recipient's device joins, it
advertises its key. The sender's device re-seals as soon as it is in the room:
at once if it is connected, otherwise at its next connection. That is the
desktop for its 8 most recent DMs and any it watches, and the web when the DM
is open. iOS and Android follow the same rules. The existing stuck copies are
re-sealed the same way once both sides run this build. The Swift side was not
compiled here (no toolchain on the agent box); the Android core tests ran.

Tests: `app/tests/test_chat_recipient_count.py`
(`test_sender_can_supersede_only_its_own_stored_copies`,
`test_only_new_messages_raise_the_direct_unread_count`),
`app/tests/test_chat_direct_server_copies_frontend.py`
(`test_direct_messages_are_resealed_once_the_peer_key_arrives`) and
`desktop/tests/test_chat_transport.cpp`
(`testDirectRoomResealsUndeliveredMessages`).

### Limits

- "Received" means received by one of the recipient's devices. That person's
  other devices cannot replay a message the first device already
  acknowledged. Each client keeps its own local history.
- The same applies to the sender's other devices.
- Rows stored before this change have no sender tag. They are never counted
  and expire with the normal 7-day window.
- A client that does not send receipts (an older build) leaves copies on the
  server until that window expires. The indicator then keeps the count above
  zero, which is the truth.

### Clients

| Client | Receipt | Indicator |
| --- | --- | --- |
| Web `/chat` | `chat-room-transport.js` (`noteReceived`, batched for 150 ms; outbox and re-seal in `resealNow`, enabled by `isDirectRoom`) | `#chat-direct-server-copies` under the title (`chat.js`, `renderDirectServerCopies`) |
| Desktop | `ServerNode::noteReceived` / `flushReceived` (batched for 150 ms; outbox and re-seal in `ServerNode::resealOutbox`, enabled by `setDirectRoom`) | `serverCopiesLabel` beside the channel title (`updateChatConversationHeading`), via `OfficeChannelMirror::directServerCopiesKnown` |
| iOS | `DirectMessageService.noteReceived` (batched for 150 ms); outbox and re-seal in the same service; sockets to the open DM and every DM with unread messages, reconnected with backoff ([native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md)) | `ChatHub.serverCopiesLine` under the conversation name and in `headerDetail` |
| Android | `DirectMessageService` sends replay receipts at `forkmesh-history-end` and live ones at once, queued until the socket is set; outbox and re-seal in the same service. Built in `ChatDepth.kt` since 2026-10-08 (it was never constructed before, so Android received no DMs) ([native/ANDROID.md](../native/ANDROID.md)) | `ChatHub.serverCopiesLine` as the panel header subtitle. A DM opens from its notification; there is no DM list yet |

## Delivery ticks on direct messages (2026-10-09)

Asked: "on messages have the two dots like signal does for delivered, read
etc." Your own messages in a web `/chat` direct message now end with a
Signal-style mark in place of the old "to 1" label (a DM's count is always 1,
even while the other person is offline, so it said nothing):

| Mark | State | Set when |
| --- | --- | --- |
| Dashed ring | Sending | The message is on screen, but the relay has not acknowledged it yet |
| One ring with a tick | Sent | `forkmesh-recipients` arrived for it, or it reached this tab as a relayed frame (replayed backlog, or sent from your other device) |
| Two rings | Delivered | `forkmesh-delivered` named its ref |
| Two filled dots | Read | `forkmesh-read` arrived after it was sent |

### Relay frames

- `{"kind":"forkmesh-delivered","v":1,"refs":[...]}`. When a device sends
  `forkmesh-received`, `ForkMeshRoom._direct_received` forwards the cleaned
  refs (`chat_direct_refs`) to every live socket of the other account
  (`_send_to_other_accounts`). It sends nothing for `forkmesh-superseded`.
  The receiving client ignores refs that do not belong to its own messages.
- `{"kind":"forkmesh-read","v":1}` carries no refs. `_mark_read` in
  `chat_direct_messages_api.py` (`POST .../read`, and `room-access`, which
  marks the conversation read when it opens) calls `runtime.announce_read`
  only when the reader's `last_read_count` actually moved.
  `_announce_chat_direct_read` posts to the room's internal
  `/api/chat/direct-messages/<id>/v<kv>/read?account=<reader>` path
  (`CHAT_DIRECT_MESSAGE_DO_RE` now accepts `read`). The room sends the frame
  to the other account's sockets. Every reader client (web, desktop, iOS,
  Android) already calls `/read`, so all of them produce read ticks with no
  client change.
- A read frame marks every own message the tab already counts as sent. That
  is exact for ordering: the relay sends the sender's ack before any later
  frame on the same socket, and a message relayed after the read is acked
  after the read frame, so it stays at "Sent". The account-event channel was
  not used: the `direct-messages` topic sends a phone banner, and its 2-second
  coalescing drops a second event instead of delaying it.

### Web client

- `chat-room-transport.js` maps refs to message ids with `recipientRefs`.
  This covers its own sends, re-seal resends, and, in a direct room, every
  frame it opens. It turns the two frames into `onDelivered(ids, room)` and
  `onRead(room)`, and only for direct rooms.
- `chat.js`: `directReceiptState`, `directReceiptIcon`, `paintDirectReceipt`,
  `applyDirectDelivered` and `applyDirectRead` (renamed and widened on
  2026-10-13, see the next section). `buildRow` puts the mark
  where the recipient label was. The `.chat-receipt` styles are in `chat.css`.
- A hidden tab, or a frame for a conversation that is not the open one, no
  longer posts `/read`. The read is deferred (`directReadDeferred`) until the
  tab is visible again (`markActiveDirectMessageReadIfDeferred`). Before this
  change, a background tab marked messages read as they arrived, which would
  have shown the sender "Read" for messages nobody had seen.

### Limits

- The ticks are not persisted. The web keeps no local DM history: after a
  reload, the only own messages it shows are the ones the server still holds,
  which are undelivered, so "Sent" is still the truth for them.
- Ticks change only while the sender's tab has the conversation open. A
  delivery or read that happens with no socket open is not replayed.
- Desktop, iOS, Android and the Office chat still show their earlier labels.
  To add ticks there, handle the same two frames in `ServerNode`, the native
  `DirectMessageService` and `office/public/features/chat.js`. (Done on
  2026-10-13, see the next section.)

Tests: `app/tests/test_chat_recipient_count.py`
(`test_receiving_a_direct_message_tells_the_sender_it_was_delivered`,
`test_superseding_a_direct_message_is_not_a_delivery`,
`test_read_announcement_reaches_only_the_other_participant`),
`app/tests/test_chat_direct_messages_api.py`
(`test_reading_announces_to_the_sender_only_when_the_cursor_moves`) and
`app/tests/test_chat_direct_receipts_frontend.py`.

## Delivery ticks on every own message, all clients (2026-10-13)

Asked: "add these symbols for each chat message", with two references:
Signal's rings (sent: one ring with a tick; delivered: two overlapping rings
with ticks; read: two filled rings with the ticks cut out) and a tick row
(sending: dotted; sent: tick; delivered: double tick; read: blue double
tick). One design combines both, and every client draws the same shape.

| State | Mark | Set when |
| --- | --- | --- |
| Sending | One ring drawn dotted, no tick, muted at 70% opacity | The message is on screen and the relay has not acknowledged it |
| Sent | One outlined ring with a tick inside | `forkmesh-recipients` arrived for its ref, or it arrived as a relayed or replayed frame (your other device, or history) |
| Delivered | Two overlapping outlined rings, each with a tick; the back ring sits left, partly behind the front one | `forkmesh-delivered` named its ref (direct messages only) |
| Read | The same two rings filled with the accent blue, ticks cut out in the background colour | `forkmesh-read` arrived after it was sent (direct messages only) |

Accessible name and tooltip: "Sending", "Sent", "Delivered", "Read". A client
that already had a failure state keeps it.

### Geometry

A 22 x 14 box, stroke width 1.4, round caps and joins, drawn about 17 x 11 px
next to the time:

- Front ring: centre (14, 7), radius 5.2.
- Back ring: centre (8.5, 7), radius 5.2, drawn only outside the front ring.
  On the web that is the arc `M11.25 2.59A5.2 5.2 0 1 0 11.25 11.41`, from one
  crossing of the two circles to the other.
- Front tick: (11.7, 7.2) to (13.4, 8.9) to (16.4, 5.6).
- Back tick: (6.2, 7.2) to (7.9, 8.9) to (9.3, 7.4). The last stretch runs
  under the front ring, so the web stops it where it meets the front circle
  (8.87, 7.86).
- Read: the front disc is filled (fill and stroke, so its edge matches the
  outlined ring). The back disc is filled only outside the front disc, with a
  0.9 gap in the background colour between them, so they read as two
  (`M10.21 1.35A5.9 5.9 0 1 0 10.21 12.65A6.8 6.8 0 0 1 10.21 1.35Z`). Both
  ticks are stroked in the background colour; the back tick is cut where the
  gap starts (7.33, 8.33).
- Colours: sending, sent and delivered use the client's muted text colour;
  read uses the accent blue (web `--accent`: `#58a6ff` dark, `#0969da`
  light; Office `--brand`; desktop and phones their own accent blue).

### Scope

Every own message shows the mark: chat rows and thread replies, in direct
messages, private rooms and public or Office channels. Direct messages go
through all four states. Channels reach only sending and sent: the relay
sends `forkmesh-delivered` and `forkmesh-read` only in direct rooms, because
honest per-member delivery in a room with many readers would need server work
that is not in this change. Channels keep their "to N" label next to the
mark; direct messages show the mark instead of "to 1", as before.

### Clients

| Client | Where |
| --- | --- |
| Web `/chat` (and the World office embed) | `chat.js`: `receiptState`, `receiptIcon`, `paintReceipt`, `makeReceipt`, `updateReceipt`, `applyReceiptSent`, `applyDirectDelivered`, `applyDirectRead`. `buildRow` adds the mark to every own row (after "to N" in channels); `threadMessageHeader` adds it to own thread replies and the thread root. Records that came from the relay carry `relayed` (set in `renderChatEntry` / `renderThreadEntry`), which counts as sent. World #general has no relay ack, so a send that the socket accepted marks it sent (`send`). Thread replies keep `receipt` in the thread store (`portableMessageRecord`). Styles: `.chat-receipt` in `chat.css` |
| Office | `office/public/features/chat/core.js` (`receiptState`, `receiptShapes`, `RECEIPT_LABELS`) and `features/chat.js` (`receiptNode`, `onRecipients`, `advance` for `onDelivered` / `onRead`, `isDirectRoom` passed to the transport). A sent message is shown at once as "Sending" and removed again, with its text put back in the composer, when the transport refuses it. Relay recipient counts are kept on the message (`recipients`); Office never showed "to N" and still does not, to stay inside its bundle budget. Styles: `.rc` in `features/chat.css` |
| Desktop | `desktop/**` ([docs/desktop](../desktop/README.md)) |
| iOS | `native/ios/**` ([native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md)) |
| Android | `native/android/**` ([native/ANDROID.md](../native/ANDROID.md)) |

Tests: `app/tests/test_chat_direct_receipts_frontend.py` (states, shapes,
channels stop at sent, thread replies, supersede),
`app/tests/test_chat_recipient_count_frontend.py` (count beside the mark),
`office/tests/chat.test.mjs`.

## Delete for everyone, replies and reactions in direct messages (2026-10-13)

Asked: "make it so that you can delete for everyone something you sent" and
"also have it so you can reply and react to chats from others in p2p" (p2p
meaning direct messages between two people).

### Frames

Direct messages use the frames relay rooms and web channels already use, sealed
like any other DM frame (the same RoomSeal path as `chat`):

| Frame | Shape | Relay flags |
| --- | --- | --- |
| Reply | `{type:"thread-reply", id, senderId, sender, ts, rootId, channel:"direct:<id>", text, richText?}` | persist and notify, like `chat` |
| Reaction | `{type:"reaction", id, senderId, sender, ts, conversation:"direct:<id>", target, emoji, reactorId, reactorName, added}` | persist |
| Edit | `{type:"edit", id, senderId, sender, ts, conversation:"direct:<id>", target, text, richText?, editedAt}` | persist |
| Delete | `{type:"delete", id, senderId, sender, ts, conversation:"direct:<id>", target}` | persist |

### Rules

- Reply and react work on any message in a direct message, yours or the other
  person's. Edit and "Delete for everyone" only on your own. "Delete for me"
  stays where a client already has it.
- A receiver applies an edit or delete only when the frame's `senderId` equals
  the target message's `senderId`.
- A delete removes the message and its reactions from the transcript and from
  any saved local history, so it does not come back after a restart or a
  history replay. Mutations replayed ahead of their target are applied after
  the replay (web `handleHistory` registers every `chat` and `thread-reply`
  first).

### Withdrawing the server copy

Like Signal, the sender also withdraws the copy the relay still holds. After
sending `delete`, the client sends
`{"kind":"forkmesh-superseded","v":1,"refs":["<ref of the original frame>"]}`
when it knows that ref. `chat_direct_superseded` then deletes the stored
ciphertext, but only rows the same account sent and only if the recipient has
not received them yet (received rows are already gone). The delete frame
itself stays on the relay until the other person's device receives it, so an
offline recipient still gets it.

The ref is known for your own sends and for your own frames replayed to this
device. Web `chat-room-transport.js` already keeps `recipientRefs` (ref to
message id, 256 entries) for its sends, re-seal resends and every frame it
opens in a direct room. `refsFor(ids)` looks the refs up by message id, also
checking the re-seal outbox, and `supersede(ids)` sends the frame (in batches
of 64 refs, direct rooms only) and drops those messages from the outbox so a
later re-seal cannot resend a deleted message. `confirmMessageDelete` in
`chat.js` and `sendMutation` in Office call it once the delete frame is sent.

### Web and Office today

Web `/chat` direct messages already had reply in thread, reactions, edit and
delete on the right messages (`buildRow`, `buildThreadReply`), the author
check in `handlePlain`, and replay ordering in `handleHistory`; the only
change is the server-copy withdrawal. The web keeps no local message history,
so there is nothing else to delete. Office has edit and delete only (no
threads or reactions); it now withdraws the server copy of a deleted direct
message too.

Desktop, iOS and Android: see their own pages
([docs/desktop](../desktop/README.md), [native/ios/docs/FMChat.md](../../native/ios/docs/FMChat.md),
[native/ANDROID.md](../native/ANDROID.md)). On iOS a received delete also
removes the delivered notification for that message
([chat-push-previews.md](chat-push-previews.md)).

Tests: `app/tests/test_chat_direct_receipts_frontend.py`
(`test_delete_for_everyone_withdraws_the_server_copy`,
`test_supersede_is_a_no_op_outside_direct_messages`),
`app/tests/test_chat_recipient_count.py`
(`test_sender_can_supersede_only_its_own_stored_copies`),
`office/tests/chat.test.mjs`.

## Last line and pictures in the conversation list (2026-10-13)

Asked: "Make sure threads still work too also in the chat list show the last
line per channel and per message with users also show their user icon in the
chat list".

### What a row shows

- A second, muted, single line under the name with the newest top-level
  message this device has decrypted for that conversation: the text in a
  direct message, "You: text" for your own (also when sent from another of
  your devices), "Name: text" in channels. A file with no text shows
  "📎 name". Markdown is reduced to plain text (`plainTextFromRichSource`)
  and newlines collapse to spaces.
- Thread replies, reactions and key frames never count. A delete takes the
  message out, so the line falls back to the one before; an edit of the
  newest message replaces the line.
- A short time on the right of the name: the clock time today, the weekday
  within six days, otherwise day and month.
- A direct-message row shows the other person's picture (30 px on web, 28 px
  in Office) with the presence dot over its lower right corner.

### Plaintext stays on the device

The line is built from messages the client already holds in memory. No server
API gained plaintext and the web stores nothing new. Web `/chat` and Office
keep one room socket open at a time, so a conversation not opened in this
session has no line yet: its row keeps the empty second line, so every row has
the same height.

### Web `/chat`

`renderRooms` and `renderDirectMessages` build each row with
`roomTextColumn` (name line, time, preview) and tag it with `data-channel`.
`conversationPreviewRecord` walks `channelMessages` from the end for the
newest record with text or a file that is not a deleted thread root;
`conversationPreviewText` formats it. Any change to a conversation
(`insertMessage`, `removeMessage`, an edit in `handlePlain` or
`saveMessageEdit`, `pruneExpiredMessages`) calls `queueRoomPreview`, which
collects channels and repaints only their preview nodes once after 50 ms
(`flushRoomPreviews`), so a replayed backlog costs one repaint, not one per
frame. The direct-message list is now sorted by recency (the server's
`updatedAt`, or the newest message seen here, whichever is later; the server
already pages it that way), and the flush rebuilds that list only when the
order actually changed. Under 680 px the rooms become a chip strip and the
preview and time are hidden there. The picture is `makeAvatar`, the same
loader and cache as the transcript.

### Office

`renderList` in `office/public/features/chat.js` shows the same line
(`lastLine` and `listTime` in `features/chat/core.js`) and repaints when a
line arrives, a backlog replays or you send (the list is a handful of rows).
A direct row asks `/api/accounts/<peer>/avatar?size=128`; a 404 falls back to
the letter avatar and is remembered (`S.noPic`) so the list does not ask
again. The presence dot shows when the peer is in the office (matched by the
account handle or name). The DM list sorts by the later of `updatedAt` and
the newest message seen here.

### Threads after the 2026-10-13 changes

Web `/chat` threads were checked in channels and direct messages: a reply is
sent with `channel: channelWireLabel(...)` (`direct:<id>` in a DM), the
receiver's `frameMatchesScope` accepts it only in that room, and
`renderThreadEntry` adds it once to the thread store and repaints the summary
and the open thread. Office has no threads.

Round 1 (`de27d1cc03`) dropped Office's `mutate` while adding receipts but
left its three callers, so the first non-chat frame in a room (presence,
edit or delete) threw and edits and deletes from others never applied. It is
back, with the author check; `office/tests/chat.test.mjs` now checks it is
defined.

Tests: `app/tests/test_chat_list_previews_frontend.py`,
`office/tests/chat.test.mjs`.

## Direct messages over the local mesh (2026-10-09)

Asked: "make direct messages work over the mesh as well." A direct message now
also travels over the local mesh (`#mesh`: LAN and Bluetooth, see
[../desktop/networking.md](../desktop/networking.md#local-mesh) and
[../native/MESH_PUSH.md](../native/MESH_PUSH.md)) to the other person's
devices that are linked there. With the internet or the relay down, two people
in the same place keep messaging each other. The relay copy is still sent, so
the conversation stays the same on every device and on the web.

**Who it goes to.** Every client has one X25519 device key for all its rooms
(desktop `chat/e2ee/x25519PrivateV2`, the same key name on iOS and Android).
The device keys a direct-message room has learned (`RoomSeal` peers, from
`e2ee-key` frames or sealed frames in that relay room) are exactly the devices
of its two participants, because the relay admits only them. The mesh room
learns the same key ids from each link's key frame. A direct message is sent
only over a link whose key id is one of the room's keys, and it is sealed to
that one key (`RoomSeal::sealObjectFor`, Kotlin `sealFor`, Swift
`seal(_:for:)`). Other devices on the mesh never receive it, and a device in
between cannot open or relay it: only directly linked devices are reached.

**Wire format.** Inside the mesh room's two layers, the plain frame is the
relay frame with `type` set to `direct` and `conversation` to `#mesh`:
`{type:"direct", id, senderId, sender, channel:"direct:<conversation id>",
text, ts, ...}`. Older clients drop it unread: it is sealed to someone else,
and their room code only shows `chat`. The mesh does not store it in `#mesh`
history, relay it to other links, or queue it for devices that link later.

**Who it is accepted from.** The receiving device reads the sender's key id
from the sealed frame (the X25519 wrap proves the sender holds that key) and
shows the message only when that key is one of the direct room's keys. A
device that is merely on the same network cannot post into a conversation.
The type is set back to `chat` and the frame goes through the same parser as
a relay frame (`chatMessageFromPlain`, `DirectMessageFrames.chatMessage`); the
mesh copy carries no file. Message ids are shared with the relay copy, so
whichever copy arrives second is dropped as already seen. The recipient still
acknowledges the relay copy when it arrives, which deletes it from the server.

**Relay failures.**

- Desktop (`OfficeChannelMirror`): `sendMessage` hands the text to
  `LocalMeshNode::sendDirect` with the same message id. A message the mesh
  delivered is not reported as failed when room access or the socket fails;
  it stays queued and goes to the relay when the room reconnects. When the
  queue of 32 is full, the oldest mesh-delivered entry makes room.
- Phones (`DirectMessageService.send`): the mesh copy goes first. When the
  relay socket cannot be opened or the send throws, the message is kept and
  shown if the mesh reached a device, and the relay copy is not retried.
- A message is reported as failed, as before, when neither path reached
  anyone.

**Offline start (desktop).** The desktop keeps its direct-message list and
each room's device keys in the setting `chat/direct/meshDirectory` (account,
API base, and per conversation the id, the other user and the keys). The
Office mirror restores it before its first listing, and the window now also
starts the mirror when offline mode turns on, or when the mesh starts in
offline mode. Before, the mirror only started after a relay heartbeat, so a
desktop started without internet had no DMs at all. The phones learn the
room's keys and name when they connect the room. A phone that has not
connected a DM since the app started cannot send that DM over the mesh.

**Limits.** No group DMs (there are none). No files, edits, reactions or
typing over the mesh. Nothing is queued on the mesh: a message written while
the other device is not linked waits for the relay. The other person's device
key must already be in the room; a brand-new DM whose recipient never joined
the relay room has no key to seal to.

**Code.** Desktop: `LocalMeshNode::sendDirect`, `handleDirect`,
`directMessageArrived`, `Link::keyId`; `OfficeChannelMirror::acceptMeshDirect`,
`directDeviceKeys`, `setMeshDirectSender`, `saveDirectDirectory`,
`restoreDirectDirectory`; `ServerNode::roomSealPeerKeyIds` and
`roomSealPeersChanged`; wired in `MainWindow::startOfficeChannelMirror` and
`startLocalMesh`. Android: `LocalMeshRoom.sendDirect` / `onDirect`,
`DirectMessageService.meshSender` / `acceptMeshDirect` / `deviceKeys`, wired in
`ChatDepth` and `ChatFeature.buildMesh`. iOS: `LocalMeshService.sendDirect` /
`onDirect`, `DirectMessageService.meshSender` / `acceptMeshDirect` /
`deviceKeys`, wired in `AppServices+Chat.swift`.

Tests: `testDirectMessagesOverLocalMesh` in
`desktop/tests/test_chat_transport.cpp`, `MeshDirectMessagesTest.kt`
(Android core) and `MeshDirectMessagesTests.swift` (iOS core, Linux
toolchain).

## Images and files in direct messages (2026-10-10)

Asked: "have it so that images can be pasted and uploaded on direct messages
as well." The web `/chat` page already did this: a DM uses the same composer,
paste handler, paperclip and drop target as a channel, and sends
`{type:"chat", channel:"direct:<id>", fileName, fileMime, file}` sealed like
any other DM frame. The relay never reads the frame, so nothing changed on the
server. The other clients refused or dropped these frames:

- Desktop: `attachFile` and `trySendClipboardImage` returned early for every
  Office conversation, so the paperclip did nothing and a pasted image fell
  through to a text paste.
- iOS: `canSendMedia` was false for DMs, which hid Photos, Camera, File, the
  microphone and image paste.
- Android: the composer offered the paperclip and paste, but
  `ChatHub.sendAttachment` refused DMs without saying so.
- On both phones, `DirectMessageFrames.chatMessage` dropped a frame with no
  text, so an image sent from the web never showed.

What changed:

- **Desktop.** `OfficeChannelMirror::sendFile` queues a file beside the
  texts (`PendingText` has file fields now) and `flushPending` sends it with
  `ServerNode::sendFileWithId`, so the image shows at once under the same id
  the relay copy carries. Only DM rooms take files (`canSendFile`): Office
  channels stay text-only on the desktop, as before. A pasted image is sent
  as PNG when it fits, otherwise as JPEG (quality 85, then 70), scaled down by
  a quarter at a time until it fits.
- **iOS.** `DirectMessageService.sendFile` seals the file frame the same way
  as text (`sendSealed`). `canSendMedia` is true for a DM the service knows.
  `sendMedia` (photos, camera, voice, paste) and the File picker
  (`sendDirectFile`) go through it. `ChatHub.sendAttachment` refuses DMs, so a
  file is never sent to the relay's channel path by mistake.
- **Android.** `DirectMessageService.sendFile` matches iOS. `ChatHub.sendAttachment`
  routes a DM to it, and `rows` attaches the stored file to DM rows.
- **Receiving (phones).** `chatMessage` accepts a frame that has a `fileName`
  and no text. A new `onFrame` hook on the DM service hands each sealed chat
  frame, received or sent, to `ChatExtras` (iOS `applyRoomFrame`, Android
  `apply`), which keeps the bytes for the row, as Office rooms already did.

**Limit.** 1 MiB per file on every client
(`forkmesh::office::kMaxDirectFileBytes`, `DirectMessageFrames.maxFileBytes`,
`MAX_FILE_BYTES`), the same as the web's `MAX_ATTACHMENT_BYTES`. The relay's
retained-body cap (`CHAT_HISTORY_MAX_BODY`, 1.9 MB) is sized for that after
base64 and encryption. Larger files get a message saying so. Files do not go
over the local mesh: the mesh copy is text only, as described above.

Tests: `testOfficePrivateRoomsAndDirectMessages` in
`desktop/tests/test_chat_transport.cpp`,
`testImagesTravelSealedInDirectMessages` in `DirectMessagesTests.swift` and
`testHubSendsAndShowsImagesInDirectMessages` in `DirectMessagesTest.kt`.

## Touch, movement and drawing in direct messages (2026-10-13)

Asked: "in the direct chat have it so that you can tap to a user and they
feel the haptics - also have it so a user can share their accelerometer and
then you can watch the other user's movement as you are chatting - also let
the background be shared and also let users draw to each other on the backs
of their screens in addition to the tapping."

All four travel inside the DM room as sealed frames, so the relay still sees
only ciphertext. Built on iOS; the web, desktop and Android ignore the new
frame type (they show only `type: "chat"` rows).

### Live frames

A tap, a drawing stroke and movement samples are `type: "live"` frames
(`DirectLiveFrames`, `DirectLiveEngine` in
`native/ios/ForkMeshCore/Sources/FMChat/Direct/DirectLive.swift`). One frame
carries everything queued since the last one:

- `taps`: `[[x, y], ...]`, at most 12.
- `ink`: up to 8 stroke chunks `{s, p, c?, w?, e?}`. `s` is the stroke id,
  `p` a flat `[x, y, x, y, ...]` list (160 points per frame in all), `c`
  (colour, `0xRRGGBB`) and `w` (width x10) go with a stroke's first chunk,
  `e: true` ends it. A stroke keeps at most 1,500 points.
- `motion`: up to 40 samples `[dt, gx, gy, gz, ax, ay, az]`: `dt` is
  milliseconds from the frame's `ts`, `g` the gravity vector and `a` the user
  acceleration, both in g x100. `motionOff: true` says sharing stopped.
- `clear: true` wipes the drawing on both phones.

Positions are scaled by the screen width so a circle stays round on another
phone: `x` is 0..1 across, `y` is distance from the vertical middle in screen
widths (clamped to +-2.5), both x1000 as integers.

The sender's envelope has `"live": true` and no `persist`, `notify` or
`resend`, so nothing is stored, counted as unread, pushed or acknowledged. A
live frame read from the history replay is ignored; frames from the reader's
own account (another of their devices) are ignored too.

### Relay budget

Movement and drawing need several frames a second, but a chat socket may send
only 20 frames per 10 seconds (`ROOM_MSG_MAX_PER_WINDOW`); live frames in
that bucket would crowd out messages. In `ForkMeshRoom.webSocketMessage`
(`app/src/entry.py`), a DM socket's cipher frame with `live: true` and none
of `persist`/`notify`/`resend`, at most 16 KiB (`chat_live_envelope`), goes
to `_relay_live` instead:

- its own bucket, `ROOM_LIVE_MAX_PER_WINDOW` (150) per 10 s
  (`lv_start`/`lv_count` in the socket attachment);
- room access (`_private_room_current`, three queries) is re-checked at most
  every `ROOM_LIVE_ACCESS_RECHECK_MS` (5 s, `lv_checked`) instead of on every
  frame;
- it is fanned out to the other sockets as is, with no `ref`, recipient count,
  retention or push.

Anything else, including `live` frames in channels, takes the normal path.
`_write_attachment` now writes the attachment for both buckets so neither
loses the other's counters.

On join a DM socket is told the budget:
`{"kind":"forkmesh-live","v":1,"perWindow":150,"windowMs":10000}`. The phone
then sends at most one live frame every `windowMs * 4 / (perWindow * 3)` ms
(88 ms, at least 80). Until it hears that (an older relay), it sends one every
2.5 s, which stays inside the old shared bucket.

### Shared background

A DM background is now a sealed `look` frame like a room's (`field`,
`preset` or `image`), sent with `persist: true` and no `notify`
(`DirectMessageService.sendLook`). It is kept on the relay only until the
other phone receives it, like any DM frame, and is applied on both sides by
`ChatExtras.applyRoomFrame`. This replaces the device-only background of
2026-10-13 (`setLocalLook` is gone). People pictures stay on the phone.

### iPhone

`MessagesLiveTouch.swift`: the hand button beside + in the composer opens a
touch canvas over the conversation. A tap sends a buzz (a ripple on both
screens, a heavy-then-rigid knock on the other phone); a drag draws a glowing
stroke in one of eight colours behind the messages on both screens. Strokes
fade after 45 s. The toolbar shares movement (Core Motion device motion at
20 Hz), clears the drawing and closes. While the other person shares, a card
in the top corner shows a phone tilting, turning and shaking as theirs does,
played back from the buffered samples 120 ms behind. Sharing stops when the
conversation closes, the app leaves the foreground or the account signs out.

Tests: `app/tests/test_chat_recipient_count.py` (`test_live_*`) and
`DirectLiveTests.swift`.

## View-once video and voice messages (2026-10-13)

Asked: "make it so that you can record a video or voice message and send that
to someone; have those get deleted after they are watched or listened to".

Direct messages only. A DM has exactly one recipient, so "opened" has one
meaning; a private room would need a per-member state and a rule for when the
last copy goes. Groups are private rooms, so they do not get these messages.

### Wire format

The media never travels in the room. The sender encrypts the recording on the
device with a fresh AES-256-GCM key (32 random bytes, 12-byte random IV,
16-byte tag appended, as WebCrypto, CryptoKit's `ciphertext + tag` and
`AES/GCM/NoPadding` produce it), uploads the ciphertext, then sends an ordinary
sealed DM chat frame (`persist`, `notify`) that carries the key:

```
{type:"chat", channel:"direct:<conversation>", id, senderId, sender, ts,
 text:"🎥 Video message" | "🎤 Voice message",
 viewOnce:{v:1, kind:"video"|"voice", media:"<32 hex>", key:"<base64url>",
           iv:"<base64url>", mime:"video/webm"|"video/mp4"|"audio/webm"|"audio/mp4"|...,
           durationMs, size:<ciphertext bytes>, parts}}
```

`text` is only for clients that do not know `viewOnce` (the desktop today):
they show the line and nothing else. A client that knows `viewOnce` ignores
`text`, never shows a download, save, copy or forward action on the row, and
keeps no copy of the media. The relay sees ciphertext twice over: the frame is
sealed like every DM frame, and the stored media is encrypted with a key that
only exists inside that frame.

### Server API

`app/src/chat_direct_media_api.py`, under
`/api/chat/direct-messages/<conversation>/media`. Every route needs a DM
participant; anybody else gets 404, as for the rest of the DM API.

| Route | Who | Does |
| --- | --- | --- |
| `POST /media` `{kind, size, parts, durationMs}` | either participant (the sender) | Creates an `uploading` row for the other participant. `parts = ceil(size / 524288)` |
| `PUT /media/<id>/parts/<n>` `{data: base64}` | sender | Stores part `n`; every part but the last is exactly 512 KiB |
| `POST /media/<id>/finish` | sender | Checks every part is there and the sizes add up, then `ready` |
| `GET /media?ids=a,b` / `GET /media/<id>` | both | State: `uploading`, `ready`, `opened`, `expired`, `withdrawn`, with `sentAt`, `openedAt`, `expiresAt`, `role` |
| `GET /media/<id>/parts/<n>` | recipient only (sender: 403) | `{index, parts, data}` while `ready`; `410 {error:"media_gone", state}` after |
| `POST /media/<id>/consume` | recipient only | Deletes every part, then `opened` with `openedAt`. Repeating it answers `opened` again |
| `DELETE /media/<id>` | sender only | Withdraws an unopened message: parts deleted, `withdrawn` |

Limits: video 12 MiB and 61 s, voice 4 MiB and 121 s (the clients stop at 60 s
and 120 s), plus 1 KiB for the tag. Quota per sender: 20 unopened messages and
64 MiB of them at once (`429 media_quota_count` / `media_quota_bytes`). The
server cannot check a content type (it only ever sees ciphertext), so `kind`
picks the size limit and the clients only play the MIME types they know.

### Storage and deletion

Two tables (migration `0258_chat_direct_media`): `chat_direct_media` (one row
per message: sender and recipient blind indexes, state, sizes, times, a sealed
`{kind, durationMs}`) and `chat_direct_media_parts` (the ciphertext in 512 KiB
rows, each sealed again with `DATA_KEY` like `org_agent_attachments`). They are
ordinary rqlite tables, so every `DELETE` is a Raft write applied on all three
cores; there is no other blob store and no thumbnail. What the cores still hold
after a delete (SQLite free pages, rqlite snapshots and the encrypted backups
until they roll over) is ciphertext whose key was only ever in the sealed DM
frame.

- **Opened.** The recipient's client downloads all parts, decrypts them in
  memory and calls `consume` when playback starts. From then on the server
  holds no media. The client plays its in-memory copy once: when playback
  ends or the viewer is closed, whichever comes first, the copy is dropped
  (and any temporary file deleted) and the row says "Opened". Closing the
  viewer after it started counts as opened; closing it before playback
  started does not.
- **Sender.** Never downloads (the routes refuse) and keeps no local copy.
  The row shows "Sent", then "Opened" when the room relays
  `{"kind":"forkmesh-media","v":1,"id":"<media>","state":"opened"}` (sent by
  `consume` through the room's internal `/media` path, to the other account's
  sockets only), or when the client reads `GET /media?ids=` once on opening
  the conversation.
- **Unopened.** Expires 7 days after it was sent. The hourly `chat-prune`
  cron phase, and every new upload, run `sweep`: expired and abandoned
  uploads (an hour old and not finished) lose their parts and become
  `expired`, the room is told (`state: "expired"` to both participants), any
  part without a live message row is deleted, and finished rows are removed
  30 days after they were sent.
- **Deleted conversation or account.** `_delete_chat_direct_conversations`
  deletes the conversation's media and parts with it.

Screen capture cannot be prevented by any of the apps. Where the platform
reports it (iOS `UIScreen.capturedDidChange` / screenshot notification,
Android `FLAG_SECURE` on the viewer), the viewer says so or blanks itself;
the web cannot detect it at all.

### Web `/chat`

- `chat-view-once.js`: frame validation (`viewOnceFromEntry`), AES-GCM,
  the upload/download/consume/status client, the bubble and the one-time
  viewer (no download link, `controlsList="nodownload"`, no context menu,
  the object URL revoked and the bytes zeroed on close).
- `chat-view-once-recorder.js`: capture and `MediaRecorder` (MP4 with
  H.264/AAC where the browser can record it, because the iPhone app cannot
  play WebM; WebM/Opus otherwise, as in Firefox, which the iPhone then
  reports as unplayable and leaves unopened; video 640x480 at about
  0.9 Mbit/s, voice 32 kbit/s), a live preview or level meter, a 60 s / 120 s timer that
  stops by itself, and review (play back, Discard, Re-record, Send). Voice
  messages start with a tap (tap Stop to finish) or a press: holding the
  button for longer than 0.45 s and letting go sends at once.
- `chat.js`: the two composer buttons (DMs only, `updateAdminChannelControls`),
  `sendViewOnce`, `openViewOnceRecord`, `applyViewOnceState` and one
  `GET media?ids=` per batch of rows put on screen (`queueViewOnceStatus`).
  Deleting your own unopened message also withdraws the media.
- **Held receipt.** The web keeps no local DM history, so it would lose an
  unopened message on reload once the relay had deleted the frame. The
  transport's `holdReceipt` option keeps the `forkmesh-received` for a
  view-once frame from the other person until it is opened, has expired or
  was withdrawn (`releaseReceipt`). Until then the relay keeps the frame (the
  "1 message on the server" line says so) for its usual 7 days, the same as
  the media.

**Filters.** The recorder builds the stream it records from a
`{video track, audio track}` pair and asks the registered stages for each
(`registerViewOnceFilterStages({face, voice})` in the recorder module, each
`(input, choice) => {track, stop}`), so a filtered track is recorded exactly
like a raw one. `chat.js` registers the call engine's `createVoiceStage`
(`chat-call-voice.js`) when the browser has AudioWorklet: the recorder shows a
Voice picker (default "Your own voice", remembered in
`forkmesh.chat.viewOnceVoice`) and "Hear yourself (headphones)", which turns
on the stage's monitor. If a chosen voice cannot start, nothing is recorded:
the recorder says so instead of sending the real voice. The face and
background stage is not connected yet (its branch has not landed); `face`
is the seam it plugs into.

Tests: `app/tests/test_chat_direct_media_api.py` (upload, download, consume,
expiry, sweep, withdraw, quotas, limits, who may do what),
`app/tests/test_chat_recipient_count.py` (`test_media_*`, the room frame),
`app/tests/test_chat_view_once_frontend.py` (round trip through a fake
server, wrong key, held receipts, `forkmesh-media`) and
`app/browser_tests/tests/chat-view-once.spec.js` (Chromium with a fake camera
and microphone: record video and voice, send, open once, deleted on the
server, viewer gone, key dropped; hold-to-send with a changed voice; and two
`/chat` pages talking through a mocked relay: the bubble arrives sealed, plays
once, the sender sees "Opened", and the receipt is sent only after opening).
The spec runs against a static server of `app/public` that also answers
`/world/` (the config's readiness URL).

## Recipient counts ("to N")

Every message in a server room shows a small muted "to N" beside it, with the
tooltip and accessible name "Delivered to N people" ("Delivered to 1 person").
N is the number of distinct accounts the message went to when it was sent,
not counting the sender. It is never estimated: where a surface cannot know N
for a message, it shows nothing.

Hovering also says who it went to (task 1418): "Delivered to 2 people:
@alice, @bob". The relay lists at most 20 usernames
(`CHAT_RECIPIENT_NAMES_MAX`); past that the tooltip ends "and N more".
Messages stamped before names existed keep the bare count.

### Source of N per path

The relay (`ForkMeshRoom.webSocketMessage` in `app/src/entry.py`,
`chat_room_audience` / `chat_room_recipients`) is authoritative for every server room. It
computes N once per retained (`persist: true`) frame, before fan-out:

| Room | N |
| --- | --- |
| Direct message | `chat_direct_participants` rows minus the sender (always 1) |
| Private channel | creator plus `chat_channel_members` rows, minus the sender, online or not (they read it from history) |
| Notices channel | its creator, minus the sender |
| Public channel | distinct `account_bi` of the live sockets the frame is fanned out to, minus the sender's account (all of the sender's own devices are excluded) |
| Channel webhook | the same rules with no sender (`_channel_webhook`) |
| Public World #general, repository rooms, legacy `/api/room/*` | nothing: these sockets are anonymous, so the relay cannot tell accounts from devices or guests |

The desktop's per-device seal list (`RoomSeal::sealObject`) is not used as a
source. Its keys carry no account binding (the `e2ee-key` frame is only
`kid` and `pub`), it keeps keys of people removed from a room, and the relay
gates delivery by membership anyway. Counting keys would therefore report
devices or former members, which is exactly the guess the rule forbids.

### Wire format

- The relay strips any client-supplied outer `recipients` from every cipher
  frame (World #general included) and stamps its own integer on the outer
  envelope: `{"kind":"cipher",...,"persist":true,"recipients":3}`. It is never
  inside the ciphertext, so clients read it only from the outer envelope and
  drop any same-named field from decrypted plaintext.
- The stamped frame is what peers receive live, what `chat_history` stores
  (so socket replay carries it) and what `GET /api/chat/channels/<id>/history`
  returns in `messages[].body`; that endpoint also lifts it to
  `messages[].recipients`.
- The sender's socket is skipped by fan-out, so the relay sends it
  `{"kind":"forkmesh-recipients","v":1,"ref":"<sha256(frame)[:40]>","recipients":N}`.
  `ref` is the SHA-256 of the exact text frame the client sent (before the
  stamp), which is also the frame's `chat_history.msg_id`. Clients keep a
  bounded map from ref to their own message id. After a reload the sender's
  own messages carry N from history like anyone else's.
- Next to the count the relay stamps `"recipientNames":["alice","bob"]`:
  the usernames of the counted accounts, sorted, at most 20 (the first 20
  account blind indexes in sort order are resolved, so for a big public room
  the list is a sample). `chat_recipient_names` reads each name from the
  `users`/`nodes` record (`_account_identity_rec_by_bi`) and keeps it in a
  per-isolate cache (`_CHAT_RECIPIENT_NAMES`, up to 10,000 entries) so a busy
  room does not decrypt a record per message. An account with no record is
  counted but not named. The field is left out when no name resolves, and the
  relay strips a client-supplied `recipientNames` like `recipients`. The
  sender ack carries the same field, and the history endpoint lifts it to
  `messages[].recipientNames`.
- The names are stored with the message (in the `chat_history` row, outside
  the ciphertext). In DMs and private channels they are people every member
  already sees; in a public channel they show which accounts were connected
  when the message was sent.
- Frames retained before this change, and live-only frames (typing, presence),
  carry no count and show nothing.

### Surfaces

- **Web chat** (`/chat`, and the World office embed, which runs the same
  `chat.js`): `chat-room-transport.js` attaches `relayRecipients` from the
  outer envelope and maps the sender ack back to the message id; `chat.js`
  renders `span.chat-recipients` on every row (right-hand side, timestamp
  styling from `chat.css`) and in thread headers. In World #general the label
  stays hidden. The transport also attaches `relayRecipientNames` and passes
  names with the ack; `recipientCountTitle` builds the named tooltip.
- **Dashboard chat** (`dashboard-chat.js`) connects only to World #general and
  repository rooms, which are anonymous, so it has no honest N and
  shows nothing. Counting there would first need account-bound tickets for
  repository-room sockets and a rule for guests.
- **Desktop**: `ChatRecipients.h` (`forkmesh::recipients`) validates the
  outer value (a whole number 0..1000000) and builds the label and tooltip.
  `ServerNode::processFrame` passes the outer count to chat, thread-reply and
  DM messages (`ChatMessage::recipientCount`, -1 when unknown); peer `history`
  entries get none. `sendEncrypted` remembers the SHA-256 ref of each chat
  frame it sends (last 256) and turns the ack into
  `messageRecipientsKnown`; `OfficeChannelMirror` also reads it from HTTP
  history bodies and forwards it with the conversation.
  `MainWindow::applyMessageRecipients` updates the stored message, the visible
  `MessageRow` label (11px, the timestamp grey, tooltip and accessible name
  "Delivered to N people: @a, @b") and the chat cache, which saves
  `recipients` and `recipientNames` so both survive a restart.
  `namesFromValue` accepts only distinct lowercase handles, at most 20. Every desktop row has its own header, so every
  message shows it.
- **iOS and Android** (`OfficeFrames` / `OfficeChannelService` in
  `OfficeChannels.swift` and `OfficeChannels.kt`): the server channel rooms read
  the outer count from live frames and history bodies, map the sender ack
  through a 200-entry ref table, and show "to N" after the time in
  `ChatScreen.swift` / `ChatMessageParts.kt` with the same accessibility text.
  The phones reach DMs through the legacy relay, not `chat-direct` rooms, so
  their DMs show nothing until they use the server DM rooms. Not built or run
  here (no Swift or Kotlin toolchain). The phones have no hover and ignore
  `recipientNames` for now.

Tests: `app/tests/test_chat_recipient_count.py` (relay: DM, private room,
public room, notices, webhook, forged and missing counts, sender ack, stored
and replayed bodies), `test_chat_channels_api.py` (history field),
`test_chat_recipient_count_frontend.py` (transport and label rendering),
`desktop/tests/test_chat_transport.cpp` `testRecipientCounts`, the window test
`--recipient-label-only`, and the native `OfficeChannelsTests.swift` /
`OfficeAndDiscordTest.kt` cases.

## Root cause of the 2026-09-28 report

jett created `#not-very-secret` from the phone at 22:58 UTC and saw everyone in
it. The phone never called `POST /api/chat/channels` (n1 caddy log for the
phone's address shows only reads and the `rooms/world-general/ws` socket),
and no `chat.channel.create` audit row exists after 2026-07-27. The room was a
client-side mesh "private room": a label on frames sent into the shared World
#general room, which every signed-in device joins.

- Frames went to every socket in world-general, and `RoomSeal::sealObject`
  wraps the content key for every device key advertised in that room, so any
  connected device could decrypt them. Only the receiving client decided
  whether to show them.
- iOS and Android `ChatExtras.apply` add any conversation carrying
  `private: true` to their private set, so those phones joined the room on
  sight. The desktop showed a mesh private room's member list as every
  account in the directory ("DATABASE USERS IN THIS ROOM").
- Chat "persist" frames were retained by the relay for up to 7 days.

Nothing was stored as a private room on the server, so there is nothing to
repair with a migration: the two private rooms that do exist (2026-07-24 and
2026-07-27) already have explicit member rows and are enforced. To have a
really private room, create it again with "New private room" (desktop), the
web chat's Private option, or the phones' Private option, and invite people.
The old `#not-very-secret` messages cannot be recalled from devices that were
connected to World #general at the time.

## Remaining legacy behaviour

The mesh private-room engine is still in the phones (`ChatExtras` on iOS and
Android) so older conversations keep rendering, and it still auto-joins a
private-labelled frame. No client offers to create one any more, and the
desktop no longer sends or accepts mesh `invite` frames. Mesh DMs between
nodes (`openDirectChat`, the phones' `@peer` conversations) still ride the
shared room; the desktop only falls back to them when it has no account key
for the relay. That shared room is a relay room, not the local `#mesh`
room; direct messages over the local mesh are described above.
