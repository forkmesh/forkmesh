# Accounts and website sessions

How a signed-in state is created, stored and shown across the App, the site
and the World.

## Session tokens

- `POST /api/accounts/login` mints a random, server-revocable session
  (`_account_session_token`). Format `v2.<id>.<secret>`
  (`ACCOUNT_SESSION_TOKEN_RE`); the database stores only an HMAC digest
  (`_account_session_token_digest`).
- Lifetime `ACCOUNT_SESSION_TTL_MS` (five years); at most
  `ACCOUNT_SESSION_MAX_ACTIVE` (20) live sessions per account, least recently
  used evicted; last-use stamps written at most every
  `ACCOUNT_SESSION_TOUCH_MS` (5 minutes).
- A session from a key-bound login (desktop or phone) is tied to an
  `account_devices` row; revoking the device revokes the session.
- The account's primary key (`rec["pubkey"]`, set by the first desktop login)
  is also a device row. Revoking that row, alone or through `revoke-all`,
  takes the key off the account (`_account_drop_revoked_primary`): it moves to
  `prev_pubkeys`, which nothing trusts for signing, and the next desktop login
  sets a new primary key. A primary key whose device row is revoked or
  disabled is also refused on every path that checks it
  (`_account_key_revoked`: owner signing keys, signed admin requests,
  heartbeat, finalize, rotate as the old or the new key), and a later desktop
  login replaces it. Before this, revoking a stolen first desktop left its key
  trusted, and rotate let it bind a successor the owner could not see or
  revoke.
- Routes: `GET /api/accounts/sessions`, `DELETE /api/accounts/sessions/<id>`,
  `GET /api/accounts/devices`, `POST /api/accounts/devices/check`,
  `POST /api/accounts/devices/revoke-all`, `DELETE /api/accounts/devices/<id>`,
  `POST /api/accounts/logout`.

## Cookies and browser markers

| Name | Where | Purpose |
| --- | --- | --- |
| `forkmesh_account` | App origin, `HttpOnly; Secure; SameSite=Strict` | Session bearer for browser requests; re-issued on cookie-authenticated reads because browsers clamp Max-Age |
| `forkmesh_admin` | App origin | Admin console cookie, 12 hours (`ADMIN_SESSION_TTL_MS`) |
| `forkmesh.session` | `localStorage` | Client-side marker of the signed-in account |
| `forkmesh_session=1` | cookie | Presence hint only; no credential |
| `fm_attr` | site cookie | Acquisition attribution, not a login |
| `forkmesh_home` | cookie on `.forkmesh.com` | `stay` turns off the signed-in homepage redirect; absent or `dashboard` leaves it on |

## Rendering login state

- Site pages: `www/public/site-header.js`; home header:
  `www/public/home-header-auth.js`.
- Dashboard: `02-helpers.js` and `04-account.js`; it rebuilds a missing
  `forkmesh.session` marker from the App cookie.
- The World is a separate origin: it restores the session from the cookie via
  `/api/accounts/sessions` (`restoreSession` in `www/public/api-client.js`),
  waiting at most `WORLD_SESSION_RESTORE_BUDGET_MS` (250 ms) before starting
  as a guest.
- Session validity is not polled: pages check on load, focus and socket
  frames. Tabs share state through `BroadcastChannel("forkmesh.session")` and
  `localStorage`; sign-out clears both.

## Signed-in homepage redirect

A signed-in visitor opening the homepage is sent to the dashboard unless
Settings → Appearance turns it off (the `forkmesh_home` cookie). The redirect
lives in `www/public/home-header-auth.js` (never in `index.html`), runs only
after the session is confirmed, is skipped for visits referred from a
ForkMesh page or URLs with a hash or query, and uses `location.replace`.

## Where sign-out lands

Signing out, in this tab or another, sends the browser to the public homepage,
never back into the app. `https://app.forkmesh.com/` answers 308 to
`/dashboard`, so the old `location.replace("/")` after logout reopened the
dashboard (issue 2017). The dashboard uses `signedOutHomeUrl()` in
`02-helpers.js` (`https://forkmesh.com/` on the official hosts, the node's own
`/` on a self-hosted origin); `site-header.js` uses `SIGNED_OUT_HREF`
(`https://forkmesh.com/` from `app.` and `world.`, `/` elsewhere). The arrival
is referred from a ForkMesh page, so the signed-in homepage redirect above
does not fire.

## Two-factor authentication for administrators

Every account with `users.is_admin=1` must use TOTP (RFC 6238: SHA-1, 6
digits, 30 seconds); other accounts may enroll optionally.

- Login (`_account_login`): an enrolled account must send a valid `totp`, or
  gets `bad_totp`. An unenrolled administrator gets
  `401 {"error": "totp_setup_required", "totpSecret", "otpauthUri"}` and no
  session; the key is held as `totp_pending_secret` until a login presents a
  matching code, which sets `totp_enrolled` and writes an
  `account.totp_enroll` audit row.
- Admin rights (`_is_admin`, `_admin_authorized`) require `totp_enrolled`;
  `_is_admin_flag` reads only the column (for moderation protection and
  enrollment decisions).
