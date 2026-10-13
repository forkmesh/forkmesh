# Marketing email: product updates and offers

Added 2026-10-07 (branch `1746-also-add-marketing-email`). The operator asked
for "a marketing email channel and plan to email our users with updates and any
campaign / promotion we may have". The plan for what to send and when is in the
[marketing plan](../marketing/plan.md) ("Email channel"). This page covers the
code.

Until this change ForkMesh had no lawful way to email its accounts about
anything but their own activity. Every account email was transactional or a
notification the person had asked for, and the plan's E1 item noted that no
opt-in field existed.

## Code map

| Piece | Where |
| --- | --- |
| Consent rules, campaign validation, the email body and footer, unsubscribe link shape | `app/src/marketing_email.py` (pure, imported by tests directly) |
| Cron drain, admin console section, signed unsubscribe page, consent withdrawal | `app/src/marketing_email_runtime.py` (lazy module, `_bind_runtime`) |
| Settings switch read and write | `ping_settings_payload` / `_apply_ping_settings` in `app/src/ping_settings.py` (`marketingEmail`, `marketingEmailSince`) |
| Signup checkbox | `app/public/signup.html` (`#updates-agree`), `app/public/signup.js`, the signup handler in `app/src/entry.py` |
| Web settings card | `app/public/dashboard/partials/views/settings.html` (`#marketing-email`), `app/public/dashboard/js/04-account.js` (`renderMarketingEmail`) |
| Admin console | `?view=marketing-email` on the secret console path, "Growth → Marketing email" in `admin_shell.NAV` |
| Tables | `marketing_email_campaigns`, `marketing_email_sends` (migration `0245_marketing_email.sql`) |
| Cron phase | `marketing-email`, every ten minutes at residue 9 (`_cron_due_phases`, `cron_activity.CRON_PHASE_CATALOG`) |
| Tests | `app/tests/test_marketing_email.py`, plus the payload checks in `test_ping_settings.py` |

## Consent

- The account record holds `marketing_email: {"at": <ms>, "source":
  "signup"|"settings"}` while the person is subscribed. The key is absent
  otherwise, so every existing account starts unsubscribed. That is the point:
  nobody gets marketing mail because they once signed up.
- The record keeps when and where the person agreed, which is the evidence a
  consent-based list needs (GDPR art. 7(1)). Turning it off deletes the key.
- The signup checkbox is unchecked by default and separate from the Terms
  checkbox. Bundling consent into the Terms, or pre-ticking it, is not valid
  consent in the EU or UK.
- The switch is independent of "Email me about pings". Turning ping email off
  does not unsubscribe from updates, and the reverse is also true.
- An account receives a campaign only when it is opted in, `status` is
  `active`, `kind` is `user`, and its email is verified (`me.subscribed`).

## Unsubscribing

There are three ways out, and each one only stops marketing mail:

1. **The footer link.** `/api/marketing-email/unsubscribe?a=<name>&s=<sig>`.
   `sig` is the first 32 hex characters of `blind_index("marketing-unsub:" +
   name)`, so it cannot be forged and needs no stored token. A GET shows a
   confirm button; the POST deletes the consent. It works the same on both
   transports.
2. **The one-click header.** `marketing` is in
   `mail_tracking.UNSUBSCRIBE_KINDS`, so core (Postfix) sends carry
   `List-Unsubscribe` and `List-Unsubscribe-Post`, as Gmail and Yahoo require
   for bulk senders. `record_core_mail_event` turns an unsubscribe on a
   `marketing` send into the suppression reason `marketing_unsubscribe`
   instead of `unsubscribe`.
3. **The settings switch.**

`_mail_suppressed` applies the reasons like this:

- `marketing_unsubscribe` blocks only `marketing` mail.
- Every other reason (bounce, spam, reject, a notification unsubscribe) also
  blocks `marketing` mail.
- Before this change, adding a kind to `UNSUBSCRIBE_KINDS` meant that
  unsubscribing from it stopped every notification email as well. Marketing
  must never cost someone their ping or security mail.

