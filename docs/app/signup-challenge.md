# Signup and password reset challenge

Part of [the App docs](README.md). `/signup` and `/forgot-password` both ask
for a Turnstile token (`app/src/turnstile.py`) and both fall back
to the same proof-of-work backup check when a browser cannot produce one.
This page covers the signup page's challenge, backup check and claim renewal,
the Pings every failed signup sends, and the password reset page that reuses
the same recovery.

## Signup failure pings

Every failed attempt on `/signup` sends a Ping to each platform administrator
(kind `operational_alert`, title `Signup failed: <reason>`). The Ping names
the username when the visitor typed a valid one and never includes the email
address.

## Signup: browser side

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

## Signup backup check

It exists because some browsers can never produce a Turnstile token: a wrong
clock (`challenge_error [200100:1]`, which no retry fixes), a blocked `api.js`
(ad blockers, corporate filters), iframe load errors or a Turnstile outage.

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
`account_signup_intents` row) lives 15 minutes. Renewal keeps a visitor who
left the page open, reloaded it in another tab or retried after a used claim
from having to reload and lose what they typed.

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

## Signup: server side

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

## Signup ping storm limits

`ping_signup_failure` keeps an in-process memo: one Ping per page view
(claim), source and reason; claimless requests grouped hourly; at most
`SIGNUP_FAILURE_PINGS_PER_WINDOW` (60) per process per hour. Rows upsert on
`signup-failure:<claim>:<source>:<reason>`.

## Password reset challenge

`/forgot-password` sends `POST /api/accounts/forgot-password`, which needs a
Turnstile token (action `password-reset`) or a backup proof. The page has the
same recovery as signup because on phones the widget answers slowly, some
errors no reload fixes (`200100`, a wrong clock), `api.js` can be blocked
(content blockers, private DNS, Lockdown Mode), and a token held in a
background tab goes stale.

### Password reset: browser side

`forgot-password.js` follows `signup.js`:

- The site key and a backup page value come from
  `GET /api/accounts/forgot-password/challenge` (`siteKey`, `enabled`,
  `cdata`); `/api/turnstile` stays for pages loaded from an older build.
- On submit with no callback token the page reads `turnstile.getResponse`,
  then waits up to `CHALLENGE_WAIT_MS` (30 s) for the widget ("Finishing the
  security check above…") instead of refusing.
- A token older than 240 s, or one the widget reports expired, is reset and
  waited for. A tab becoming visible with a stale token, or a page restored
  from the back/forward cache, resets the widget.
- `3xxxxx`/`6xxxxx` errors are retried by the widget; after three errors, on
  any other code, when `render` throws, when `api.js` has not loaded after
  10 s or at submit, when there is no site key, when no token arrives in the
  wait, or when the server answers `turnstile_unavailable` (one resend), the
  page hides the widget and runs the backup check.
- The backup check is the signup puzzle (`BACKUP_PREFIX`, `BACKUP_BITS`)
  over the page's `cdata`. The body carries
  `backupProof: {nonce, reason, cdata}` and no `turnstileToken`. A
  `turnstile_expired` answer to a proof fetches a fresh `cdata`, solves again
  and resends once.

### Password reset: server side

- `cdata` is `<unix seconds hex>-<16 random hex>-<signature>`; the signature
  is the first 32 hex of the blind-index HMAC of
  `password-reset-backup-v1\ncdata\n<seconds>-<random>`. It is valid for
  `PASSWORD_RESET_BACKUP_CDATA_TTL_MS` (30 min) and 60 s into the future.
- `_password_reset_backup_gate` (in `password_reset.py`, taken instead of
  `_turnstile_gate` when the body has `backupProof`) checks the proof with
  `turnstile.verify_backup`, then the signature and age (`turnstile_expired`
  otherwise), then spends two `signup_rate` budgets per hour:
  `PASSWORD_RESET_BACKUP_MAX_PER_IP` (10, per IPv6 /64 like the requester
  limit) and `PASSWORD_RESET_BACKUP_MAX_PER_WINDOW` (60) for the platform. An
  exhausted budget answers 429 `rate_limited`; the page says to try later.
- Both gates run before any account lookup, so a refusal says nothing about
  whether the address exists. The per-requester and per-account reset limits
  still apply after either gate.
- `SIGNUP_BACKUP_CHECK=off` turns off this backup too; the server then
  answers a proof with `turnstile_required`.

## Code map

| Path | Role |
| --- | --- |
| `app/public/signup.js`, `app/public/signup.html` | Explicit render, `widgetToken()`, `reportProblem()` |
| `app/public/forgot-password.js`, `app/public/forgot-password.html` | Widget, token wait, backup check |
| `app/src/entry.py` | `ping_signup_failure`, `_signup_refused`, `_account_signup_problem`, `_signup_backup_gate`, `PASSWORD_RESET_BACKUP_*`, the reset route |
| `app/src/password_reset.py` | `_account_forgot_password_challenge`, `_password_reset_backup_gate` |
| `app/src/turnstile.py` | `verify_backup`, `BACKUP_BITS`, `BACKUP_PREFIX` |
| `app/tests/test_signup_failure_pings.py`, `app/tests/test_turnstile.py`, `app/tests/test_signup_page_renewal.py`, `app/tests/test_signup_backup_check.py` | Signup tests |
| `app/tests/test_password_reset_backup_check.py` | Password reset tests |
