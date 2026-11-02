# Accounts and website sessions

How a signed-in state is created, stored and shown across the App, the site
and the World. Part of [the App docs](README.md).

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
  login replaces it. So revoking a stolen first desktop untrusts its key, and
  it cannot rotate to a successor the owner cannot see. The key also comes off
  every node this account owns, because a node record that kept it would still
  accept its pushes, rotations and link grants; the owner relinks the node from
  a new desktop.
- A password reset or recovery-code reset also revokes every device key except
  the account's primary key and its nodes' keys, and removes its SSH keys
  (`_account_revoke_other_keys`), because someone who once had the password may
  have bound their own key. The primary and node keys stay so a person who
  forgot their password does not lose their identity.
- `POST /api/accounts/rotate` also needs `newSig`: the successor key signs
  `forkmesh-rotate-accept-v1\n<account>\n<oldPubkey>\n<newPubkey>\n<ts>`
  (`ForkMeshIdentity::acceptRotation` on the desktop). A request without it
  gets 426 `new_key_proof_required`, a bad one 401 `bad_new_key_signature`.
  The successor is refused with 409 `device_key_conflict` when another
  account (other than a node's owning user) has it in `account_devices`, or
  `_device_proven_user_bis` names another user. The proof stops an account
  binding a key it does not hold (say the browser key on someone else's public
  pull request) and then passing the pull author check.
- `_git_push_account_for_pubkey` resolves a key to nobody when its `nodes` row
  belongs to one account but an enabled device row for the same key belongs
  to another (other than the node's owning user), so a primary key can never
  outrank someone else's device.
- Routes: `GET /api/accounts/sessions`, `DELETE /api/accounts/sessions/<id>`,
  `GET /api/accounts/devices`, `POST /api/accounts/devices/check`,
  `POST /api/accounts/devices/revoke-all`, `DELETE /api/accounts/devices/<id>`,
  `POST /api/accounts/logout`.

## Guessing budgets stop correct guesses too

A wrong password charges a source budget and a source+name budget. Once the
source+name budget is spent, `_account_login` and `_password_git_account` answer
429 before checking the password, so a correct guess looks like a wrong one.
The budget belongs to the source, so a stranger cannot lock someone out by
naming them. A source's own budget refuses correct passwords only past
`LOGIN_SOURCE_HARD_FAILS` failures, so a few typos in a shared office do not
block everyone there, while spraying many accounts from one address stops.
A request with no client address metadata has only a name budget, so it stays
unchecked until the password is verified.

## Password checks cost the same for unknown accounts

`POST /api/accounts/login` and the password branch of
`POST /api/accounts/profile` run one PBKDF2 check even when the identifier
has no active account with a password (`_verify_password_miss`, against the
fixed `PASSWORD_MISS_SALT` / `PASSWORD_MISS_HASH`), so response time does not
tell whether an email address has an account. Tests:
`app/tests/test_login_key_binding.py`, `app/tests/test_account_totp_gate.py`.

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
- The admin shortcut (the shield button in the dashboard header and
  **Admin console** in the account menu) needs `adminUrl` in the stored
  session. The profile read (`/api/accounts/<name>`) never returns the private
  console path, so a session rebuilt from the cookie gets it from the
  `account` block of `GET /api/accounts/sessions`, which carries `isAdmin` and
  `adminUrl` for an admin only (`_account_admin_link`, TOTP-enrolled accounts
  only). The dashboard's `backfillAdminUrl` asks once when a stored admin
  session has no `adminUrl`, then marks it `adminUrlCheckedAt`.
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
`/dashboard`, so `location.replace("/")` would reopen the dashboard. The
dashboard uses `signedOutHomeUrl()` in
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
  `/api/accounts/profile`), so they are not an uncounted password oracle.
  - A missing code answers `bad_totp` without touching any budget, so a
    client that has not asked for the code yet (the desktop's "Log in as a
    user", the dashboard's claim form) cannot lock its own account's
    `login-totp:<name>` budget by retrying. Only a wrong code that follows a
    correct password charges that budget; an outsider without the password is
    charged on its source and source+name keys and never reaches the code
    check.
  - The desktop dialog and the dashboard form show a 2FA code field when the
    answer is `bad_totp` and send it as `totp`; the dashboard also sends its
    bearer token when it is not in cookie mode, so the own-session waiver
    applies there too.
- Address budgets charge an IPv6 address by its /64 (`_client_source_bucket`),
  the smallest block one customer is routed, so rotating addresses inside it
  buys no new guesses: login, signup, the signup backup check, node
  reserve and the security webhook. `test_every_address_budget_in_the_worker_folds_ipv6_to_a_slash_64`
  fails a new budget in `entry.py` that skips the fold. The same fold covers
  the per-address budgets outside it: signed-out bug reports
  (`bug_reports._reporter_key`), the `/product-analysis` login, instance
  launches and directory registrations (`LaunchRuntime.client_key`), the AI
  guestbook and client error reports. `test_every_per_address_budget_folds_ipv6_into_its_64`
  lists every `_transient_client_address` call in `app/src` as either a
  budget (must fold) or a stored value (kept as is), so a new call fails until
  it is classified.
- `/product-analysis` is guarded by one shared credential, so on top of the
  per-source budget every failed login there also counts against one global
  budget: 200 failures in an hour refuse every login, correct or not, until
  the hour that started with the first of them ends. Administrators keep
  their console session route into the page.
- Signed-out bug reports share one hourly attachment budget of 64 MiB
  (`ANONYMOUS_ATTACHMENT_BYTES_PER_WINDOW`), since every attachment is copied
  to every core. Past it a signed-out report with attachments gets 429
  `attachments_busy`; a report without attachments, or from a signed-in
  account, is still accepted.
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

Whoever signs up picks the address, so a resend mails someone who may not have
asked. Two paths resend: `POST /api/accounts/resend-verification` (session
only) and the profile POST with `resendVerification` (password). Both go
through `_verification_resend_refusal`: at most one send a minute and five a
day per account, read from the account's `email_sends` history.

That history is written only after the send (seconds over postfix), so alone
it would let parallel requests all pass. Each send therefore also claims a row
in `notification_email_sends` keyed by account and current minute
(`verification-resend:` blind index) with one `INSERT OR IGNORE` through
rqlite: one request a minute wins on any core. If the claim cannot be written
the resend is refused, and the profile path answers `verificationError` and
`verificationRetryAfterMs`. Each send also pings the admins ("Verification
email sent to <name>"), so the limit caps those pings too. Tests:
`app/tests/test_email_verification_resend.py`.

### One email per signup

Signup (`_account_signup`) and node finalize (`_account_finalize` in
`app/src/route_handlers.py`) both see an unverified address, and a retried or
parallel request would mail again each time. Both call
`_send_signup_verification`, which claims a row in `notification_email_sends` keyed by the account name and the lowercased
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

[portable identities](../portable-identities.md),
[organization roles](../security/organization-roles.md),
[encryption](../encryption.md).

## Settings pinned header

The account header (avatar, name and the page buttons) and the open section's
title stay at the top while the settings page scrolls. A `<style>` block at the top of `partials/views/settings.html`
makes `[data-settings-account-header]` sticky from 768 px up, where it is a
single row, and every `[data-settings-section-heading]` (a section's `h2`, or
the Pings title row with its save status) sticky at all widths.
`bindSettingsStickyHeader` in `04-account.js` keeps
`--settings-sticky-top` at the header's height while it is sticky (0 on
phones, where it would take a quarter of the screen), so the section title
pins right under it, and anchors scrolled into view clear both.

## Settings feedback and confirmations

The settings page (`partials/views/settings.html`, `04-account.js`) reports
every save inline: the status line under each form is muted while working,
`text-success` on a confirmed save and `text-destructive` on a failure.
Signing out other devices, revoking a node or SSH key, and deleting an
organization, team, member, linked repo or bot token ask through
`confirmAccountAction`, an `fm-dialog`
([dashboard-components.md](dashboard-components.md)) with a neutral Cancel (focused first)
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
