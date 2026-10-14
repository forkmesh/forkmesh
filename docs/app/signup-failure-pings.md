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
- Turnstile `3xxxxx`/`6xxxxx` errors are retried by the widget; the page
  reports `challenge_error` only after `CHALLENGE_RETRY_ERRORS` (3) errors or
  for a non-retryable code.
- When the page cannot submit, or a request fails before the server answers,
  it reports to `POST /api/accounts/signup/problem`: `challenge_unavailable`,
  `challenge_not_rendered`, `challenge_render_failed`,
  `challenge_token_missing`, `challenge_stale`, `challenge_error`,
  `network_error`, or `unexpected_response`.

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
| `app/src/entry.py` | `ping_signup_failure`, `_signup_refused`, `_account_signup_problem` |
| `app/tests/test_signup_failure_pings.py`, `app/tests/test_turnstile.py` | Tests |
