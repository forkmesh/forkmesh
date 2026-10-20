# Mail: the #email-log channel, tracking and core delivery

Every email the platform sends leaves a note in a private chat channel,
**#email-log**, visible only to the account named in `MAIL_LOG_OWNER`. The
operator sees all outgoing mail and its outcome in one place.

## Code map

| Piece | Where |
|---|---|
| Notes, scrubbing, batching, outbox drain, channel provisioning | `app/src/mail_log.py` |
| Send-path hook | `entry._mail_log_note`, called from `_send_email` and package mail (`send_mailtrap`) |
| Delivery follow-ups | `process_mail_event` / `mailtrap_webhook_handler` → `mail_log.webhook_note` |
| Per-send tracking, opens, clicks, views, tracked footers | `app/src/mail_activity.py`, tables `email_sends`, `email_footers` |
| Daily budget | `app/src/mail_budget.py` |
| Posting into the room | `entry._chat_channel_deliver` (the channel incoming-webhook path) |
| Cron phase | `mail-log`, every minute |
| Outbox table | `app/migrations/0209_mail_log_outbox.sql` |
| Core mail tooling | `tools/next_mail.py`, `deploy/core/mail_events_agent.py` |
| Tests | `app/tests/test_mail_log.py`, `app/tests/host/test_mail_log_e2e.py`, `app/tests/test_mail_activity.py` |

## The notices channel

#email-log is an ordinary private `chat_channels` row with `kind: "notices"`
in its sealed record. The server must write into it, so it is not end-to-end
encrypted; it uses the room cipher only. Compared with a normal private
channel it is stricter:

- Only its creator can see it; admins get the same answer as for a channel
  that does not exist.
- It is read-only: no rename, members, webhooks, visibility change or delete
  (`403 notices_channel_read_only`). Payloads carry `readOnly: true`,
  `serverVisible: true`, `kind: "notices"`.
- Every client labels it **server-visible** and disables the composer.
- Its name index is per owner, so creating it reveals nothing to others.

A bot app was rejected for this: app posting is limited to public channels,
and a private-channel exception would be a new trust path.

## The note

One card per event, sent as block-kit `blocks` plus the same layout as plain
text. Headline: kind, result and UTC time. Fields: To, From, Subject, Body
(first 200 characters, scrubbed), Reason, Detail (opens, clicks, views),
Budget (on suppressed Mailtrap sends), Route (transport and node), Source
(mail sent outside the app), Message ID. Each kind, result and field label
has its own emoji (`KIND_EMOJI`, `RESULT_EMOJI`, `FIELD_EMOJI`).

Results: `accepted`, `REJECTED`, `FAILED`, `suppressed` (budget, provider
hold, a domain that receives no mail, or a suppressed recipient), and later
`delivered`, `BOUNCED`, `soft-bounced`, `COMPLAINED`, `BLOCKED`, `opened`,
`clicked`, `viewed`.

Scrubbing: URLs become `[link]`, token-like runs of 24+ characters become
`[redacted]`, 6+ digit runs become `[code]`. Notes never contain links,
tokens or more than 200 body characters. The queued note is sealed with
`encrypt_row`. The send's scrubbed excerpt is stored with the send so later
notes repeat it.

## Delivery

1. **Send path.** Each outcome of `_send_email` and package mail inserts one
   row into `mail_log_outbox`. The hook swallows every exception, so a broken
   log never changes the send result.
2. **Follow-ups** from the Postfix log or the provider webhook are queued
   the same way.
3. **Cron `mail-log`** posts up to 40 due notes per run in messages of at
   most 20 notes, deletes each message's notes once posted, trims the backlog
   to 1,000 notes, and retries failures with backoff (dropped after 6
   attempts). The channel is created on the first drain.

**Mail sent outside the app** also lands here: Postfix events with no app
send record are labelled "Not sent by the ForkMesh app"; tools that send
through a core's Postfix and the IDS direct mailer write `mail_log_node_notes`
rows (cores through rqlite, runners through a signed
`POST /api/runners/mail-log-note`), which the cron moves into the sealed
outbox. A new sender must call `_send_email`, write a node note, or send
through core Postfix.

## Configuration

- `MAIL_LOG_OWNER`: the owning account; empty or unknown means notes wait.
- `MAIL_TRANSPORT = "smtp"` sends through Postfix on the cores; otherwise the
  configured mail API.
- Changing `MAIL_LOG_OWNER` provisions a new channel; never edit a notices
  channel's members or visibility in SQL.

## Daily mail budget

On a metered mail API, `mail_budget.py` caps sends per UTC day
(`MAIL_DAILY_LIMIT`) and reserves `MAIL_ACCOUNT_RESERVE_PERCENT` for
verification, password and account mail. A provider "daily limit" rejection
sets a provider hold until the retry time. With core SMTP the budget does not
apply; Postfix's per-client rate limit, the IDS email cap and the marketing
limit still do.

