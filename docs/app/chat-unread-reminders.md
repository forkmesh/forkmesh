# Unread channel reminder emails

Members of a private room who miss new messages get one reminder email per
unread episode. Home page: [app/README.md](README.md). Related:
[chat-private-rooms.md](chat-private-rooms.md),
[cron-and-durable-objects.md](cron-and-durable-objects.md).

## Rules

- Private rooms only; only frames with the plaintext `notify: true` flag count.
- A message must be at least 30 minutes old (`REMINDER_DELAY_MS`), unread and
  newer than the member's join.
- Requires a verified address and the `channel_unread` kind enabled (on by
  default).
- `chat_channel_reads` holds `read_at` and `reminded_at` per room and member;
  a member is reminded only while `reminded_at <= read_at`. `read_at` moves
  forward on room access, history reads and live delivery to any client.

## Cron

Phase `channel-unread-email` (`channel_reminders.send_reminders`) runs every
five minutes: up to 4 rooms with a `notify` message aged 30 to 90 minutes
(`REMINDER_WINDOW_MS`), grouped into one email per member (up to 4 per pass),
sent as `email_kind="channel_unread"` with List-Unsubscribe. `reminded_at` is
set whether or not the send succeeds. The email names rooms and counts only.

Read rows are deleted with the room or member; `tools/next_rekey.py`
recomputes `account_bi`.