A stronger reason later replaces `marketing_unsubscribe`; the other reasons
keep `INSERT OR IGNORE` semantics. An unsubscribe or spam event on a
`marketing` send also deletes the account's consent (`process_mail_event`
calls `withdraw`), so the admin's subscriber count drops at once. Turning the
switch back on deletes a `marketing_unsubscribe` suppression for that address.

## Sending

1. An administrator writes a draft in the console: Product update or
   Promotion, a subject, a plain-text message (a blank line starts a
   paragraph), and an optional `https://` button link and label. The content is
   stored with `encrypt_row` (`next_rekey` `CIPHER_COLUMNS`).
2. **Send test to me** mails the campaign to the administrator's own verified
   address, with `[Test]` in front of the subject.
3. **Start sending** needs the confirmation box ticked. It refuses while
   another campaign is still sending, so opens and clicks can be attributed by
   time window. It also refuses while `MARKETING_EMAIL_POSTAL` is
   unset: CAN-SPAM requires a valid physical postal address in every
   commercial email.
4. The `marketing-email` cron phase sends up to `BATCH` (2) emails per run,
   about 12 an hour. It walks `users` by `rowid` from the campaign's
   `cursor_rowid`, reading 100 rows per run. `marketing_email_sends` is
   claimed before each send, so a crash or a retried tick never sends twice.
   The run stops for the day when `MAIL_MARKETING_DAILY_LIMIT` (default 100,
   bucket `mail-marketing:<day>`) is spent or Mailtrap holds sends. The claim
   is released when the limit stops a send, so that recipient goes first the
   next day. When a run ends short of 100 rows, the campaign is `sent`.
5. Each email goes through `_send_email(email_kind="marketing")`. It gets the
   usual card, a footer with the reason it was sent, the unsubscribe and
   settings links, and the postal address, plus open and click tracking like
   notification mail. On the Mailtrap transport, `bulk: true` sends it to
   Mailtrap's Bulk stream (`MAILTRAP_BULK_API_URL`, default
   `https://bulk.api.mailtrap.io/api/send`), because Mailtrap does not allow
   marketing mail on the transactional stream. On the Mailtrap transport the
   general bulk share of the daily budget also applies. Postfix has no budget
   (`MAIL_LOG.md`), so the marketing limit is what paces it.

The cost of one send is a claim, a budget reservation, `_send_email` and a
fresh account read and save for the "last email" stamp. That is about 15
subrequests, so two sends fit the 45-subrequest phase budget with room to
spare.

## Results

The console shows, per campaign, the sent, not sent (the transport refused or
the address is suppressed) and skipped (a domain that receives no mail)
counts. It also shows opened, clicked, unsubscribed, complaints and bounced,
read from `mailtrap_email_sends` rows of kind `marketing` between the
campaign's start and finish. That table is pruned after 90 days, and test
sends made while a campaign is sending are counted with it. The subscriber
count decrypts every `users` row. That is fine at the current size, but it is
not something to put on a public page.

UTM values for campaign links are allowlisted in `app/wrangler.toml`: source
`email`, medium `email`, and campaigns `product_update` and `promotion`.

## Configuration

| Variable | Meaning |
| --- | --- |
| `MARKETING_EMAIL_POSTAL` | Required before any campaign sends. Printed in every footer |
| `MAIL_MARKETING_DAILY_LIMIT` | Marketing emails per UTC day, default 100; 0 stops sending |
| `MAILTRAP_BULK_API_URL` | Override for the Mailtrap Bulk stream endpoint |

## Not done

- The desktop and the phone apps have no switch yet. Their settings pages read
  the same account payload, so they can add one without a server change.
- There is no segmenting. A campaign goes to every subscriber.
- Opens and clicks are not split per campaign beyond the time window.
- There is no double opt-in. The address is already verified at signup, which
  is the same evidence a confirmation email would give.
