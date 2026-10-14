# Marketing email: product updates and offers

An opt-in email channel for product updates and promotions. What to send and
when is in the [marketing plan](../marketing/plan.md).

## Code map

| Piece | Where |
| --- | --- |
| Consent rules, campaign validation, body, footer, unsubscribe link | `app/src/marketing_email.py` (pure) |
| Cron drain, admin console section, unsubscribe page, consent withdrawal | `app/src/marketing_email_runtime.py` |
| Settings switch | `app/src/ping_settings.py` (`marketingEmail`, `marketingEmailSince`) |
| Signup checkbox | `app/public/signup.html` (`#updates-agree`), `signup.js` |
| Settings card | `#marketing-email` in the settings view, `renderMarketingEmail` |
| Admin console | `?view=marketing-email`, "Growth → Marketing email" |
| Tables | `marketing_email_campaigns`, `marketing_email_sends` |
| Cron phase | `marketing-email`, every ten minutes |
| Tests | `app/tests/test_marketing_email.py`, `test_ping_settings.py` |

## Consent

- The account record holds `marketing_email: {"at": <ms>, "source":
  "signup"|"settings"}` only while subscribed; every account starts
  unsubscribed. The record is the evidence of when and where consent was
  given; opting out deletes it.
- The signup checkbox is unchecked by default and separate from the Terms.
- Independent of ping email settings in both directions.
- A campaign goes only to opted-in, active, `user`-kind accounts with a
  verified email.

## Unsubscribing

1. Footer link `/api/marketing-email/unsubscribe?a=<name>&s=<sig>`, where
   `sig` is the first 32 hex characters of
   `blind_index("marketing-unsub:" + name)`. GET confirms, POST deletes the
   consent.
2. One-click `List-Unsubscribe` / `List-Unsubscribe-Post` headers
   (`marketing` is in `mail_tracking.UNSUBSCRIBE_KINDS`); such an unsubscribe
   records the suppression reason `marketing_unsubscribe`.
3. The settings switch.

`marketing_unsubscribe` blocks only marketing mail; every other suppression
reason (bounce, spam, notification unsubscribe) also blocks marketing.
Marketing opt-out never stops notification or security mail. An unsubscribe
or spam event on a marketing send also withdraws consent.

## Sending

1. An administrator drafts a campaign (Product update or Promotion, subject,
   plain-text body, optional `https://` button); content is stored encrypted.
2. **Send test to me** mails it to the admin with `[Test]` in the subject.
3. **Start sending** needs confirmation, refuses while another campaign is
   sending, and refuses while `MARKETING_EMAIL_POSTAL` is unset (commercial
   email must carry a postal address).
4. The cron phase sends up to `BATCH` (2) per run, walking `users` by `rowid`
   from `cursor_rowid`. Each send is claimed in `marketing_email_sends` first,
   so retries never send twice. `MAIL_MARKETING_DAILY_LIMIT` paces the
   channel.
5. Mail goes through `_send_email(email_kind="marketing")` with footer,
   unsubscribe link, postal address and open/click tracking. On the Mailtrap
   transport it uses the Bulk stream (`MAILTRAP_BULK_API_URL`).

## Results

Per campaign: sent, not sent, skipped, opened, clicked, unsubscribed,
complaints and bounced (from `mailtrap_email_sends` rows of kind `marketing`
within the campaign's time window). UTM values are allowlisted: source
`email`, medium `email`, campaigns `product_update` and `promotion`.

## Configuration

| Variable | Meaning |
| --- | --- |
| `MARKETING_EMAIL_POSTAL` | Required before any campaign sends; printed in every footer |
| `MAIL_MARKETING_DAILY_LIMIT` | Marketing emails per UTC day, default 100; 0 stops sending |
| `MAILTRAP_BULK_API_URL` | Override for the Mailtrap Bulk stream endpoint |

## Limits

No segmenting, no double opt-in (the address is verified at signup), and no
switch in the desktop or phone apps yet (they read the same account payload).
