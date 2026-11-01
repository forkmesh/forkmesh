# Pings and notification email

Part of [the App docs](README.md). A Ping is a `notifications` row: an inbox
entry that wakes web and desktop clients and pushes to phones, and that may
be emailed. This page covers who hears about an issue or pull request thread,
what the issue sidebar and an inbox ping say, how every new issue raises a
ping, how pings are emailed exactly once, and how the server names the
author of a collaboration event. Task pings are in
[tasks.md](tasks.md#pings); marketing mail is in
[marketing-email.md](marketing-email.md).

## Thread notifications

An issue page's sidebar has a Notifications box: a Subscribe/Unsubscribe
button and one sentence saying whether you will hear about the thread.

### Who hears about a thread

| Who | How | Can mute it |
| --- | --- | --- |
| The repository owner (the hosting node, or the account that owns that node) | `notify_pending_inbox`: every event that lands in the relay inbox | Yes, except the ping for a new issue or pull request |
| Anyone who commented | `subscribe_thread` after their comment, then `notify_subscribers` | Yes |
| Anyone assigned to the issue | `notify_issue_assignees` pings them and subscribes them | Yes |
| Anyone @mentioned in an issue or pull request | `notify_mentions` pings them and subscribes them | Yes |
| Anyone who pressed Subscribe | `subscribe_handler` | Yes |

A mute (Unsubscribe) is a `thread_subscriptions` row with `muted` set. It is
sticky: a later comment, assignment or mention does not re-subscribe you.
`notify_subscribers` skips muted rows, and `notify_pending_inbox` checks
`_thread_muted` for the hosting node and its owning account, so an owner who
unsubscribes stops hearing about that thread. The ping for a brand-new
item is never muted: there is no thread to mute yet.

### What the sidebar says

`POST /api/repo/<owner>/<repo>/subscribe` with `action: "status"` answers
`subscribed` and a `reason`:

| `reason` | `subscribed` | Sentence |
| --- | --- | --- |
| `subscribed` | true | You're receiving notifications because you're subscribed to this thread. |
| `owner` | true | You're receiving notifications because you own this repository. |
| `muted` | false | You're not receiving notifications from this thread. |
| empty | false | You're not receiving notifications from this thread. |

The owner gets every inbox ping without a subscription row, so the status
must report `owner` rather than look only for a row. `issueSubscriptionNote`
in `06-repo-content.js` picks the sentence.

### What an inbox ping says

For an event without text (an approval with no note, closing a pull request,
resolving a review thread, closing, assigning or relabelling an issue),
`pending_inbox_action` names the event instead of a generic "activity" title:

- title: `Pull request approved in owner/repo`, `Pull request closed in …`,
  `Issue assigned in …`, `Issue closed in …`, and so on;
- body: the pull request's title (read with `pull_attention.pull_record`),
  then a sentence such as `alice approved pull request #3.`, then any note the
  person wrote;
- link: the issue or pull request itself whenever its number is known.

An unknown event type gets `There's new activity on issue #12.`. The pull request title is looked up
only for events without text, so a plain comment costs no extra read.

### Opening the comment

A mention or a followed-thread ping for a comment stores the comment's event
id with the number and source (`meta.comment`, at most 80 characters;
`notify_mentions` and `notify_subscribers` take it as `comment=`). The feed
and its detail panel (`homeFeedHref` in `05-repo-list-explorer.js`) link
`/<owner>/<repo>/{issues,pull,discussions}/<n>#comment-<id>`, and
`initCommentAnchor` in `09-comment-reactions.js` waits up to 20 seconds for
that comment's reaction bar, scrolls it into view and outlines it for four
seconds. Pings without a comment id still open the thread.
`app/tests/test_comment_notification_links.py` covers the links.

## New issue pings

Every new issue should raise an `issue_opened` ping ("New issue in owner/repo").
`issue_opened` is in `NOTIFICATION_IMMEDIATE_KINDS`, so it is also emailed
straight away, not held for the hourly digest (see
[Notification email](#notification-email)). Email is on by default.

### Two ways an issue appears

1. **Through the relay inbox.** The web composer, the phone apps, a desktop
   filing on someone else's repo, ForkBot, the Discord bug watcher, apps and
   microjobs all write to `issue_inbox` and call
   `notify_pending_inbox(..., opened=True)`. That pings the hosting account and
   the owners, admins and maintainers of an organization that fronts the repo
   (issue #714). Each takes its number from the relay's `issue_seq` counter
   (`_forkbot_next_issue_number`).
2. **Directly on a node.** Some issues never touch the inbox: one created in
   the hosting desktop's Issues tab, one filed by an agent or crash-repair
   dispatch on that desktop, or one taken by a mirror's direct intake
   (`tools/mirror_gateway.py` → `server/intake.go`). Without the catalog path
   below, organization teammates would never hear about these.

### Catching the second kind: `notify_catalog_issues_opened`

Every catalog publication, from the desktop or from `server/catalog.go`,
carries `issueMaxNumber`. When an accepted update raises it,
`catalog_routes.py` calls `notify_catalog_issues_opened` (in `entry.py`):

- Only numbers at or above `issue_seq.next_number` count. Anything lower was
  handed out by the relay, and the inbox has already pinged it. If the counter
  can't be read, nothing is sent: a missed ping is better than a duplicate
  banner.
- Recipients are the organization owners, admins and maintainers
  (`_org_repo_maintainers`) of the publishing owner and repo, leaving out the
  publishing node. A mirror publishing someone else's repo matches no
  `org_repos` row, so it pings no one. A personal repo with no organization
  pings no one either: on that path the only person is the node's owner, who
  made the issue.
- The actor is the publishing node. The catalog doesn't say who wrote the
  issue, and fetching it inline would hold up the publication, so the body
  reads only `Issue #N` and links to the issue.
- At most `PING_CATALOG_ISSUES_MAX` (5) of the newest numbers are pinged per
  publication, so a big import or renumbering can't flood anyone's phone.
- The dedupe key is `issue-opened:<owner>/<repo>:<N>`. A republish at the same
  maximum sends nothing, because the maximum didn't rise.
- The first publication of a repo, or a previous record with no
  `issueMaxNumber`, sends nothing.

A known gap: if the relay reserves N while the desktop has already made a
local N, the desktop moves the web issue to N+1. The catalog then pings N+1
(already pinged through the inbox as N), and the local N goes unpinged.

Tests: `app/tests/test_catalog_issue_pings.py`.

### Where a ping's card links

`repo_record_href` (`app/src/entry.py`) links a notification to the thing it
is about: one issue or pull request (`/issues/<n>`, `/pull/<n>`) when its
number is known, otherwise a repository section such as `/releases` or
`/commits`, otherwise the repository. Older pings stored only the repository
root; the home feed (`homeFeedHref` in `05-repo-list-explorer.js`) still sends
those cards to the record they were about.

## Notification email

A ping (a `notifications` row) is emailed by one of three senders:

- the instant rail, `_deliver_instant_notifications`, which sends one email per
  ping for an account set to instant delivery;
- the immediate rail, `_deliver_immediate_notifications`, which sends the kinds
  that cannot wait for a digest (`notification_email_immediate`) to an account
  on digest delivery;
- the hourly digest in `send_notification_digests`, which bundles the rest.

Each runs from the notification queue consumer and from the
`notification-digest` cron phase. Every core node runs its own consumer
against the one rqlite database, so two senders can race on the same ping
(below).

### Why a watermark is not enough

Each sender reads the rows above a per-recipient watermark
(`last_digest_ts`, `last_immediate_ts`) kept inside the encrypted account
record, sends, and saves the record. The save overwrites the whole record
with no compare-and-swap, so two cores that load the record before either
saves both see the same ping above the watermark and both email it.

A re-file can also lift a ping back above the watermark: `enqueue_notification`
re-files under the same dedupe key with an upsert, and a new `ts` would
re-mail the ping and move it to the top of the inbox (see "Re-filing keeps
`ts`").

### The send claim (`notification_email_sends`, migration 0261)

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
- A database error on the claim sends anyway (a duplicate beats a lost
  email).

The `referrer-prune` cron phase (hourly, minute 17) deletes claims older than
30 days (`NOTIFICATION_EMAIL_SEND_RETAIN_MS`). The races the claim guards
against last seconds, and every sender reads only above a watermark that each
pass advances, so a claim is only needed again if a watermark stood still for
a month.

### Re-filing keeps `ts` (`notifications.content_bi`)

`enqueue_notification` stores `content_bi`, a blind index of the payload
without its `ts` and `readAt`. On a re-file, the upsert keeps the stored `ts`
and `data` when `content_bi` is unchanged, and takes the new `ts` and `data`
when the content changed. A re-file with the same content (a replayed
webhook, a heartbeat reporting the same state) neither re-mails the ping nor
moves it in the inbox; one with different content (an error group whose count
grew) re-surfaces. A row with an empty `content_bi` (older than migration
0261) bumps `ts` once on its next re-file.

A caller that wants a recurring event to ring again must change the content
or use a new dedupe key. `credits_refilled` keeps one row per window kind with
the same text, so a repeat refill does not re-mail an unread ping.

### Re-keying

`notification_email_sends` is emptied by `tools/next_rekey.py`
([rekey](../operations/rekey.md)): its keys are HMACs under the old key and match
nothing afterwards. `notifications.content_bi` is pseudonymised like
`dedupe_bi`, so the first re-file of each row after a re-key bumps `ts` once.
`content_bi` is also declared in `app/src/private_surface_audit.py`, the
contract of columns that may appear next to a private row's ciphertext.

## Collaboration author display

The issue, pull request, branch comment and discussion inbox handlers, and the
microjob issue report, set an event's `authorName` on the server
(`_bind_session_author` in `app/src/entry.py`); the value the client sent is
never kept: the event signature covers the `author` key and the content, not
`authorName`, and workflows such as `/assign` trust the name.

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
  instance ([remote-forks.md](remote-forks.md)) always lands here, because the
  browser posts it without credentials; the claim says who sent it.
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
