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

## Portable sessions for mirror dashboards

Independent mirror dashboards accept a signed, narrow grant
(`app/src/portable_sessions.py`): type `forkmesh.portable-session`, audience
`forkmesh-mirror-dashboard`, at most 12 hours, verified with the published
router key. A mirror never receives the session row or password material.

## Related

`www/docs/portable-identities.md`, `www/docs/security/organization-roles.md`.
