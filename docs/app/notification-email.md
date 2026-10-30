# Notification email: one email per ping

A ping (a `notifications` row) is emailed by one of three senders:

- the instant rail, `_deliver_instant_notifications`, which sends one email per
  ping for an account set to instant delivery;
- the immediate rail, `_deliver_immediate_notifications`, which sends the kinds
  that cannot wait for a digest (`notification_email_immediate`) to an account
  on digest delivery;
- the hourly digest in `send_notification_digests`, which bundles the rest.

Each runs from the notification queue consumer and from the
`notification-digest` cron phase, and on ForkMesh Next every core (n2, n3, n4)
runs its own consumer against the one rqlite database.

## Collaboration author display

The issue, pull request, branch comment and discussion inbox handlers, and the
microjob issue report, set an event's `authorName` on the server
(`_bind_session_author` in `app/src/entry.py`); the value the client sent is
never kept. The event signature covers the `author` key and the content, not
`authorName`, so before this anyone could post a comment that named another
account, and workflows such as `/assign` trusted it.

- A browser signing key is generated per browser and belongs to no account. A
  desktop or mobile key is bound: the account's primary key (`nodes`) or an
  enabled, unrevoked device key (`account_devices`), resolved by
  `_git_push_account_for_pubkey`.
- With a signed-in session (body `sessionToken`, bearer or cookie; an
  unverified email still counts), `authorName` is the session's account. If
  the signing key is bound to a different account the submission is refused
  with `403 author_mismatch`.
- Without a session, a bound key names its account.
- Otherwise the submission is anonymous: `authorName` is empty, never a
  username. The name the client sent, if it is a valid account name, is kept
  separately as `authorClaimedName`. A pull request sent from another
  instance (`docs/app/remote-forks.md`) always lands here, because the browser
  posts it without credentials, and showing only the key hid who sent it.
  `server/intake.go` writes the claim into `pull.md` and the pull event files.
  The pull pages show it as `name (unverified)` with no profile link
  (`recordAuthorName` in `app/public/dashboard/js/02-helpers.js`,
  `_record_author` in `app/host/forkmesh_host/repo_views.py`). Nothing else
  reads it: notifications, permissions and workflows use only `authorName`.
  The server drops `authorClaimedName` whenever it binds a verified name.
  Pulls stored before this change have no claim and still show the key.
- `authorVerified` is set to `true` when a name was bound this way and
  `false` otherwise, overwriting anything the client sent. It travels with the
  event into the ledger.
- Actions job API events are built by the server and signed by the
  `<org>-actions` bot; they keep the bot name and are verified.

The mirror gateway's direct intake (`tools/mirror_gateway.py`) already sets
`authorName` from its portable session.

## Why a watermark is not enough

Each sender reads the rows above a per-recipient watermark
(`last_digest_ts`, `last_immediate_ts`) kept inside the encrypted account
record, sends, and saves the record. The save overwrites the whole record
with no compare-and-swap, so two cores that load the record before either
saves both see the same ping above the watermark and both email it. Seen in
production on 2026-10-14 as the same ping arriving twice.

A second path re-mailed pings without any race: `enqueue_notification`
re-files a ping under the same dedupe key with an upsert, and the upsert used
to set `ts` to the time of the re-file. A ping that had already been mailed
then sat above the watermark again and was mailed again, and it also jumped to
the top of the inbox.

## The send claim (`notification_email_sends`, migration 0261)

Before any of the three senders emails a ping it claims it:

```
INSERT OR IGNORE INTO notification_email_sends (send_bi, created_at)
```

`send_bi` is `blind_index("notification-email:" + address, kind, repo,
row id, ts)`: the lowercased destination address, the ping's kind and repo,
the row's `dedupe_bi`, and the row's `ts`. When the insert changes no row,
another sender already claimed it, and the ping is skipped but still counted
as delivered so the watermark moves past it. Only the HMAC is stored; no
address or ping text is in the table.

- Different pings to the same address have different row ids, so each is
  sent.
- A ping re-filed with new content gets a new `ts` (below) and a new key, so
  the update is sent.
- A failed send deletes its claim, so the next pass retries it, as before.
  A process that dies between the claim and the send loses that email rather
  than sending it twice.
- The digest claims each item it would bundle and leaves out the items that
  are already claimed; when none are left it only advances the watermark.
- A database error on the claim sends anyway, the behaviour from before the
  table existed.

The `referrer-prune` cron phase (hourly, minute 17) deletes claims older than
30 days (`NOTIFICATION_EMAIL_SEND_RETAIN_MS`). The races the claim guards
against last seconds, and every sender reads only above a watermark that each
pass advances, so a claim is only needed again if a watermark stood still for
a month.

## Re-filing keeps `ts` (`notifications.content_bi`)

`enqueue_notification` stores `content_bi`, a blind index of the payload
without its `ts` and `readAt`. On a re-file, the upsert keeps the stored `ts`
and `data` when `content_bi` is unchanged, and takes the new `ts` and `data`
when the content changed. A re-file with the same content (a replayed
webhook, a heartbeat that reports the same state again) therefore no longer
re-mails the ping or moves it in the inbox. A re-file with different content
(for example an error group whose count grew) still re-surfaces. Rows from
before migration 0261 have an empty `content_bi`, so their first re-file
after the deploy bumps `ts` one last time.

A caller that wants a recurring event to ring again must change the content
or use a new dedupe key. `credits_refilled` keeps one row per window kind with
the same text, so a repeat refill no longer re-mails a ping that is still
unread; once the ping is read it was never re-mailed anyway.

## Re-keying

`notification_email_sends` is emptied by `tools/next_rekey.py`
([REKEY.md](../REKEY.md)): its keys are HMACs under the old key and match
nothing afterwards. `notifications.content_bi` is pseudonymised like
`dedupe_bi`, so the first re-file of each row after a re-key bumps `ts` once.
`content_bi` is also declared in `app/src/private_surface_audit.py`, the
contract of columns that may appear next to a private row's ciphertext.
