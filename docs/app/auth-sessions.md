# Accounts and website sessions

How a signed-in state is created, stored and shown across the App
(`app.forkmesh.com`), WWW (`forkmesh.com`) and World (`world.forkmesh.com`).

## Session tokens

- `POST /api/accounts/login` mints a random, server-revocable session
  (`_account_session_token` in `app/src/entry.py`). The token format is
  `v2.<id>.<secret>` (`ACCOUNT_SESSION_TOKEN_RE`). D1 stores only an HMAC
  digest of it (`_account_session_token_digest`), never the bearer token.
- Lifetime: `ACCOUNT_SESSION_TTL_MS`, five years. At most
  `ACCOUNT_SESSION_MAX_ACTIVE` (20) live sessions per account; the least
  recently used is evicted. Last-use stamps are written at most every
  `ACCOUNT_SESSION_TOUCH_MS` (5 minutes).
- A session minted by a key-bound login (desktop or mobile node) is tied to an
  `account_devices` row; revoking the device revokes the session.
- Management routes: `GET /api/accounts/sessions`,
  `DELETE /api/accounts/sessions/<id>`, `GET /api/accounts/devices`,
  `POST /api/accounts/devices/check`, `POST /api/accounts/devices/revoke-all`,
  `DELETE /api/accounts/devices/<id>`,
  `POST /api/accounts/logout`.

## Cookies and browser markers

| Name | Where | Purpose |
| --- | --- | --- |
| `forkmesh_account` | App origin, `HttpOnly; Secure; SameSite=Strict` | The session bearer for browser requests (`ACCOUNT_SESSION_COOKIE`). Browsers clamp Max-Age (Chromium to 400 days), so the session API re-issues it on each cookie-authenticated read. |
| `forkmesh_admin` | App origin | Admin console page cookie, 12 hours (`ADMIN_SESSION_TTL_MS`). |
| `forkmesh.session` | `localStorage` | The one client-side marker of the signed-in account, read by every site section. |
| `forkmesh_session=1` | cookie | Presence hint only; carries no credential. |
| `fm_attr` | WWW cookie | Acquisition attribution (`www/worker.js`, `docs/attribution.md`), not a login. |
| `forkmesh_home` | cookie on `.forkmesh.com` | `stay` turns off the signed-in homepage redirect; absent or `dashboard` leaves it on. |

## Which script renders login state

- Marketing, docs and chat pages: `www/public/site-header.js`.
- Home hero header: `www/public/home-header-auth.js`.
- Dashboard: `app/public/dashboard/js/02-helpers.js` and `04-account.js`
  (bundled into `app/public/dashboard.js`). The dashboard rebuilds a missing
  `forkmesh.session` marker from the App cookie.
- World is a separate origin and never sees the App's `localStorage`. It
  restores the session from the `forkmesh_account` cookie through
  `/api/accounts/sessions` (`restoreSession` in `www/public/api-client.js`) and waits
  only `WORLD_SESSION_RESTORE_BUDGET_MS` (250 ms, `world/public/world/world.js`)
  before starting as a guest; a later restore adopts the login.
- WWW expires its cached marker when the App has already revoked the session
  (`aebd861ba`, `www/tests/test_landing_session_expiry.py`).

## Two-factor authentication for administrators

Every account with `users.is_admin=1` must use an authenticator app (TOTP,
RFC 6238: SHA-1, 6 digits, 30 seconds). Accounts that are not administrators
can still sign in with a password alone unless they have enrolled.

- Login (`_account_login`): after a correct password, an enrolled account must
  send a valid `totp`, otherwise the response is `bad_totp`. An administrator
  who has not enrolled gets `401 {"error": "totp_setup_required",
  "totpSecret", "otpauthUri"}` and no session.
  `_account_admin_totp_enrollment` stores the key as `totp_pending_secret` in
  the encrypted account record and sends the same key again until a login
  presents a code that matches it. That code moves the key to `totp_secret`,
  sets `totp_enrolled`, writes an `account.totp_enroll` audit row and finishes
  the login. A wrong code is `bad_totp` and counts against the login budget.
- Admin rights (`_is_admin`) also need `totp_enrolled`. An account whose
  `is_admin` column is set but which has not enrolled is treated as an
  ordinary user, so admin sessions minted before 2FA existed, and admins
  promoted in the console, get no admin powers until they enroll at their next
  sign-in. Node-signed admin requests (`_admin_authorized`) apply the same
  rule. `_is_admin_flag` reads only the column. It is used where the question
  is "is this target an administrator" (moderation protection) and to decide
  whether a login must enroll.
- Sudo re-auth (`_account_sudo_reauth`) asks for the code whenever the account
  is enrolled.
- Clients: the web login page (`app/public/login.js`) shows the key and an
  `otpauth://` link. The desktop (`verifyTotpLogin`) shows the key in a
  dialog whose text can be selected. Clients that only know `bad_totp` show
  their generic login error; the user can sign in on the web once to enroll.
