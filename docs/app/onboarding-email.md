# Onboarding email: first repo, day 3, day 7

Until now the only email a new account gets is the verification email
(`_send_verification_email` in `app/src/entry.py`). Three getting-started
emails are drafted as templates. **They are not wired to send yet**: nothing
calls them, no cron phase runs them, and no account receives them.

## Code map

| Piece | Where |
| --- | --- |
| Templates (pure: subject, preheader, plain text, HTML fragments, footer) | `app/src/onboarding_email.py` |
| Card that wraps the fragments (same card as the verification email) | `_forkmesh_email_card_html` in `app/src/entry.py`, which now takes an optional `preheader` |
| Tests | `app/tests/test_onboarding_email.py` |

`onboarding_email.message(step, name, repo="", has_repo=True,
unsubscribe="", settings_url=SETTINGS_URL)` returns a dict with `subject`,
`preheader`, `text` (with footer), `heading`, `intro_html`, `body_html`,
`footer_html`, `action_label` and `action_url`. The HTML pieces use the dark
inline colours and `fm-*` classes of the verification email, so
`_light_email_fragment` turns them into the same light card. Hand them to the
card as:

```python
html = _forkmesh_email_card_html(
    msg["heading"], msg["intro_html"], msg["body_html"], msg["footer_html"],
    msg["action_label"], msg["action_url"], preheader=msg["preheader"])
```

The preheader is a hidden element placed before the logo, so it is the first
text an inbox shows next to the subject.

## The three emails

| Step | Subject | Preheader | Main link |
| --- | --- | --- | --- |
| `first_repo` | Your first repository is on ForkMesh | Next: give owner/repo a second copy on a mirror. | `/dashboard/repos` |
| `day_3` | Give your code a second home | Add a mirror, take the host offline, clone again. | `/desktop` (or `/dashboard/repos/new` with `has_repo=False`) |
| `day_7` | Your first week on ForkMesh | Signed reviews, AI agents, desktop CI and your phone. | `/features` |

- **First repo**: the clone URL `https://forkmesh.com/<owner>/<repo>`, then
  next steps: add a mirror (desktop app or `install.sh` on a Linux machine),
  open an issue or invite a collaborator, read `/docs/onboarding`.
- **Day 3**: what a mirror is and the product-promise walk-through: install on
  a second machine, add the repository as a mirror, take the first host
  offline, clone again (`/docs/product-promise`). With `has_repo=False` it
  asks for a first repository instead.
- **Day 7**: what else every repository gets: signed collaboration, AI agents,
  desktop CI, the phone apps, the Marketplace.

Every email asks for feedback at `founders@forkmesh.com`, like the
verification email, and makes only claims the website already makes. The
repository name is used only when it matches `owner/name`; anything else falls
back to "Your repository" with no clone line. Names are HTML-escaped.

Footer: "You get this because you created a ForkMesh account. It is one of
three getting-started emails." plus a **Stop getting-started emails** link
(when an unsubscribe URL is passed) and **Email settings**.

## Enabling sending (needs decisions)

Wiring is held back because it needs product and legal decisions:

1. **Consent basis.** Are these account mail (sent to every verified
   account, with an opt-out) or marketing mail (sent only to accounts that
   ticked "Product updates and offers", see
   [marketing-email.md](marketing-email.md))? Signup leaves that box
   unchecked, so the marketing route would reach few new users; the account
   route needs a reviewed opt-out and a decision on whether a postal address
   (`MARKETING_EMAIL_POSTAL`) is printed.
2. **Clock.** Whether day 3 and day 7 count from signup or from email
   verification, whether day 3 is skipped or changed for accounts that already
   added a mirror, and whether unverified accounts get anything.
3. **Budget.** Which `mail_budget` class the kind `onboarding` belongs to
   (account reserve or the marketing daily limit).

Once decided, the work is:

- **Preference.** An `onboardingEmail` switch in `app/src/ping_settings.py`
  (default on for the account route, off for the marketing route), shown in
  the notifications settings view.
- **Unsubscribe.** A signed `/api/onboarding-email/unsubscribe?a=<name>&s=<sig>`
  route copied from the marketing one (GET confirms, POST turns the switch
  off), `onboarding` added to `mail_tracking.UNSUBSCRIBE_KINDS` for the
  one-click `List-Unsubscribe` headers, and any existing suppression
  respected.
- **First repo.** Queue `first_repo` where the attribution event
  `first_mirror_created` is emitted: `catalog_routes.py` (the published
  catalog path) and `_emit_first_repository` in `hosted_git.py` (hosted
  repositories). Queue, do not send inline, so a slow mail provider never
  delays a push.
- **Day 3 and day 7.** A cron phase `onboarding-email` in
  `app/src/cron_activity.py` next to `marketing-email`
  ([cron-and-durable-objects.md](cron-and-durable-objects.md)) that walks
  accounts by `rowid`, finds those whose clock passed 3 or 7 days, claims a
  row per account and step in an `onboarding_email_sends` table before
  sending (so a retry never sends twice), and sends a few per run through
  `_send_email(email_kind="onboarding")`.
- **Tests** for the preference, the unsubscribe signature, the
  once-per-step claim and the phase cadence.

Until then, Marketing can paste these texts into a marketing campaign
(`?view=marketing-email`) for opted-in accounts.
