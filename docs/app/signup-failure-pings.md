# Signup failure pings

Every failed attempt on `/signup` sends a Ping to each platform administrator
(kind `operational_alert`, title `Signup failed: <reason>`). The Ping names the
username when the visitor typed a valid one and never includes the email
address.

## Why

On 2026-10-13 a visitor on a phone solved the Turnstile check (the widget
showed "Success!"), but the page still said "Complete the security check
above, then create your account." and never sent the form. Nobody heard about
it: the refusal happened in the browser, and the server-side record
(`log_blocked_signup`) only covers a few reasons and pings once per reason,
ever.

The cause was a load-order race. The page loaded Turnstile with implicit
rendering and `data-callback="forkmeshSignupChallengeDone"`. `api.js` is
`async` and `signup.js` is `defer`, so on a slow phone `api.js` could render
the widget before `signup.js` had defined the callback. The widget then
finished, but the page never got its token.

## The stale token (2026-10-13, later the same day)

After that fix, `@kaifdemo` got `turnstile_failed` (HTTP 403): the server
verified a token and refused it. "The first `turnstile_failed` Ping" below
covers how the Ping now names the failed check, and how a form from an older
tab gets `turnstile_stale_page`.

The likeliest cause on a phone is a token older than Turnstile's 300 seconds.
Managed mode usually passes without a click as soon as the page loads, and the
visitor then fills in the form. If the tab sits in the background (mail app,
password manager), the browser pauses the widget's timer, so it neither
refreshes the token nor fires `expired-callback`. Siteverify answers an
expired token with `invalid-input-response`, which the gate reports as
`turnstile_failed [siteverify:invalid-input-response]`.

`signup.js` now records when each token arrived. On submit, a token older than
240 s (or one `turnstile.isExpired` reports) resets the widget and waits up to
30 s for a fresh token, then submits. A managed pass needs no click. If no
token arrives, the page asks for the check and reports `challenge_stale`. When
the tab becomes visible again holding a stale token, the widget resets right
away.

## What the page does now

- `api.js` loads with `render=explicit&onload=forkmeshSignupChallengeReady`.
  `signup.js` renders the widget itself and passes the callbacks as functions,
  so it no longer matters which script runs first. The site key, action and
  cdata still come from the `data-*` attributes the server fills in.
- On submit, if no callback token arrived, `widgetToken()` reads it from the
  widget with `turnstile.getResponse` (skipped when `turnstile.isExpired`).
- When the page still cannot submit, or a request fails before the server
  answers, it reports the problem to `POST /api/accounts/signup/problem`:
  `challenge_unavailable`, `challenge_not_rendered`, `challenge_render_failed`,
  `challenge_token_missing`, `challenge_stale`, `challenge_error` (with Turnstile's error code),
  `network_error`, or `unexpected_response` (a reply with no JSON `error`,
  such as a proxy 502).

## Turnstile errors the widget retries

On 2026-10-13, shortly after these pings went live, one arrived as
`challenge_error [300010]`. Turnstile's `300***` (generic client failure) and
`600***` (challenge failure) codes are marked "retry", and the widget retries
on its own (`retry: "auto"`, every 8 s) and may call `error-callback` once per
attempt. The page used to tell the visitor to reload, and pinged, on the first
call, even though the next attempt usually succeeds.

`error-callback` now counts errors. For a `3xxxxx`/`6xxxxx` code below
`CHALLENGE_RETRY_ERRORS` (3), it shows a neutral "retrying" hint and does not
report; `challengeDone` clears that hint when a token arrives. It shows the
reload hint and reports `challenge_error` (detail `<code>:<count>`, once per
page view) when the third error arrives or when the code is not one Turnstile
retries (for example the `110***` configuration codes). A submit with no token
reports `challenge_token_missing` with the last error code as its detail.

## Server side

- `_account_signup` sends every refusal through `_signup_refused`, which pings
  and then returns the same response. That includes ordinary form errors
  (taken name, taken email, short password). Turnstile refusals report their
  exact code and the verifier's detail through `_turnstile_gate(..., outcome=)`.
  The detail goes in brackets at the end of the Ping body, for example
  `[siteverify:invalid-input-response]`, `[hostname_mismatch]` or
  `[cdata_mismatch]`.

- An exception thrown out of `_account_signup` pings `server_exception` with
  the exception class name, then still raises so the normal error log keeps it.
- `/api/accounts/signup/problem` accepts only a known problem name, from an
  allowed origin, carrying the signed signup-intent cookie (its path,
  `/api/accounts/signup`, covers this route). Without a page view there is no
  ping.
- `log_blocked_signup` is unchanged and still writes the grouped error log row.

### The first `turnstile_failed` Ping

On 2026-10-13 the first Ping from this page read `Signup refused @kaifdemo:
turnstile_failed (HTTP 403)`. It did not say which check failed:
`_turnstile_gate` dropped the detail from `turnstile.verify`, and that code
covers a siteverify rejection, a hostname, action or cdata mismatch and a bad
timestamp. Pings now carry the detail.

One of those causes could hit a real visitor on every retry. Each
`GET /signup` replaces the claim cookie, so after a reload or a second tab
the older tab's widget still signs the old cdata and can never pass. That case
is now its own code, `turnstile_stale_page`, and the page asks the visitor to
reload it (docs/OPERATIONS.md, "Signup challenge").

## Storm limits

`ping_signup_failure` keeps an in-process memo:

- one Ping per page view (signup claim), source and reason;
- requests with no claim are grouped into hourly buckets, so a bot posting
  without the page costs one Ping per reason per hour;
- at most `SIGNUP_FAILURE_PINGS_PER_WINDOW` (60) Pings per process per hour.

Each Ping row upserts on its dedupe key (`signup-failure:<claim>:<source>:<reason>`).

## Code map

| Path | Role |
| --- | --- |
| `app/public/signup.js` | Explicit Turnstile render, `widgetToken()`, `reportProblem()` |
| `app/public/signup.html` | Loads `api.js?render=explicit`; widget `data-*` attributes |
| `app/src/entry.py` | `ping_signup_failure`, `_signup_refused`, `_account_signup_problem`, dispatch in `accounts_handler` |
| `app/tests/test_signup_failure_pings.py` | Recipients, no email, dedupe, cap, browser report gate |
| `app/tests/test_turnstile.py` | Explicit render wiring, retry-before-report |