- Recovery: another administrator ticks "Reset two-factor authentication" in
  the admin console's user editor (`_admin_update_user`, audited as `totp`).
  The reset drops the enrolled and pending keys, and the next login starts
  enrollment again. An administrator cannot reset their own 2FA, because the
  reset would take away their admin rights partway through the save.

## Portable sessions for mirror dashboards

Independent mirror dashboards accept a signed, narrow session grant instead of
the D1 session (`app/src/portable_sessions.py`): type
`forkmesh.portable-session`, audience `forkmesh-mirror-dashboard`, at most 12
hours. A mirror verifies it with the published router key and never receives
the session row or password material. See
`www/docs/operations/breakout-authorization-replica.md`.

## Traps

- Session validity is not polled. Pages check it on load, focus and on a
  socket frame (`www/docs/operations/polling-elimination.md`).
- Several tabs share login state through `BroadcastChannel("forkmesh.session")`
  as well as `localStorage`; sign-out must clear both
  (`www/tests/test_landing_session_expiry.py`).

## Related

- [App Worker home](README.md)
- `www/docs/portable-identities.md`, `www/docs/security/organization-roles.md`

## 2FA setup QR code (2026-10-01)

When login answers `totp_setup_required`, the login page now draws the
`otpauthUri` as a QR code (`#totp-setup-qr`) above the key, so an authenticator
app can scan it instead of typing the secret. It uses ForkMesh's own
generator, `/qr.js` (`window.ForkMeshQR.toCanvas`), loaded only at that
moment; the key and the "Open in authenticator app" link stay as fallbacks.

## Signed-in homepage opens the dashboard (2026-10-13)

A signed-in visitor who opens forkmesh.com is sent to the dashboard
(`https://app.forkmesh.com/dashboard`, or `/dashboard` off the marketing
host). It is on by default and turned off in Settings → Appearance, "Open the
dashboard from forkmesh.com" (`[data-home-redirect]`,
`renderHomeRedirectSetting` / `saveHomeRedirectSetting` in
`app/public/dashboard/js/02-helpers.js`).

- The choice is the `forkmesh_home` cookie on `Domain=.forkmesh.com`, one
  year, the pattern of `forkmesh_genie` and `forkmesh_theme`: the switch lives
  on app.forkmesh.com and the redirect runs on forkmesh.com, so a
  `localStorage` flag could not cross. Turning it back on writes `dashboard`
  rather than deleting the cookie.
- The redirect is in `www/public/home-header-auth.js`, not `index.html`,
  which must not name the session marker
  (`app/tests/test_home_session_redirect.py`). It runs only after the marker
  is confirmed: the cached or fresh `/api/accounts/sessions` answer, or a
  cookie restore, so a revoked session never bounces to a signed-out
  dashboard.
- It is skipped when the visit came from a ForkMesh page (referrer on
  forkmesh.com or a subdomain), so the logo, the dashboard's home link and
  `/#pricing` still show the homepage, and when the URL has a hash or query
  (campaign links, `#pricing`). `location.replace` keeps the homepage out of
  history so Back does not loop.
- Tests: `www/tests/test_home_dashboard_redirect.py` runs the script under
  node with stubbed storage and cookies.

## Email confirmation link (2026-10-13)

A reporter read `issuedAt=<ms>&token=<hex>` in the confirmation URL as
something anyone could swap in to confirm an address. It never was: the token
is an HMAC under `DATA_KEY` (the blind-index key, see [REKEY.md](../REKEY.md))
over the account name, the address on file and the issue time, so changing
`node`, `issuedAt` or `token` fails. The flow was changed so it also looks and
reads that way (`_send_verification_email`, `_verify_email` in
`app/src/entry.py`):

- The link is `/api/accounts/verify-email?node=<name>&token=<51 chars>`. The
  token is base64url of 6 bytes of issue time plus the full 32-byte HMAC
  (`forkmesh-email-verify-v3` prefix); there is no editable timestamp.
- Every delivered send stores `email_verify_issued_at` on the account. A link
  with a different issue time answers 410 "This link was replaced", so a resend
  cancels earlier links. A send that failed (queued for manual verification)
  does not cancel the link already in the mailbox. Callers that hold the record
  and save it again pass `rec=` so their stale copy cannot restore the old
  time.
- The confirm page names the masked address (`a•••@example.com`) and account,
  says the link expires after 72 hours and that ForkMesh never asks for a
  password there. A link for an already-verified address answers "Email
  already confirmed" and changes nothing; confirming clears
  `email_verify_issued_at`. Confirming still runs the referral and hackathon
  hooks, since a verified friend is what moves a referrer's progress.
- The email states the same terms under the button.
- Links sent before this change (`issuedAt` + 32-hex `v2` token) still confirm
  until they expire; that branch in `_email_verify_link_valid` can go after
  2026-10-17.
- Tests: `app/tests/test_email_verification_link.py` runs the real handler
  over a stubbed store (edited URLs, replaced links, expiry, reuse, legacy
  links); `app/tests/test_signup_page_gate.py` checks the token codec.
