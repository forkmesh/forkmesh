# Admin dashboard (`/product-analysis`)

An internal, read-only dashboard at `https://app.forkmesh.com/product-analysis`
for account, signup-origin, mirror, and clone counts. It has no public version.

## Access

| Secret | Meaning |
| --- | --- |
| `PRODUCT_ANALYSIS_USER` | Login username. |
| `PRODUCT_ANALYSIS_PASS` | Login password and session-cookie key material. |

- With either secret unset the route returns the ordinary 404 (a signed-in
  `is_admin` account is still admitted).
- Unauthenticated `GET` shows only a sign-in form, `noindex`. Unknown paths,
  methods, views, and actions return 404.
- Login sets `forkmesh_product_analysis` (HttpOnly, Secure, SameSite=Strict,
  12 hours). Rotating the password ends every session.
- Failed logins share the account-login rate limit (`429` with `Retry-After`).
- No JavaScript; CSP `default-src 'none'`; `no-store`. Rendered inside the
  shared admin shell (`app/src/admin_shell.py`).

## Sections

All windows are UTC (last 30 days, previous 30 days, 90-day and 30-day daily
charts). Five tabs:

- **Overview**: total accounts, accounts with a mirror, activation (first
  mirror within 7 days of signup), returning accounts, mirrors created,
  repositories cloned and anonymous clones (from `product_clone_daily`), and
  all-time clones (`mirror_serve_counters`).
- **Signup sources**: accounts and mirror creators per origin.
- **Users**: sortable, paginated, read-only list with last activity.
- **Repositories**: catalog records with mirror count, size, clones, and
  `hostedSince`.
- **Acquisition**: pending; served without building a report
  (`VIEWS_WITHOUT_REPORT`).

## Signup origin

`users.signup_origin` is `job_flow`, `referral`, `organic`, or `unknown`,
stamped at signup by `_record_signup_origin`. Rules (`v1`): a referral
attribution or referrer marker gives `referral`; a signup inside
`PRODUCT_ANALYSIS_JOB_FLOW_WINDOWS` (none configured) gives `job_flow`; a
captured marker without referrer gives `organic`; everything else is `unknown`.
Migration `0175_product_analysis.sql` backfills the referral rule; the app
backfills the encrypted rules once, guarded by `schema_meta` key
`product_analysis_signup_origin_backfill:v1`.

## Data correctness and placement

Every source table is read with keyset pagination (`scan_table`) and checked
against `COUNT(*)`; a mismatch renders an error for the affected figures only.
Requests run in their own app ingress shard (an emulated Durable Object on
the core nodes) because a report decrypts every account and repository row.

## Operations

- Rotate the password on the fleet: edit `PRODUCT_ANALYSIS_PASS` in
  `secrets.env`, then run `python3 tools/next_vault.py sync --execute`
  ([OPERATIONS.md](../../docs/OPERATIONS.md)).
- Disable the route by deleting either secret.
- Each public HTTPS clone does one best-effort `product_clone_daily` upsert.
- Down migration: `app/migrations/down/0175_product_analysis.sql`.