## Admin test email

Settings, Admin access, **Send test email** (`POST /api/accounts/test-email`)
sends kind `admin_test` to the signed-in admin's own address through the
normal path, at most once a minute.

## Tracking

- Every accepted send gets an `email_sends` row: send id, kind, recipient
  blind index, `ref`, sealed details and excerpt, and first-time and count
  for opens, clicks and views. Mail records expire after 90 days.
- Opens and clicks are tracked only on notification kinds that carry an
  unsubscribe header; account and security mail is never tracked. A view
  token (`mail_activity.view_token`) lets a page opened from an email record
  a view.
- `mail_activity.FOLLOWERS` lets a feature (such as tasks) hear its own
  sends' events.
- Package mail on the cores carries one send id for both mail tables, its
  package variables, and a tracked logo footer that records opens. A package
  that tracks its own opens and clicks sends `"tracked": true` in the
  `send_mailtrap` payload; the host drops the key and skips its footer, so the
  email carries one logo, not two (the CRM's automated outreach does this).
- `GET /api/mail/footer` gives each account a tracked signature footer for
  mail written in other mail apps; it counts opens per account, not per
  message.
- Bounces, rejects and unsubscribes add the recipient to `mail_suppressions`.
- `www/public/privacy.html` ("Email we send") discloses tracking.

## Sending from the cores

- Postfix is send-only on loopback, with rate limits and opportunistic TLS.
  OpenDKIM signs with selector `fmnext`, signing and oversigning the identity
  and one-click unsubscribe headers, `relaxed/relaxed` canonicalization.
- Each core's hostname, HELO, forward DNS and PTR (IPv4 and IPv6) match;
  Postfix prefers IPv4. `next_mail.py check nN` verifies forward-confirmed
  reverse DNS.
- `fmnext-mail-events` follows the Postfix journal (no network) and turns
  results into delivery, soft-bounce and bounce events; `mail_sends` maps send
  ids to Message-IDs and the latest DSN code.
- Every send carries `Auto-Submitted` and `Feedback-ID: <kind>:forkmesh`.
- `/status` checks that Postfix answers on loopback and that recent sends are
  not stuck.
- Postfix's own non-delivery notices are DKIM-signed too
  (`internal_mail_filter_classes = bounce`, OpenDKIM `SubDomains yes`, so
  `MAILER-DAEMON@nN.next.forkmesh.com` signs as `forkmesh.com`). Unsigned, they
  failed DMARC: on 2026-10-20 Zoho refused n4's notice to our own sender with
  `554 5.7.7 Email policy violation`. Rolling this out needs
  `next_mail.py install nN --execute` on each core.
- A bounce whose reply says the address does not exist sets
  `mail_sends.cause = 'recipient'` (`mail_tracking.bounce_cause`, migration
  `0278_mail_sends_cause`), whatever DSN the server used: Tutanota answers
  `550 5.7.1 Recipient address rejected: Recipient not available`, which raised
  a false "security or policy" alert the same day. A reply that also mentions
  spam, blocking, reputation, policy, SPF, DKIM, DMARC or authentication stays
  a policy refusal and keeps `/status` red. Either way the address goes into
  `mail_suppressions`, so it is never mailed again.
- Setup with `tools/next_mail.py`: `dkim`, `dns`, `rdns` once; per core
  `install` then `restart`; `check`.

## Branding

Every card from `_forkmesh_email_card_html` starts with
`assets/logo-96.png` (see Layout below). Inbox
avatars use BIMI: `www/public/assets/forkmesh-bimi.svg` (SVG Tiny PS),
TXT `default._bimi.forkmesh.com`, and DMARC at `p=quarantine`. Gmail and
Apple Mail additionally need a mark certificate. Every sender for the domain
must sign with DKIM before it sends.

### Layout

In October 2026 the card was redrawn after the user compared three templates
("Notification", "Card", "Letter") and picked the Letter: a quiet, centred
480px column on `#fafafa` with no bordered card, the logo on its own, a 22px
heading, a grey intro, the body, a near-black (`#171717`) button, the action
URL written out under it ("Or open this link"), and a hairline footer with
"Email settings". The logo is `assets/logo-96.png` at 32px wide with
`height:auto`; the user asked for the black tile to go and for the logo not
to be squeezed into a fixed box. `logo-96.png` is the dark cube, so it reads
on the light page without a backing tile. The palette stays light-only so a
client that half-applies dark mode cannot make it unreadable. The wrapper
keeps its two `div`s, so code that inserts before
`</div></div></body></html>` (status logs, the manage link, the instance
"Powered by" line) still lands inside the column. `_instance_brand_email`
rewrites the logo's `alt`, the footer name and the settings link for
white-label instances.
