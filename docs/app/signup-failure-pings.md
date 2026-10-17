# Signup failure pings

Every failed attempt on `/signup` sends a Ping to each platform administrator
(kind `operational_alert`, title `Signup failed: <reason>`). The Ping names
the username when the visitor typed a valid one and never includes the email
address.

## Browser side

- `api.js` loads with `render=explicit&onload=forkmeshSignupChallengeReady`;
  `signup.js` renders the Turnstile widget itself and passes callbacks as
  functions, so script load order does not matter. Site key, action and cdata
  come from `data-*` attributes filled in by the server.
- On submit with no callback token, `widgetToken()` reads it with
  `turnstile.getResponse` (unless `turnstile.isExpired`).
- Tokens expire after 300 s and background tabs pause the widget. A token
  older than 240 s is reset on submit, waiting up to 30 s for a fresh one;
  a tab becoming visible with a stale token resets the widget.
- Turnstile `3xxxxx`/`6xxxxx` errors are retried by the widget; after
  `CHALLENGE_RETRY_ERRORS` (3) errors, or on a non-retryable code, the page
  switches to the backup check below instead of stopping.
- When the page cannot submit, or a request fails before the server answers,
  it reports to `POST /api/accounts/signup/problem`: `challenge_token_missing`
  (first submit without a token), `challenge_backup_failed`,
  `network_error`, `signup_page_renew_failed`, or `unexpected_response`.
  The server still accepts `challenge_unavailable`, `challenge_not_rendered`,
  `challenge_render_failed`, `challenge_stale` and `challenge_error` from
  pages loaded before the backup check shipped.

## Backup check

A visitor whose browser cannot produce a Turnstile token used to be stuck.
The Ping of 2026-10-17 was `challenge_error [200100:1]`: Turnstile's "the
visitor's clock is incorrect" code, which no retry or reload fixes. Blocked
`api.js` (ad blockers, corporate filters), iframe load errors and a
Cloudflare outage stopped visitors the same way.

- The page switches to the backup check when the widget reports a
  non-retryable error or exhausts its retries, `render` throws, `api.js` has
  not loaded after `CHALLENGE_LOAD_WAIT_MS` (10 s) or at submit, a stale
  token cannot be refreshed, a second submit still has no token, or the
  server answers `turnstile_unavailable` (one resubmit).
- It removes and hides the widget and finds a nonce such that
  `sha256("forkmesh-signup-backup-v1:" + cdata + ":" + nonce)` starts with
  `BACKUP_BITS` (18) zero bits: about 260,000 hashes, under a second on a
  laptop and a few seconds on a slow phone, run in 20,000-hash slices in the
  background. The cdata is the page's Turnstile cdata, so a proof is bound
  to one one-use signup claim; a renewed claim gets a fresh proof and no new
  widget.
- The signup body carries `backupProof: {nonce, reason}` with an empty
  `turnstileToken`. `reason` is `e<code>` for a widget error, or
  `unavailable`, `render_<error>`, `stale`, `not_rendered`, `token_missing`,
  `server_unavailable`.
- `_signup_backup_gate` (instead of `_turnstile_gate`) checks the proof with
  `turnstile.verify_backup`, then spends two `signup_rate` budgets per hour:
  `SIGNUP_BACKUP_MAX_PER_IP` (10) per address and
  `SIGNUP_BACKUP_MAX_PER_WINDOW` (60) for the whole platform. Attempts count,
  not only creations, because one proof can be replayed on its claim until
  the claim is used. An exhausted budget is a `rate_limited` refusal with
  detail `backup_check`, so a bot spending the platform budget pings the
  administrators; Turnstile signups are unaffected.
- A backup signup stores `signup.challenge = "backup:<reason>"` in the
  encrypted account record.
- The proof is a speed bump, not a human check: the honeypot field, the
  randomized email field, the one-use claim, the per-IP creation limit and
  email verification still apply. `SIGNUP_BACKUP_CHECK=off` (also `0`,
  `false`, `no`) turns it off; the server then answers a proof with
  `turnstile_required`.

## Claim renewal

The signup claim (`forkmesh_signup_intent` cookie and its
`account_signup_intents` row) lives 15 minutes. Visitors who left the page
open longer, reloaded it in another tab, or retried after a claim was used
got `signup_page_required` or `turnstile_stale_page` and had to reload,
losing what they had typed (Ping of 2026-10-14).

- `POST /api/accounts/signup/intent` mints a new claim exactly as a
  `GET /signup` does: it replaces the cookie and answers the new email field
  name and Turnstile cdata. It needs an allowed origin, nothing more; it gives
  nothing a page view does not.
- `signup.js` renews the claim before submitting once the page is 12 minutes
  old (`SIGNUP_INTENT_RENEW_MS`, kept two minutes under the server TTL), and
  in the background when a tab returns after that.
- When the server still answers `signup_page_required` or
  `turnstile_stale_page`, the page renews once, renames the email input,
  re-renders the widget with the new cdata, waits for its token and resubmits.
  A second refusal shows the reload message; a failed renewal shows its own
  message and reports `signup_page_renew_failed`.
- The server still pings on the first refusal, so a recovered refusal can
  still produce a ping. A `signup_page_renew_failed` ping, or two refusals
  for one visitor, is the sign of a real fault.

## Server side

- `_account_signup` sends every refusal through `_signup_refused`, which pings
  and returns the same response, form errors included. Turnstile refusals
  carry the verifier's detail in brackets, for example
  `[siteverify:invalid-input-response]`, `[hostname_mismatch]`,
  `[cdata_mismatch]`.
- Each `GET /signup` replaces the claim cookie, so an older tab's widget
  signs stale cdata; that case is its own code, `turnstile_stale_page`, and
  the page asks for a reload.
- An exception in `_account_signup` pings `server_exception` with the class
  name and re-raises.
- `/api/accounts/signup/problem` accepts only known problem names, from an
  allowed origin, with the signed signup-intent cookie.

## Storm limits

`ping_signup_failure` keeps an in-process memo: one Ping per page view
(claim), source and reason; claimless requests grouped hourly; at most
`SIGNUP_FAILURE_PINGS_PER_WINDOW` (60) per process per hour. Rows upsert on
`signup-failure:<claim>:<source>:<reason>`.

## Code map

| Path | Role |
| --- | --- |
| `app/public/signup.js`, `app/public/signup.html` | Explicit render, `widgetToken()`, `reportProblem()` |
| `app/src/entry.py` | `ping_signup_failure`, `_signup_refused`, `_account_signup_problem`, `_signup_backup_gate` |
| `app/src/turnstile.py` | `verify_backup`, `BACKUP_BITS`, `BACKUP_PREFIX` |
| `app/tests/test_signup_failure_pings.py`, `app/tests/test_turnstile.py`, `app/tests/test_signup_page_renewal.py`, `app/tests/test_signup_backup_check.py` | Tests |