- Sudo re-auth (`_account_sudo_reauth`) asks for the code when enrolled.
- The claim endpoints (`claim-node`, `claim-confirm`, `link-self`) check the
  password through `_resolve_user_by_password`. It refuses with 429 while the
  caller's source budget is locked, counts every wrong password or code
  against the login budgets, and asks an enrolled account for its code unless
  the request also carries that account's own session (the S-3 rule of
  `/api/accounts/profile`). They used to be an uncounted password oracle with
  no TOTP.
- Login budgets charge an IPv6 address by its /64 (`_client_source_bucket`),
  the smallest block one customer is routed, so rotating addresses inside it
  no longer buys new guesses.
- The login page draws `otpauthUri` as a QR code with the built-in `/qr.js`.
- Recovery: another administrator resets 2FA in the console user editor
  (audited as `totp`); an administrator cannot reset their own.

## Email confirmation link

`/api/accounts/verify-email?node=<name>&token=<51 chars>`: base64url of 6
bytes of issue time plus a 32-byte HMAC under `DATA_KEY` over the account
name, address and issue time (`forkmesh-email-verify-v3`). Each delivered send
stores `email_verify_issued_at`; a link with a different issue time answers
410, so a resend cancels earlier links. Links expire after 72 hours; the
confirm page shows the masked address. Code: `_send_verification_email`,
`_verify_email`, `_email_verify_link_valid`; tests
`app/tests/test_email_verification_link.py`.

### Resend limit

Whoever signs up picks the address, so a resend sends mail to someone who may
not have asked for it. Two paths can resend: `POST
/api/accounts/resend-verification` (session only) and the profile POST with
`resendVerification` (password). Both go through
`_verification_resend_refusal`: at most one send a minute and five a day per
account, read from the account's `email_sends` history.

The history is written only after the send, which takes seconds over postfix,
and the postfix route spends no daily mail budget. On its own, the history
check would let a burst of parallel requests all pass. So each send also
claims a row in `notification_email_sends`, keyed by the account name and the
current minute (`verification-resend:` blind index). The claim is one
`INSERT OR IGNORE` through rqlite, so only one request a minute wins on any
core. If the claim cannot be written, the resend is refused. The profile path
then answers `verificationError` and `verificationRetryAfterMs` instead of
sending. Each send also pings the admins ("Verification email sent to
<name>"), so the limit caps those pings too. This came out of a check on the
`chatgpt-race-b9a0aadce62d` signup on 2026-10-20: one signup and one resend,
both within the limits. Tests: `app/tests/test_email_verification_resend.py`.

### One email per signup

A new account got three verification emails on 2026-10-25. Signup
(`_account_signup`) and node finalize (`_account_finalize` in
`app/src/route_handlers.py`) used to send a fresh email on every call that
found an unverified address, so a retried, repeated or parallel request sent
another one each time. Both now call `_send_signup_verification`. It claims a
row in `notification_email_sends` keyed by the account name and the lowercased
address (`verification-signup:` blind index), and only the request that wins
the `INSERT OR IGNORE` sends. A failed send still goes to the manual
verification queue, once. A new address on the same account claims its own
row and gets its own email. If the claim cannot be written, the email is sent
anyway, because a missing link would leave the account stuck. Rows are pruned
after 30 days (`NOTIFICATION_EMAIL_SEND_RETAIN_MS`). The signup page also
ignores a second submit while the first is still running (`creatingAccount` in
`app/public/signup.js`). Sending again on purpose still goes through the
resend limit above. Tests: `app/tests/test_signup_verification_once.py`.

## Portable sessions for mirror dashboards

Independent mirror dashboards accept a signed, narrow grant
(`app/src/portable_sessions.py`): type `forkmesh.portable-session`, audience
`forkmesh-mirror-dashboard`, at most 12 hours, verified with the published
router key. A mirror never receives the session row or password material.

## Related

`www/docs/portable-identities.md`, `www/docs/security/organization-roles.md`.

## Settings feedback and confirmations

The settings page (`partials/views/settings.html`, `04-account.js`) reports
every save inline: the status line under each form is muted while working,
`text-success` on a confirmed save and `text-destructive` on a failure.
Signing out other devices, revoking a node or SSH key, and deleting an
organization, team, member, linked repo or bot token ask through
`confirmAccountAction`, an `fm-dialog` with a neutral Cancel (focused first)
and a danger confirm; issue and comment deletion and sub-issue creation in
`03-issues-profile-io.js` use the same dialog. Notices that appear on an event
are `fm-flash` banners: the node reconnect callout (warning), the node link
grant (info, with its own small CTA) and the SSH key safety note (warning).
The callout and grant keep a plain outer element that the code toggles with
the `hidden` class, because `.fm-flash` sets its own display. Empty and error
lists for sessions, nodes, SSH keys and organizations render as
`fm-blankslate`; "This device" is an accent `fm-label`; revoke buttons are
`fm-btn fm-btn-sm fm-btn-danger`. Pings switches and day toggles read the
`primary` token, and checkboxes and radios use `accent-primary`, so checked
controls are blue, never green.
