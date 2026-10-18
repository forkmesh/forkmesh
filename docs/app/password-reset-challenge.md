# Password reset challenge

`/forgot-password` sends `POST /api/accounts/forgot-password`, which needs a
Turnstile token (`app/src/turnstile.py`, action `password-reset`) or a backup
proof. Before 2026-10-18 the page had none of the recovery the signup page
gained ([signup-failure-pings.md](signup-failure-pings.md)), and reset did not
work on phones:

- A tap on "Send reset link" before the managed widget had answered (it takes
  a second or two on a phone, longer when it shows the checkbox) was refused
  with "Complete the security check above first" instead of waiting.
- Any widget error, retryable or not, showed "could not load, reload the
  page". Errors no reload fixes (`200100`, the phone's clock is wrong) and a
  blocked `api.js` (content blockers, private DNS, Lockdown Mode) left the
  visitor stuck.
- A token held while the tab sat in the background, or a page restored from
  the back/forward cache, was spent stale.

## Browser side

`forgot-password.js` follows `signup.js`:

- The site key and a backup page value come from
  `GET /api/accounts/forgot-password/challenge` (`siteKey`, `enabled`,
  `cdata`); `/api/turnstile` stays for pages loaded before this change.
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

## Server side

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
| `app/public/forgot-password.js`, `app/public/forgot-password.html` | Widget, token wait, backup check |
| `app/src/password_reset.py` | `_account_forgot_password_challenge`, `_password_reset_backup_gate` |
| `app/src/entry.py` | `PASSWORD_RESET_BACKUP_*`, route |
| `app/tests/test_password_reset_backup_check.py` | Tests |
