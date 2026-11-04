# Read state and unread reminders

How chat knows what a person has read: read marks synced across the
account's devices, and the reminder emails sent for unread private-room
messages and for direct messages still waiting on the server. Part of
[the App docs](README.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[cron-and-durable-objects.md](cron-and-durable-objects.md).

## Read state across devices

Reading a chat on one device clears its unread count on the account's other
clients (web `/chat`, Office, desktop, iOS, Android). The relay cannot know
what a person has seen, so the displaying client reports it.

### Storage and API

`chat_read_marks`: one row per account and conversation (`#<name>`,
`private:<32 hex>`, `direct:<32 hex>`) holding `read_ts`, the sender `ts` of
the newest seen message. Marks only move forward; 500 kept per account
(`READS_KEEP_MAX`). Local-only rooms are not synced.

- `GET /api/chat/reads` → `{"marks": {...}, "readAt": {...}}`
- `POST /api/chat/reads` with 1 to 50 marks; any bad entry → `400 invalid_marks`

Auth is a session, or for the desktop an Ed25519-signed query over
`forkmesh-chat-reads-list-v1\n<node>\n<ts>` or
`forkmesh-chat-reads-mark-v1\n<node>\n<ts>\n<digest>` (`marks_digest`). A POST
that advances a mark emits the payload-free account event `chat-read` (never a
phone push).

### Client rules

1. While a conversation is on screen, queue the newest loaded `ts`; post in
   one batch about a second later.
2. On start, reconnect, resume and every `chat-read` event, fetch marks and
   clear unread messages at or before them.
3. New arrivals at or before the mark are not unread.

Direct-message counts still come from the server's `unreadCount`.

Tests: `app/tests/test_chat_reads_api.py`,
`app/tests/test_chat_read_marks_clients.py`.

## Unread private-room reminders

Members of a private room who miss new messages get one reminder email per
unread episode.

### Rules

- Private rooms only; only frames with the plaintext `notify: true` flag count.
- A message must be at least 30 minutes old (`REMINDER_DELAY_MS`), unread and
  newer than the member's join.
- Requires a verified address and the `channel_unread` kind enabled (on by
  default).
- `chat_channel_reads` holds `read_at` and `reminded_at` per room and member;
  a member is reminded only while `reminded_at <= read_at`. `read_at` moves
  forward on room access, history reads and live delivery to any client.

### Cron

Phase `channel-unread-email` (`channel_reminders.send_reminders`) runs every
five minutes: up to 4 rooms with a `notify` message aged 30 to 90 minutes
(`REMINDER_WINDOW_MS`), grouped into one email per member (up to 4 per pass),
sent as `email_kind="channel_unread"` with List-Unsubscribe. `reminded_at` is
set whether or not the send succeeds. The email says only "You have unread
messages on ForkMesh" and a total count. It names no room, because a private
room's name is itself private and the mail goes through third-party
mailboxes.

Read rows are deleted with the room or member; `tools/next_rekey.py`
recomputes `account_bi`.

## Direct messages waiting on the server

The relay keeps an encrypted copy of a direct message only until one of the
recipient's devices acknowledges it ([chat-private-rooms.md](chat-private-rooms.md#server-copies-of-direct-messages)).
A recipient whose copies sit there for a day gets one email asking them to
open chat, so the copies are received and deleted instead of waiting out the
7-day expiry.

- Counts only `notify` rows (`msg_id` ending `:n`) sent by the other
  participant; key advertisements and the recipient's own frames are ignored.
- The oldest such row must be at least 24 hours old (`REMINDER_DELAY_MS`) and
  inside the 7-day retention window.
- `chat_direct_participants.unread_reminded_at` (migration 0260) makes it one
  email per episode: only rows stored after the last reminder count, so the
  next email needs a new message that again waits a day.
- Requires a verified address and the `direct_unread` kind ("Unread direct
  messages", on by default), sent as `email_kind="direct_unread"` with
  List-Unsubscribe. The email has the subject "You have unread messages on
  ForkMesh" and a count; it names no sender (who writes to whom is private),
  and messages from an account the recipient blocked are
  not counted.

Phase `direct-unread-email` (`direct_reminders.send_reminders`) runs every
five minutes with the reward sweeps (minute % 5 == 1): one joined query over the
`chat-direct:` rows, up to 4 recipients per pass, one email per recipient
listing every waiting sender. `unread_reminded_at` is set whether or not the
send succeeds.
