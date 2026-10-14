# Read state across devices

Reading a chat on one device clears its unread count on the account's other
clients (web `/chat`, Office, desktop, iOS, Android). The relay cannot know
what a person has seen, so the displaying client reports it. Home page:
[app/README.md](README.md). Related:
[chat-unread-reminders.md](chat-unread-reminders.md).

## Storage and API

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

## Client rules

1. While a conversation is on screen, queue the newest loaded `ts`; post in
   one batch about a second later.
2. On start, reconnect, resume and every `chat-read` event, fetch marks and
   clear unread messages at or before them.
3. New arrivals at or before the mark are not unread.

Direct-message counts still come from the server's `unreadCount`.

Tests: `app/tests/test_chat_reads_api.py`,
`app/tests/test_chat_read_marks_clients.py`.
