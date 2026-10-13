# Unread channel reminder emails

When people post in a private room you belong to and you don't see it, you get
one email saying so, the way Slack does. Home page: [app/README.md](README.md).
Related: [chat-private-rooms.md](chat-private-rooms.md),
[cron-and-durable-objects.md](cron-and-durable-objects.md),
[../MAIL_LOG.md](../MAIL_LOG.md), [../REKEY.md](../REKEY.md).

## Who gets one

- Members of a **private** room: its creator plus every `chat_channel_members`
  row. Public channels are skipped (they have no membership; #general already
  has its own daily digest, `general_chat`). Notices channels such as
  `#email-log` are skipped too.
- Only messages count. The relay sees ciphertext only, so it uses the
  plaintext `notify: true` flag clients set on real messages. Edits, reactions,
  key adverts and webhook posts don't carry it, so they never trigger an email.
  Webhook posts already have their own `chat_webhook` ping.
- A message has to be at least 30 minutes old (`REMINDER_DELAY_MS`) and still
  unread. A message from before the member joined doesn't count.
- The address must be verified, email notifications on, pings not paused, and
  the `channel_unread` kind neither muted nor email-off. The kind appears in
  every settings page as "Unread channel messages" (Conversations), on by
  default.

## One time

`chat_channel_reads` (migration 0231) holds one row per room and member:

| column | meaning |
| --- | --- |
| `read_at` | the last time this member saw the room |
| `reminded_at` | the last time a reminder covered the room |

A member is reminded about a room only while `reminded_at <= read_at`. After a
reminder, that room sends nothing more, however many messages follow, until
the member opens it again. Then the next unread episode can send one more
email. A member who is skipped (opted out, no verified address) is stamped as
reminded too, so the cron doesn't look them up again on every pass.

## What counts as "seen"

`read_at` only ever moves forward. It is set when:

- a client asks for `room-access` (opening the room's live socket) or
  `history` for that room (`chat_channels_api._mark_read`);
- a message lands in the room: the room Durable Object stamps the sender and
  every account with a live socket in it (`_maybe_retain` →
  `chat_channel_mark_read`). One upsert per message, capped at 100 accounts.

So a message delivered live to any of a person's clients counts as seen. That
includes a desktop left running, because `OfficeChannelMirror` keeps up to 16
room sockets open. Such a person sees the unread dot in the app instead of an
email.

## The cron pass

Phase `channel-unread-email` runs every five minutes, at residue 3 next to the
two digests (`channel_reminders.send_reminders`, main relay only):

1. Find rooms with a `notify` message aged 30 to 90 minutes
   (`REMINDER_WINDOW_MS`), oldest first, at most 4 rooms per pass. A message
   stays a candidate for an hour, so a missed pass or a full slice is caught
   up later. The one-time rule stops overlapping passes from sending twice.
2. For each room, read its audience, its `notify` timestamps from the last 24
   hours (at most 200), and its `chat_channel_reads` rows.
3. Group by member, so one email lists every room with unread messages and
   how many. At most 4 members per pass. The rest wait for the next pass.
4. Send (`email_kind="channel_unread"`, with a List-Unsubscribe header), stamp
   the account's last-email record, and set `reminded_at` for every room
   listed, whether the send worked or not.

The email names rooms and counts only. Message text is end-to-end encrypted
and never reaches the server in plaintext, so it can't be quoted.

## Cleanup

Deleting a room deletes its `chat_channel_reads` rows. Removing a member
deletes that member's row. `tools/next_rekey.py` recomputes `account_bi` on a
key rotation.
