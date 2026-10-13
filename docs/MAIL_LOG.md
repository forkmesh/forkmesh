# Mail log: a private #email-log channel for all outgoing mail

Every email ForkMesh sends through Mailtrap also puts a note in a
private chat channel, **#email-log**. Only the account named in
`MAIL_LOG_OWNER` can see it (`jett` in production, set in
`app/wrangler.toml`). This lets the operator see all outgoing mail in one
place, whether the send worked or not, without opening Mailtrap.

## Code map

| Piece | Where |
|---|---|
| Notes, scrubbing, masking, batching, outbox drain, channel provisioning | `app/src/mail_log.py` (lazy module) |
| Send-path hook | `entry._mail_log_note`, called from `_send_email` and `_PluginApiRuntime.send_mailtrap` |
| Mailtrap delivery-webhook follow-ups | `route_handlers.mailtrap_webhook_handler` → `mail_log.webhook_note` |
| Every send, its opens, clicks and views | `app/src/mail_activity.py` (lazy module), table `email_sends` (migration 0247); see "Every email is tracked" |
| Tracked footer for mail sent from Zoho Mail or any other mail app | `mail_activity.footer*`, table `email_footers` (migration 0250), `route_handlers.mail_footer_handler` (`GET /api/mail/footer`) and `mail_footer_image_handler` (`/api/mail/t/f/<token>.png`), Settings → Account → Tracked email footer (`app/public/dashboard/js/04-account.js`); see "Package mail and the tracked footer" |
| Posting into the room | `entry._chat_channel_deliver` (the same code the channel incoming webhooks use; `_ChatChannelsRuntime.deliver_webhook` now calls it) |
| Cron phase | `mail-log`, every minute (`_cron_due_phases`, `cron_activity.CRON_PHASE_CATALOG`) |
| Owner-only visibility | `chat_channels_api._owner_only_denied`, the channel socket checks in `entry.py` and `world/src/world_office_routes.py`, and the room re-admission checks in `entry.py` and `world/src/world_durable_objects.py` |
| Web UI | `app/public/chat.js`: "server-visible" badge, seal panel text, read-only composer |
| Table | `app/migrations/0209_mail_log_outbox.sql` |
| Tests | `app/tests/test_mail_log.py`, `app/tests/host/test_mail_log_e2e.py`, `app/tests/test_mail_activity.py` |

## Design choice: a server-postable "notices" channel, not an App

The server has to write into this channel, so it cannot be end-to-end
encrypted. There were two options.

- **(b) A ForkMesh Apps bot.** Rejected. `chat.postMessage` only posts in
  public channels (APPS.md, "Private channels and direct messages are sealed
  to their members"). Using it would mean one of two things:
  - making the channel public, which exposes it to everyone; or
  - teaching Apps to post into one private channel, which is a new trust
    path for third-party code.

  It would also need a registered app, an OAuth install, tokens and an
  external event endpoint, all to reach code that already runs on the server.
- **(a) A server-postable channel type.** Chosen. It is an ordinary
  `chat_channels` row, `visibility: "private"`, with `kind: "notices"` in the
  sealed record. It reuses everything channels already have:
  - the room Durable Object;
  - the room cipher (the key is derived from `DATA_KEY`, like every channel's
    room key);
  - history and replay;
  - the desktop and web channel lists.

  The only new trust is a rule that makes this channel **stricter** than a
  normal private channel.

What `kind: "notices"` changes:

- **Only the creator (the owner) can see it.** The usual platform-admin
  override is turned off. In the list, history, room-access, members,
  webhooks, both WebSocket upgrades and both room re-admission checks, a
  notices channel is visible only to its creator. Anyone else, admins
  included, gets exactly what a channel that does not exist returns (404, no
  audit row naming the channel id).
- **It is read-only.** It cannot be renamed, made public, given members or
  webhooks, or deleted through the API. The owner gets
  `403 notices_channel_read_only`. The payload carries `canManage: false`,
  `readOnly: true`, `serverVisible: true`, `kind: "notices"`.
- **It is labelled as server-visible.**
  - Web chat: the badge reads **server-visible**, and the seal button reads
    **Server-visible**. Its panel says the server writes these notes and can
    read them, and that only you can see the channel. The composer is
    disabled.
  - Desktop: it has no seal layer. It shows the channel with the topic
    "Server-visible notices … Not end-to-end encrypted. Only you can see this
    channel." The header button reads **Server-visible**, and the composer
    is read-only.
    - Until 2026-09-28 the desktop showed nothing here. It ignored
      `serverVisible`, treated the channel as an end-to-end private room,
      and dropped every frame, because the server writes frames with the
      room key only.
    - `OfficeChannelMirror` now accepts room-layer frames for a
      server-visible channel only.
  - iOS and Android: the same gap until 2026-09-29. `OfficeChannelService`
    dropped every unsealed frame in a private room, so #email-log was listed
    but empty. It now accepts room-layer frames for a server-visible channel
    only; the badge reads **server-visible** and the composer is replaced by
    "Only ForkMesh posts in this channel." ([native/README.md](native/README.md)).
  - Settings: the gear on the channel row, or **Settings** in the header,
    opens a read-only settings view. It explains where the notes come from
    and that the channel cannot be renamed, shared, given webhooks or
    deleted.
- **Its name is not reserved globally.** The channel's `name_bi` is
  `blind("chat-channel-notices:<owner>:email-log")`, not the global channel
  name index. So nobody else learns it exists by getting
  `channel_name_taken` when they create an `#email-log` of their own.

The browser treats a notices channel as controller-readable, like a public
channel. Frames are room-layer only (no X25519 device seal), because a
private channel's browser would otherwise refuse frames the server wrote.

**What an operator can still see.** Anyone holding `DATA_KEY` can decrypt
room frames. That is true of every channel's room layer, and it is why the
UI says "server-visible". The admin Database tab shows only sealed rows.

## The note

One card per event. Each note is a block-kit `section`
(`mail_log.section`): the headline is `type · result · time`, and each
detail is a `Label: value` field. The frame carries the sections as
`blocks` and the same layout as plain `text`, one note per paragraph:

    🪪 Verification · ✅ accepted · 2026-09-28 17:35:58 UTC
    📨 To: jo@example.com
    📤 From: no-reply@forkmesh.com
    📝 Subject: Welcome to ForkMesh, jo
    📄 Body: Hi jo, confirm your address: [link] The code is [code].
    🛤️ Route: local mail (Postfix on the core) on n1
    🆔 Message ID: 172754495801.3021.1175843360519442133@forkmesh.com

    🪪 Verification · 📥 delivered · 2026-09-28 17:36:20 UTC
    📨 To: jo@example.com
    📤 From: no-reply@forkmesh.com
    📝 Subject: Welcome to ForkMesh, jo
    📄 Body: Hi jo, confirm your address: [link] The code is [code].
    🛤️ Route: local mail (Postfix on the core) on n1
    🆔 Message ID: 172754495801.3021.1175843360519442133@forkmesh.com

    📬 Notification digest · 🚫 suppressed · 2026-09-28 18:02:11 UTC
    📨 To: jo@example.com
    📤 From: no-reply@forkmesh.com
    📝 Subject: 3 new notifications
    📄 Body: 3 things happened while you were away. Open ForkMesh: [link]
    ⚠️ Reason: daily mail budget spent
    📊 Budget: 150 of 150 sent today (UTC), bulk 45 of 45 (70% kept for account mail)
    🛤️ Route: Mailtrap API

| Field | Content |
|---|---|
| Headline | 🪪 the type (from `email_kind`: Verification, Password reset, IDS alert, Notification digest, Status alert, Package mail and so on), ✅ the result (see below) and the UTC time |
| 📨 To | The full recipient address |
| 📤 From | The full envelope or From address (`<>` is a bounce notice) |
| 📝 Subject | Scrubbed |
| 📄 Body | The first 200 characters of the text part (or of the HTML with tags removed), scrubbed. On every note that can have one — see "The body on every note" |
| ⚠️ Reason | `HTTP n`, the error text, or both, when the send was refused, failed or held |
| 🔍 Detail | On an open, click or view: who did it, how many times, and the account the email was sent to |
| 📊 Budget | On a `suppressed` note sent through Mailtrap: the day's budget when the send was held. Local mail has no budget, so its notes have none |
| 🛤️ Route | The transport and the node: `local mail (Postfix on the core)` (the core's own Postfix, what production uses), `Mailtrap API`, `Mailtrap API, the package's own token`, `Mailtrap API, IDS direct`, or `Mailtrap` for a Mailtrap webhook follow-up, then `on nN` for the core whose Postfix sent it or the node that reported it. A send skipped before any transport ran names the transport it would have used |
| 🧰 Source | Only on mail sent outside the app: the tool that reported it, or "no app send record" |
| 🆔 Message ID | In full, on every note that has one. The `accepted` note and the later `delivered` or `BOUNCED` note carry the same id, so a follow-up can be matched to its send |

**Emoji.** `mail_log.KIND_EMOJI` gives each kind its own emoji and
`RESULT_EMOJI` each result; `FIELD_EMOJI` gives each field label one. They go
in the note's text, so every client gets them, including the native apps that
draw the plain text. A kind with no entry (a kind name that is not in
`KIND_LABELS`) reads `✉️`. `test_mail_log.py` holds every table to one
emoji per entry and no duplicates, so a new kind or result has to pick its own.

Until 2026-10-06 each note was a single line of ` · `-separated parts
(`2026-09-28 17:35:58Z · Verification · on n1 · via … · to … · accepted ·
msg …`). Forty of them in one message read as a wall of text, so the
operator asked for a formatted log that other server notices can reuse.

**How clients draw it.** The desktop (`desktop/src/ChatBlocks.h`) renders
the `blocks` of a frame whose sender id starts with `system:` or `app:` as
cards: the first headline part in bold, a result word in green (accepted,
delivered), amber (suppressed, soft-bounced), red (FAILED, BOUNCED and so on)
or blue (opened, clicked, viewed), the other parts muted, and the fields as a
label and value grid. The web chat (`chat-apps.js`) draws a `system:` frame's
blocks as cards in place of its text, with the same four tones (the
`--tone-*` variables in `chat.css`, which have their own light-theme values)
and the same label and value grid. Native iOS and Android still show the
plain text, which uses the same layout. Any server notice gets the same look
by sending `blocks` from a `system:` sender; [APPS.md](APPS.md) describes the
block kit.

Both renderers read the tone from the result word after any leading emoji, and
both split a field on the first `:` after an optional emoji, so a note's emoji
never costs it its colour or its grid. The web renderer uses the label and
value split only for notice cards; an app's `blocks` keep the plain field
layout they had.

**On 2026-10-08** the operator asked to "show the body on all emails in the
mail log and format and make them look nicer with emoji and colors": hence
the emoji, the web chat's colours (it had none; only the desktop coloured a
result), the blue tone for an open, click or view, and the body on the notes
that had none.

Until 2026-09-28 the note masked the recipient as `first letter***@domain`
(except the owner's own address), masked every non-ForkMesh sender, masked the
message id (a Postfix Message-ID contains an `@`, so it read `1***@forkmesh.com`),
showed the message id only on follow-ups, and had no transport, body or budget.
The channel is owner-only and the queued note is sealed, so the operator asked
for full addresses and more context.

Until 2026-09-29 the transport read `Postfix (core SMTP)`. The operator asked
which server a line went through, so it now reads `local mail (Postfix on the
core)`, next to `Mailtrap API` and the other Mailtrap names. IDS direct mail is the exception: the node
masks the recipient before the note leaves it, because `mail_log_node_notes`
rows are not sealed.

The result is one of (the detail in brackets is the note's `Reason`):
- `accepted`;
- `REJECTED (HTTP n: Mailtrap's error text)`;
- `FAILED (exception class)`;
- `suppressed (reason)`: the daily mail budget, the Mailtrap daily-limit hold,
  a recipient that receives no mail, or a recipient in `mail_suppressions`;
- and later, from Mailtrap's webhook or the core Postfix log: `delivered`,
  `BOUNCED`, `soft-bounced`, `COMPLAINED` (spam report), `BLOCKED` (reject
  or suspension).

**The budget on a suppressed line.** On the Mailtrap transport, every
`suppressed` line from `_send_email` and package mail reads the budget at that
moment
(`mail_budget.usage`, through `entry._mail_budget_text`), whatever the
reason, so a line held for another reason still shows how much budget was
left:
- `N of LIMIT sent today (UTC)`: the `mail:<day>` bucket against
  `MAIL_DAILY_LIMIT`;
- for bulk kinds, `bulk N of LIMIT (P% kept for account mail)`: the
  `mail-bulk:<day>` bucket;
- while a Mailtrap daily-limit hold is on, `Mailtrap daily limit hold until
  <time>`.

An IDS alert held by `ids_alerts.email_gate` reads `5 of 5 IDS alert emails
sent today (UTC)` (`EMAIL_DAILY_CAP`).

## The body on every note

A note's `Body` is the first 200 characters of the email's text part (or of
its HTML with tags removed), scrubbed. Until 2026-10-08 only the note written
at send time had one; every later note about the same email had none, because
the body was gone by then. Now the send's body excerpt is kept with the send
and repeated on each of its notes:

| Note | Where its body comes from |
|---|---|
| A send (`accepted`, `REJECTED`, `FAILED`, `suppressed`) | The text and HTML `_send_email` was handed, through `entry._mail_log_note` |
| Package mail (`_PluginApiRuntime.send_mailtrap`) | The package's `text` and `html` |
| An open, click or view (`mail_activity.tracked`) | `email_sends.data["body"]`, written by `mail_activity.sent` when the send was accepted |
| A delivery follow-up: `delivered`, `BOUNCED`, `soft-bounced`, `COMPLAINED`, `BLOCKED` | The same row, read by `mail_activity.send_details(send_id)` in `process_mail_event` and passed to `mail_log.webhook_note`, which now also repeats the `Subject` |
| IDS direct mail from a node | The alert body the node mailed, in the `mail_log_node_notes` row. `runner_leases._mail_log_note` kept only six fields, so a runner-role node's `body`, `via`, `source` and `messageId` were dropped before they reached `mail_log.node_note`; all four now pass |
| A task email held by its hourly cap (`world_task_email`) | The message the person would have received |

Three notes still have no body, because there is none to show: a note for mail
sent outside the app with no `email_sends` row (a tool, a cron job or a
bounce), a tracked-footer open (ForkMesh never saw the email, only its logo
being loaded), and a send refused before the body existed.

The stored excerpt is the scrubbed one (`mail_log.excerpt`, run in
`mail_activity.sent`): `email_sends.data` holds at most 200 scrubbed
characters, never the email itself.

**Never included.** The note never contains:
- links or reset URLs;
- tokens or codes;
- more than 200 characters of the body.

The subject, body excerpt and error text are scrubbed:
- URLs become `[link]`;
- any 24+ character token-like run becomes `[redacted]`;
- 6+ digit runs become `[code]`.

Addresses are kept whole and are not run through the token rule, so a long
local part is not redacted.

The queued note is sealed with `encrypt_row`, because it holds the full
recipient address until it is posted.

## Delivery: hook, outbox, cron

1. **Send path.** Each outcome of `_send_email` makes one `INSERT` into
   `mail_log_outbox` through `_mail_log_note`:
   - accepted;
   - rejected, with Mailtrap's status and error;
   - failed with an exception.

   - skipped before any transport ran: the recipient's domain receives no
     mail (`mail_budget.deliverable`, "suppressed"), or no transport is
     configured ("failed"). Until 2026-09-26 these two exits returned
     without a note, so a send the app attempted (for example an onboarding
     feedback mail to an `@next.forkmesh.com` address) never reached
     #email-log. Only a call with no recipient at all is not logged.

     The onboarding feedback mail (`_send_feedback_emails`) checks
     `mail_budget.deliverable` before it sends. An account whose address
     receives no mail gets its `feedback_email_sends` row and leaves the
     queue without a send or a note. Until 2026-09-29 the send ran, came
     back False, and released the claim like a real failure. The
     `feedback-email` phase then retried the account on every run, and each
     retry logged another "suppressed (recipient domain receives no mail)"
     line (fmnext, `admin@next.forkmesh.com`). It also used up one of the
     phase's 4 sends.

   Package mail through `send_mailtrap` does the same, with kind "package",
   including the no-transport exit.
   `_mail_log_note` swallows every exception. A broken log (no table, D1 down,
   no owner) never changes what `_send_email` returns, and no network call is
   added to the send. The host e2e test
   `test_a_broken_mail_log_never_fails_the_send` checks this.
2. **Mailtrap webhook.** `/api/integrations/mailtrap/webhook` is already
   HMAC-verified and deduplicated by `event_id`. It queues a follow-up note
   for `delivery`, `bounce`, `soft bounce`, `spam`, `reject` and
   `suspension`. `open`, `click` and `unsubscribe` are not logged, except
   for task email: its opens, clicks and task views each leave a note
   (`🗂️ Task activity · 👀 opened`, `clicked`,
   `viewed`, [app/task-email.md](app/task-email.md)).
3. **Cron `mail-log` phase, every minute.** It takes up to 40 due notes and
   posts them as chat messages of at most about 7,600 characters each
   (`mail_log.compose`), and at most 20 notes (20 blocks) each. A note is
   several hundred characters, so 40 notes take a few messages.
   - Until 2026-09-28 the notes went into **one** message, and lines past
     the cap were replaced by "… N more lines not shown" and deleted from
     the outbox unposted. With the longer lines that would have lost most
     of a burst.
   - Each message's notes are deleted once it is posted. If a later message
     fails, only the notes not yet posted are retried, so no line is posted
     twice.
   - **Backlog cap.** The outbox is trimmed to the newest 1,000 notes. The
     next message says how many were dropped.
   - **Transport.** The post is the channel incoming-webhook transport: the
     frame is room-encrypted and HMAC-signed
     (`chat-webhook-delivery-v1`) to the room Durable Object, which retains
     and broadcasts it.
   - **Retry.** On failure the notes stay queued and are retried with the
     repository-webhook backoff (`repository_webhooks.backoff_ms`, 30 s
     doubling to 30 min). They are dropped after `MAX_ATTEMPTS` (6).
4. **First run.** The channel is created for the owner on the first drain.
   The owner's clients get a `private-channels` account event, so an open
   chat page picks it up without a reload.

## Mail sent outside the app (any node)

`_send_email` and `send_mailtrap` cover every send the app makes (17 call
sites, audited 2026-09-26). Two more paths can send mail, and both now land in
#email-log:

- **Anything else that uses Postfix on a core** (a cron job, a tool, a
  bounce notice Postfix generates itself). `fmnext-mail-events`
  (`deploy/core/mail_events_agent.py`) already turned every Postfix result
  into a spool event. It now also reads the `qmgr` `from=<…>` line, so each
  event carries the envelope sender (`<>` for a bounce notice) and the node.
  `record_core_mail_event` (`app/src/route_handlers.py`) marks an event
  with no `mail_sends` row as `forkmesh_outside_app`, and the note reads:

      2026-09-26 21:40:02Z · Not sent by the ForkMesh app (no app send record: a tool, a cron job or a bounce), through local mail (Postfix on the core) · on n2 · from root@n2.next.forkmesh.com · to ops@example.org · delivered · msg 20260926214002.1A2B3C@n2.next.forkmesh.com

  The label says the app has no record of sending the message and names the
  server it went through: local mail (the core's own Postfix), Mailtrap, or
  another transport. Until 2026-09-29 it read only "Sent outside the app",
  which did not say which server sent the mail.

  Postfix's log has no subject or body, so these lines have neither. A tool
  that sends through a core's Postfix should leave its own note (below), and
  the two lines share the message id. A `<digits>.<pid>.<digits>@forkmesh.com`
  message id (Python's `make_msgid`) does not prove the app sent it: the Mac
  ad hoc build email uses the same format.
- **The Mac ad hoc build email** (`tools/mac_adhoc_publish.py`,
  `docs/native/MAC_BUILD.md`) pipes the message into `sendmail` on a core over
  SSH, so the app never sees it. On 2026-09-29 its delivery showed up as a
  bare "Sent outside the app" line to `jett@forkmesh.com`, and the operator
  asked what the message said. After `sendmail` returns, the tool now inserts
  a `mail_log_node_notes` row through the same core's rqlite
  (`/etc/forkmesh-next/rqlite.curlrc`) with the subject, the first 400
  characters of the text part, the message id, `via: postfix` and
  `source: the Mac ad hoc build tool`. The note reads:

      📮 Not sent by the ForkMesh app · ✅ accepted · 2026-09-29 06:43:20 UTC
      📨 To: jett@forkmesh.com
      📤 From: no-reply@forkmesh.com
      📝 Subject: Ad hoc iPhone build: ForkMesh 1.4.0 (18) at c3f94cc775e6
      📄 Body: ForkMesh 1.4.0 (18) built on the local Mac from c3f94cc775e6: … Install (open on the registered iPhone): [link] …
      🛤️ Route: local mail (Postfix on the core) on n1
      🧰 Source: sent by the Mac ad hoc build tool
      🆔 Message ID: 179066419708.2521672.2998348115286080844@forkmesh.com

  The row is plain text in rqlite until the next `mail-log` cron run moves it
  into the sealed outbox (at most a minute). It holds the operator's own
  address and the build's public install links. A failed note prints a
  warning and does not stop the publish.

  Before this, such mail showed as a subject-less "Account notification"
  with no node or sender. App mail whose Message-ID the agent missed (it
  restarted between the `cleanup` and `smtp` lines) is also labelled this way,
  so the label means "no app send record", not proof of a foreign sender.
  When the spool is full (5,000 files) the agent now prunes and then writes,
  instead of dropping the new event.
- **IDS direct mail** (`deploy/core/ids.py`, `Alerter.mail`, straight to the
  Mailtrap API, only with `[ids] direct_mail = true`, which is off today).
  Every attempt (accepted, rejected with the HTTP status, or failed) writes a
  note with the recipient masked on the node (`o***@example.org`), shown
  `via Mailtrap API, IDS direct`:
  - on a core, an `INSERT` into `mail_log_node_notes` through the node's
    rqlite (migration 0216);
  - on a runner, a signed `POST /api/runners/mail-log-note` (the same runner
    signature as `security-incident`, 120 notes an hour per runner), which
    inserts the same row with the runner's name as the node.

  `mail_log.node_note` takes `via` only as `ids_direct` or `postfix`
  (anything else becomes `ids_direct`), plus `body`, `messageId` and
  `source`. The runner endpoint copies a fixed set of fields and never
  passes `via`.

  The `mail-log` cron phase moves up to 200 `mail_log_node_notes` rows into
  the sealed outbox (`mail_log.ingest_node_notes`) before each drain. A row
  it cannot read becomes a `FAILED (unreadable node mail note)` line
  instead of disappearing.

Nothing else in the fleet sends mail: the Go daemon, World, desktop, mobile
and native code have no sender. Postfix has no aliases or local delivery, and
there is no cron `MAILTO`, unattended-upgrades mail or fail2ban mail action.
A new sender must call `_send_email`, or write a `mail_log_node_notes` row,
or send through the core Postfix.

Tests: `app/tests/host/test_mail_log_e2e.py`
(`test_mail_sent_outside_the_app_on_any_node_lands_in_email_log` drops a
spool event and a node note into a real host and reads both back out of the
decrypted channel history), `deploy/tests/test_ids.py`,
`deploy/tests/test_mail_events_agent.py`,
`app/tests/test_runner_mail_log_note.py`.

## Configuration

- `MAIL_LOG_OWNER`: the account that owns #email-log. If it is empty or
  names no active user account, nothing is queued, and queued notes wait
  and retry.
- The log is off when `MAILTRAP_API_TOKEN` is unset, because then nothing is
  sent.

## Daily mail budget

The budget exists for Mailtrap's plan limits. Since 2026-09-29 it applies only
when mail goes through Mailtrap: with `MAIL_TRANSPORT = "smtp"` (production),
`_send_email` and platform-token package mail skip both the daily budget and
the Mailtrap daily-limit hold, and their lines carry no budget
(`_mail_budget_text` returns nothing). The operator removed the limit once mail
stopped going through Mailtrap. What still limits local mail: Postfix's 300
messages an hour per client, the IDS alert email cap below, and the marketing
email limit (`MAIL_MARKETING_DAILY_LIMIT`, default 100 a day on every transport,
[app/marketing-email.md](app/marketing-email.md)).

When the daily mail budget (`mail_budget.py`) holds a Mailtrap send, a
`suppressed` note records it:
- in `_send_email`, with the error "daily mail budget spent";
- in `send_mailtrap` (package mail on the platform token), the same way;
- for an IDS alert email held by its daily cap in `ids_alerts.email_gate`,
  with the error "IDS alert email daily cap" and no recipient.

The same-node, same-detector hourly dedupe of IDS mail is not logged, so a
noisy detector cannot flood the channel.

## Admin test email

An administrator (`users.is_admin`) can check outgoing mail from the web
settings page: **Settings → Verification and payout → Admin access → Send test email**
(`/dashboard/settings/payout`).
The button is shown only when the session has `isAdmin`.

- It calls `POST /api/accounts/test-email`
  (`entry._account_admin_test_email`). The server checks `_is_admin` again
  and answers 403 for anyone else. The mail always goes to the signed-in
  account's own address; the request cannot choose a recipient.
- The send uses the kind `admin_test` (Mailtrap category "Administrator
  test"). So it goes through the normal `_send_email` path: the daily budget,
  the `#email-log` note and the Last email stamp on the account.
- One test per minute per account. The limit reads the account's own
  `email_sends` history, the same way the verification resend does.
- The reply says whether the provider accepted the mail, or whether no
  transport is configured (no `MAILTRAP_API_TOKEN` and SMTP off).
Our budget cannot see Mailtrap's own account-wide daily limit, which
production and the served stack share. When Mailtrap rejects a send with
"reached its daily sending limit … retry in XhYmZs", `_send_email` records a
provider hold in `mail_budget` (bucket `mail-provider-hold`, until that retry
time, at most 24 hours, one hour when the reply names no retry time). It logs
`mail held: kind K: mail provider daily limit reached; next send in Ns`
instead of `mailtrap send rejected`, because the limit is an expected, budgeted
outcome and not an error. Until the hold ends, every `_send_email` is skipped
without calling Mailtrap, with a `suppressed` note ("mail provider daily
limit"). Any other rejection still logs `mailtrap send rejected`.

## Finding it

- **Web:** `/chat` on app.next.forkmesh.com. Signed in as the owner,
  **email-log** is in the channel list with a lock icon.
- **Desktop:** the Messages office channels list.
- **iOS and Android:** Chat, under the office channels.
- No other account sees it or can open it by id, admins included.

## Traps

- **Do not give a notices channel members or change its visibility** in
  SQL. Every check assumes creator-only.
- **Changing `MAIL_LOG_OWNER` provisions a new channel** for the new owner.
  The old one stays with the old owner.
- **Notes are posted up to a minute late.** That is the cron batching, not
  a fault.

## Every email is tracked

Since 2026-10-07 every email `_send_email` hands to a transport, of any kind,
gets an `email_sends` row (`mail_activity.sent`, called by
`entry._record_email_send` once Postfix or the mail API accepts it). The
first version of this tracking was written for task email only
(`task_email_sends`, migration 0246); the operator asked for it "to be more
generic for all emails not just task tasks", so migration 0247 moved those
rows into `email_sends` and dropped the task table.

- **Row.** The send id, the kind, the recipient's blind index (of the
  lowercased address, like `mail_sends.recipient_bi`), a `ref` naming what
  the email is about (empty, or `task:<org_bi>:<task_id>`), a sealed
  `{email, subject, body, recipient, messageId}` (`recipient` is the account
  name when the caller passes one; `body` is the scrubbed 200-character
  excerpt every later note about the send repeats), the send time, and the
  first time and a count for opens, clicks and views. Rows go after 90 days
  with the other mail records.
- **Send id.** Every Mailtrap API send now carries `forkmesh_send_id`, not
  only sends to an address that belongs to an account, so Mailtrap's open
  and click webhooks reach the row for any recipient. A caller that needs
  the id before sending (to put it in a link) passes `send_id=`, and may
  pass `ref=` and `recipient=`.
- **Opens and clicks.** `process_mail_event` calls `mail_activity.tracked`
  for every `open` and `click`, from the Postfix pixel and click redirect or
  from a Mailtrap webhook. Each one adds a note, for example
  `🏅 Badge awarded · 👀 opened · <time>` with
  `🔍 Detail: @ada opened the email (2 times)`, the message id of the
  original send and the send's subject and body. What can be opened or
  clicked is unchanged: on core sends only the notification kinds carry the
  pixel and tracked links (see "Gmail spam placement" below), so account
  and security mail records its send and never an open or click.
- **Views.** `mail_activity.view_token(send_id)` is `<send id>.<24 hex>`, an
  HMAC under `DATA_KEY` (`blind_index("mail-view:<send id>")`), separate from
  the pixel and unsubscribe token so an address bar never holds a token that
  can unsubscribe. A page opened from the email's link passes it to
  `mail_activity.viewed(token, ref, viewer)`, which counts only when the
  signature and the row's `ref` match. Only the Tasks page does this so far
  ([app/task-email.md](app/task-email.md)). View tokens issued by the
  task-only version (`task-email-view:` scope) no longer verify; the email
  link still opens the task.
- **Followers.** `mail_activity.FOLLOWERS` maps a kind to a module that also
  hears each open, click and view (`mail_event(env, ref, event, who,
  subject, first, now)`), such as `task` → `world_task_email`, which writes
  the first of each to the task timeline. A follower that fails is printed
  and never stops the note.
- **Per-kind limits.** `mail_activity.sent_since(kind, email, since)` counts
  a recipient's recent sends of one kind; task email uses it for its hourly
  cap.

## Package mail and the tracked footer

On 2026-10-08 the operator asked for a note in #email-log whenever an email
is opened, tracked per user and per CRM contact, including mail sent from
Zoho Mail, "with a footer with the logo".

- **Package mail on the cores was never tracked.** `_send_package_smtp`
  sent the package's HTML untouched (no pixel), wrote `mail_sends` under one
  random id and `email_sends` under another, and dropped the package's
  `custom_variables`, so a CRM email sent through Postfix never recorded an
  open and the CRM never heard of its delivery. Now one send id is used for
  both rows, and the HTML (or the text part turned into HTML, `text_html`)
  ends with a logo footer (`mail_activity.add_footer`) whose image is
  `/api/mail/t/l/<token>.png`. Links are not rewritten through the click
  redirect: a redirector in person-to-person mail is what hurt Gmail
  placement (see "Gmail spam placement"), and only opens were asked for. That URL records an open exactly like the
  `o` pixel, then answers `302` to `assets/logo-96.png`, so the logo shows
  and no 1x1 pixel is added. A bad token still redirects to the logo.
- **Package variables follow the send.** `mail_activity.sent` keeps the
  package's `forkmesh_*` custom variables (up to 8, `clean_variables`) and a
  `forkmesh_sender` account name in the sealed row. `record_core_mail_event`
  merges them back into `custom_variables` (`send_variables`), so the
  package's `mailtrap_webhook_event` hook hears Postfix deliveries, bounces,
  opens and clicks the way it hears Mailtrap's webhook. On the Mailtrap
  transport, `send_mailtrap` now adds `forkmesh_send_id` to the package's
  variables and records `email_sends` under it, so Mailtrap's open webhook
  reaches the row too.
- **Per user.** The open note's detail names the sender:
  `The recipient opened the email from @alice`. The CRM sets
  `forkmesh_sender` on one-off sends and outreach. The name is the
  package's claim, shown only in the owner-only channel.
- **Per CRM contact.** The CRM's hook sets the send's status to `open` or
  `click`, which the CRM shows on the contact's email history. A later
  `accepted`, `delivery` or `soft bounce` event no longer overwrites an
  open or click, and an open no longer overwrites a click
  (`marketplace/packages/crm/README.md`, 1.25.0).
- **The tracked footer for your own mail app.** Mail written in Zoho Mail
  never passes through ForkMesh, so the only way to see it opened is an
  image in the signature. `GET /api/mail/footer` (signed in) creates one
  `email_footers` row per account (`owner_bi` is the account's blind
  index; the sealed data holds the account name and email) and returns the
  footer HTML, its image URL `/api/mail/t/f/<footer id>.<24 hex>.png` (an
  HMAC under `DATA_KEY`, scope `mail-footer:`), a `previewHtml` that uses
  the plain logo so viewing Settings does not count as an open, and the
  open count. Each image load within a minute of the last counts once,
  bumps the row and posts
  `🖼️ Own mail (tracked footer) · 👀 opened` with
  `🔍 Detail: @alice's tracked footer was opened in an email sent
  from alice@example.com (3 opens so far)`, then redirects to the logo.
  - A `DATA_KEY` re-key keeps the row and its count but changes the
    image signature: a footer already pasted into a mail signature stops
    counting until it is copied again from Settings (`docs/REKEY.md` 1.3).
  - The footer is the same in every message, so it cannot tell which
    email or which recipient: the note has no To, subject or message id.
    Per-recipient tracking needs mail sent through ForkMesh (the CRM's
    Send).
  - The sender opening their own Sent folder loads the image too and
    counts. Clients that block remote images never count; Gmail's image
    proxy loads it once per open.
  - Footers are not deleted with the 90-day mail records, because a
    signature is pasted once and must keep working. They go with the
    account's data.
  - `www/public/privacy.html` ("Email we send") describes both footers.

## Sending from the cores

Vultr lifted the port-25 block on 2026-09-25 (it takes effect after a full halt and start from the API). Mail now leaves from the cores themselves instead of Mailtrap.

- **Transport.** With `MAIL_TRANSPORT = "smtp"` (`[vars]` in `app/wrangler.toml`, read by the host), `_send_email` and platform-token package mail hand the message to Postfix on `127.0.0.1:25` (`app/src/smtp_mail.py`). A non-loopback `MAIL_SMTP_HOST` is refused. No-receive domains and the `#email-log` note are unchanged, and the daily budget no longer applies (see "Daily mail budget"): every accepted, failed or suppressed send is logged to the channel. Package mail always goes out as `no-reply@forkmesh.com`, one recipient per message.
- **Postfix** is send-only: `inet_interfaces = loopback-only`, no local delivery, relay only from `mynetworks` (loopback), 300 messages an hour per client, TLS 1.2+ whenever the receiving server offers it.
- **DKIM.** OpenDKIM signs `forkmesh.com` with selector `fmnext` over a unix socket inside the Postfix spool (no TCP listener). The key is generated on the desktop (`config/fleet/dkim/fmnext.private`, 0600) and piped over SSH stdin to each core; `milter_default_action = tempfail`, so unsigned mail never leaves.
- **Identity.** Each core's hostname, HELO, A/AAAA and PTR (IPv4 and IPv6) are `nN.next.forkmesh.com`; cloud-init keeps the hostname (`preserve_hostname`). `/etc/hosts` maps `nN.next.forkmesh.com` to the core's public IPv4, not `127.0.1.1`: with the loopback line, the app's endpoint check on that core resolved the core's own name to a non-public address and refused its lease (`invalid_manifest`, 2026-09-26). The install step deletes any old `127.0.1.1` line for the name. SPF on `forkmesh.com` lists the cores' addresses and keeps `include:zohomail.com`. DMARC is unchanged.
- **IDS.** TCP 25 is an expected listener on the cores; the install opens a 30-minute expected-change window for the new `postfix`/`opendkim` users and files.
- **Delivery tracking** replaces Mailtrap's webhook with the same event names and the same processing (`process_mail_event` in `app/src/route_handlers.py`: dedupe in `mailtrap_webhook_events`, `mailtrap_email_sends` status, plugin hooks, `#email-log` follow-up notes):
  - *delivery*, *soft bounce* (first deferral per recipient) and *bounce* come from Postfix's own log. `fmnext-mail-events` (user `fmnext-mail`, journal read only, no network) follows the `postfix/*` journal on each core and writes one event file per result into `/var/lib/fmnext-mail-spool` (group `forkmesh-app`, 0640). The app reads them in order every 15 s (`app/host/forkmesh_host/mail_events.py`) and keeps its place in `mail-events.cursor` in its data dir; the agent prunes files after 3 days.
  - On the notification kinds only (`mail_tracking.UNSUBSCRIBE_KINDS`; account and security mail is not tracked since 2026-10-06), *open* is a 1x1 pixel, *click* a redirect through `/api/mail/t/c/<token>`, and *unsubscribe* a `List-Unsubscribe` one-click POST (notification-type kinds only). Tokens are the send id plus an HMAC under the blind-index key, and each click target carries its own HMAC, so the redirect is never open. The clicked URL is not stored.
  - `mail_sends` (migration 0212) maps a send id to its Message-ID, kind and blind indexes; rows expire after 90 days.
  - Retention: `prune_mail_records` (`app/src/route_handlers.py`) deletes `mail_sends`, `mailtrap_email_sends`, `email_sends` and `mailtrap_webhook_events` rows older than 90 days. It runs after every accepted core send (`_send_email_smtp`), every core event (`record_core_mail_event`: pixel, click, unsubscribe and the Postfix spool) and every Mailtrap webhook batch. Until 2026-09-30 only the Mailtrap webhook pruned the two `mailtrap_*` tables, so with `MAIL_TRANSPORT = "smtp"` per-account open and click history was kept forever (issue #732). `app/tests/test_mail_record_retention.py` covers the SMTP path.
  - Disclosure: `www/public/privacy.html` (**Email we send**, 2026-09-30) tells readers about the open pixel, the click redirect, the 90-day deletion and that plain-text parts carry no tracking. Change that section with any change to what tracking records or how long it is kept. It does not claim the pixel or redirect requests go unlogged: Caddy access logs on the cores see them like any other request.
  - *bounce*, *reject* and *unsubscribe* add the recipient's blind index to `mail_suppressions`; later sends to it are logged as suppressed. An unsubscribe blocks only the notification kinds, never password resets or verification. An unsubscribe from a `marketing` email is stored as `marketing_unsubscribe` and blocks only marketing mail, while every other reason also blocks marketing mail ([app/marketing-email.md](app/marketing-email.md)).
  - Not covered: spam complaints (no feedback loop from the big providers) and bounces that arrive later by email, which reach the Zoho mailbox of the envelope sender.

- **/status.** With `MAIL_TRANSPORT = "smtp"` the email row checks the core mail service instead of Mailtrap: Postfix must answer an SMTP NOOP on loopback, and the most recent send (last 24 h) must not be a bounce or reject, still deferred, or without any delivery event after 30 minutes (`_core_mail_status` in `app/src/status_monitoring.py`; the delivery state is `mail_sends.status`, migration 0214). A send records `status = 'accepted'` when it is written, and the check reads only rows that carry a status. Rows written before migration 0214 have an empty status and no recorded delivery, so on 2026-09-25 they turned the row red for a delivered email ("no delivery event after 30 minutes") until this was fixed.
  - Since 2026-10-02 each send also keeps the DSN of its latest Postfix result (`mail_sends.dsn`, migration 0229; only the code, such as `4.4.3`, never Postfix's reason text, which can quote the address). A failing row names the cause from the DSN, for example "still deferred after 30 minutes: looking up the recipient's mail server in DNS failed (4.4.3)". A bounce because the address does not exist (5.1.1, 5.1.2, 5.1.3, 5.1.6, 5.1.10, 5.2.1) passes: it is the recipient's problem, and the address is already suppressed. Every other bounce, and any deferral older than 30 minutes, still fails. With the SMTP transport the row's description, check list and alert guidance describe Postfix, not Mailtrap (`STATUS_EMAIL_SMTP_*`).
  - The 2026-10-02 outage was real, not a false alarm: Vultr's resolvers (108.61.10.10, 108.61.115.10, the cores' `/etc/resolv.conf`) answered SERVFAIL for `forkmesh.com`, `zoho.com` and `mx.zoho.com` (a DNSSEC validation failure on their side; the same query with `+cd` answers), so a 06:51 UTC mail to a `forkmesh.com` address was deferred with 4.4.3 on every retry. Postfix in its chroot reads a copy of that `resolv.conf`.

### Gmail spam placement (2026-10-06)

Gmail accepted every core send (`250 2.0.0 OK`) but filed it as spam. SPF,
DKIM (`fmnext`, matching the private key) and DMARC were in place, and no core
address was on Spamhaus, SpamCop, Barracuda, SORBS or PSBL. What changed:

- **Reverse DNS on n3.** The IPv4 PTR of n3 (`203.0.113.203`) still answered
  Vultr's generic `203-0-113-203.constant.com`, although the Vultr API listed
  `n3.next.forkmesh.com`. The HELO name and the PTR did not match, which Gmail
  scores against the sender. `check nN` now prints a forward-confirmed PTR line
  for both addresses (`ptr_status`): the PTR must be `nN.next.forkmesh.com` and
  that name must resolve back to the address.
- **Postfix prefers IPv4** (`smtp_address_preference = ipv4`). Gmail is
  stricter with IPv6 senders, and a new IPv6 /64 has no reputation. Postfix
  still falls back to IPv6 when IPv4 fails.
- **DKIM covers the one-click unsubscribe.** RFC 8058 and Gmail's bulk sender
  rules require `List-Unsubscribe` and `List-Unsubscribe-Post` to be signed.
  OpenDKIM's default header list predates RFC 8058 and left
  `List-Unsubscribe-Post` out, so the one-click header was unsigned. The
  config now names the signed headers (`SIGN_HEADERS`) and oversigns From,
  Subject, the unsubscribe pair and the other identity headers
  (`OVERSIGN_HEADERS`), so no copy can be added after signing.
  Canonicalization is `relaxed/relaxed`: with `simple` body canonicalization,
  any whitespace change on the way broke the signature.
- **No tracking on account and security mail.** Verification, password reset,
  password change, admin test, status and security alerts went out with every
  link rewritten to `app.forkmesh.com/api/mail/t/c/<token>?u=<full URL>`
  and a hidden pixel. A confirmation button that points at a redirector
  carrying another URL is what phishing mail looks like. Only the kinds that
  carry an unsubscribe header (`mail_tracking.UNSUBSCRIBE_KINDS`) are tracked
  now, and the pixel is no longer `display:none`.
- **Headers.** Every core send carries `Auto-Submitted: auto-generated` and
  `Feedback-ID: <kind>:forkmesh`, so Google Postmaster Tools' spam rate splits
  by kind. Only the top-level part carries `MIME-Version`; `EmailMessage`
  had added a second one to the HTML part.

Not done: MTA-STS and TLS-RPT protect mail *to* `forkmesh.com` (Zoho) and
do not affect where Gmail files our mail. Gmail and Apple Mail will not show
the BIMI logo without a mark certificate (see below). Register `forkmesh.com`
in Google Postmaster Tools to watch domain reputation and the spam rate; it
must stay under 0.3%.

Operating it (`tools/next_mail.py`):

1. `dkim --execute`, `dns --execute`, `rdns --execute` (one time).
2. Per core, one at a time: `install nN --execute`, then `restart nN --execute` (drain, Vultr halt, start, unseal, undrain).
3. `check nN` shows hostname, services, the loopback listener, whether outbound 25 is open, and the queue.
4. Deploy the app (`next_deploy.py all --execute`); `MAIL_TRANSPORT` ships with the release.

## The email header logo

Every email ForkMesh builds goes through `_forkmesh_email_card_html` in
`app/src/entry.py`, and that card now opens with the ForkMesh logo next to the
name. Plugin mail sent through `send_mailtrap` supplies its own HTML and does
not get the header; on the cores it ends with the tracked logo footer instead
(see "Package mail and the tracked footer").

- **Linked, not embedded.** The image is
  `https://app.forkmesh.com/assets/logo-96.png`, written into the card itself
  because tests run slices of `entry.py` that skip module constants (80x96, about 13 KB, shown at
  25x30). Gmail and Outlook drop inline SVG and `data:` images, and a CID
  attachment would have to be added to both the Mailtrap and the SMTP path. The
  full `/assets/logo.png` is about 720 KB, too heavy for mail. The file is
  served from `app.forkmesh.com` because `forkmesh.com` did not have
  `logo-96.png` yet when this was added.
- **Dark badge.** The logo is white, so it sits in a `#18181b` cell. On the
  white card it would otherwise disappear. `_light_email_fragment` has no rule
  for `#18181b`, so the badge stays dark.
- **Blocked images.** The `<img>` has `alt=""` and the word "ForkMesh" is real
  text in the next cell, so a client that blocks images still shows the name
  without a broken-image label.
- Mail tracking rewrites only `href` links, so the logo URL is left alone.

## The inbox avatar (BIMI)

The round picture beside the sender in an inbox list does not come from the
message. Clients that support it take it from BIMI: a DNS record that points at
a logo, checked only when the sending domain enforces DMARC.

- **The logo.** `www/public/assets/forkmesh-bimi.svg`, served at
  `https://app.forkmesh.com/assets/forkmesh-bimi.svg`. BIMI accepts only SVG
  Tiny Portable/Secure: `version="1.2" baseProfile="tiny-ps"`, a `<title>`, a
  square viewBox, no `x`/`y` on the root, no scripts, links, styles or embedded
  images, under 32 KB. It is the white mark from `assets/logo.png`, traced to
  flat paths, on a solid `#090909` square (the site's theme colour), sized to
  fit inside the circle clients crop it to.
  `test_bimi_logo_is_square_tiny_ps_svg` checks those rules.
- **DMARC first.** BIMI is ignored under `p=none`. It needs `p=quarantine`
  or `p=reject`, with no `pct` below 100. On 2026-09-27
  `_dmarc.forkmesh.com` went from `p=none` to
  `v=DMARC1; p=quarantine; rua=mailto:rua@dmarc.brevo.com`: quarantine
  rather than reject, so a sender that turns out to be unaligned lands in
  spam instead of bouncing. Every sender for `forkmesh.com` signs with DKIM:
  Mailtrap (`rwmt1`, `rwmt2`), the cores (`fmnext`), Zoho (`zmail`) and Brevo
  (`brevo1`, `brevo2`). Watch the Brevo DMARC reports (`rua`) for unaligned
  mail; add any new sender's DKIM key before it sends.
- **The record.** TXT `default._bimi.forkmesh.com`:
  `v=BIMI1; l=https://app.forkmesh.com/assets/forkmesh-bimi.svg; a=`
  `tools/next_dns.py` manages only A and AAAA records, so this TXT record and the
  DMARC change are made by hand in Cloudflare.
- **Which clients show it.** With only the record: Yahoo, AOL, Fastmail and
  others that accept self-asserted BIMI (Yahoo also wants enough volume and a
  good reputation). Gmail and Apple Mail also need a Verified Mark Certificate
  (for a registered trademark) or a Common Mark Certificate, bought from a mark
  certificate authority, and its PEM URL goes in `a=`. Outlook does not use
  BIMI.
