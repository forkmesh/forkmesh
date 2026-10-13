# Admin dashboard (`/product-analysis`)

An internal, read-only dashboard for the founders, served by the App Worker at
`https://app.forkmesh.com/product-analysis`. It answers, without SQL: how many
accounts exist, which acquisition channel they came through, whether they
created a mirror, and whether anything is being cloned. It is deliberately
small and must not grow into a business-intelligence tool.

The marketing Worker (`forkmesh.com`) does not proxy or redirect this path;
there is no public version of the page.

## Access

Credentials are two Worker secrets, never anything in the repository:

| Secret | Meaning |
| --- | --- |
| `PRODUCT_ANALYSIS_USER` | The username the login form accepts. |
| `PRODUCT_ANALYSIS_PASS` | The password. Also the key material for the session cookie. |

Set them from `app/`:

```bash
uvx --from workers-py pywrangler secret put PRODUCT_ANALYSIS_USER
```

```bash
uvx --from workers-py pywrangler secret put PRODUCT_ANALYSIS_PASS
```

Rules the route follows:

- With either secret unset the route does not exist: every request gets the
  ordinary App 404 page.
- An unauthenticated `GET` shows only a sign-in form (username, password),
  with `noindex, nofollow` in the markup and the `X-Robots-Tag` header. No
  section name or figure is rendered before login.
- Any sub-path, any method other than `GET`, `HEAD` or `POST`, any unknown
  `?view=`, and any `POST` action other than `login`/`logout` share the
  ordinary 404 page.
- A correct login mints a signed cookie (`forkmesh_product_analysis`,
  HttpOnly, Secure, SameSite=Strict, scoped to the path) valid for 12 hours.
  Its HMAC key is derived from the password and `DATA_KEY`, so rotating the
  password signs every founder out immediately.
- Failed logins spend the same per-source budget as account logins
  (`login_attempts`, 10 failures in 15 minutes lock the source for 15
  minutes). A locked source gets `429` with `Retry-After` before the password
  is compared.
- `app/public/robots.txt` disallows the path. There is no admin bootstrap
  endpoint, no "first user becomes admin" logic and no invite flow.
- A signed-in platform administrator is also let in, without the password.
  The check is the admin console's own (`_check_admin_page_auth`: a live
  account session whose account is `is_admin`), so signing out or losing
  the role takes effect on the next request, and the route mints nothing for
  them. With the secrets unset an administrator can still open the page;
  everyone else gets the 404. Apart from that, the two secrets remain the
  whole access model.

The page ships no JavaScript. Its Content-Security-Policy is
`default-src 'none'`, with inline styles only, and it is `no-store`.

## Appearance

The page is the **Product analytics** section of the unified admin: it renders
inside the shared admin shell (`app/src/admin_shell.py`) with the same
sidebar, top bar, GitHub light and dark palettes and components as the
console, acquisition and hackathon pages. Its five views are tabs under the
page heading.

A founder signed in with the password rather than an admin account sees only
this section in the sidebar and a Sign out button, so the page never reveals
the console path. An administrator sees the whole admin sidebar.

Light, dark and system are the switch at the bottom of the sidebar (and under
the sign-in card). It is a plain form posting to `/admin/theme`, which sets
one `forkmesh_admin_theme` cookie for every admin page and redirects back to
the view that was open; without the cookie the page follows the browser's
`prefers-color-scheme`. Below 768px the sidebar becomes a drawer driven by a
hidden checkbox and labels, so there is still no script on the page, and
`prefers-reduced-motion` removes the slide.

## Timezone and windows

Every bucket boundary on every chart and every window in every figure uses
**UTC** (`PRODUCT_ANALYSIS_TIMEZONE` in `app/src/product_analysis.py`). The
Worker stores millisecond epoch timestamps, so UTC is the only timezone that
needs no conversion table.

| Window | Definition |
| --- | --- |
| All time | Every row, no lower bound. |
| Last 30 days | `[now − 30 × 24 h, now)`, ending at the request. |
| Previous 30 days | The 30 × 24 h immediately before "last 30 days". |
| Signups / mirrors per day | The last 90 UTC calendar days including today. |
| Clones per day | The last 30 UTC calendar days including today. |

Every count on the page states its window next to the number. A change is
written as a sentence ("+4 in the last 30 days, 7 fewer than the previous
30 days"); when both windows are empty, or the previous window has no
recorded data, the page says so instead of printing `0%`.

## Sections

The page has exactly five tabs.

### Overview

| Figure | Definition |
| --- | --- |
| Total accounts | Rows in `users` whose decrypted record is a user account (kind `user`), any status. Node reservations are excluded. |
| Accounts with at least one mirror | Accounts owning at least one catalog record, directly or through a node they own (`users.data.nodes` and `nodes.user_bi`). The window change counts accounts whose oldest mirror was created in the window. |
| New accounts activated | Accounts created in the last 30 days whose oldest mirror was created no later than 7 days after signup. The change states it as "N of M new accounts" for this window and the 30 days before, and how many new accounts are younger than 7 days and can still activate. This is the marketing funnel's activation stage (`docs/marketing/plan.md`). |
| Returning accounts | Accounts created more than 30 days ago whose last activity (node `last_seen`, account session, presence) falls in the last 30 days, stated as "N of M accounts older than 30 days". Only the latest activity per account is stored, so it has no previous period. This is the marketing funnel's retention stage. |
| Mirrors created | Catalog records in `repositories`, public and private. A "mirror" is one record: a repository as hosted by one owner. |
| Repositories cloned, last 30 days | Distinct `(owner, repo)` pairs with at least one row in `product_clone_daily` inside the window. |
| Anonymous clones, last 30 days | Sum of `product_clone_daily.clones` inside the window. The public HTTPS mirror route serves only non-private repositories and carries no account identity, so every clone it counts is anonymous. Private clones through a node or the SSH gateway are not counted. |
| Clones, all time | `SUM(clones)` over `mirror_serve_counters`, the cumulative counters the Mirrors tab shows. It has no previous period. |

Two charts follow: signups per day over the last 90 days, split into
**organic** and **every other origin** (job flow, referral, unknown), and
mirrors created per day over the same 90 days.

Per-day clone counting begins with the deploy that ships migration 0175.
Until the daily table has rows the two clone figures read "No clone data
yet", and while the previous window is not fully covered the comparison is
declined with the date counting started.

### Signup sources

A table of accounts by origin (count, share of all accounts, accounts with at
least one mirror, share of that origin) and a horizontal stacked bar per
origin: mirror creators in the accent colour, the remainder hatched, so the
split does not depend on hue alone.

### Users

One row per user account: identifier (`users.username`, falling back to the
record name), signup date, signup origin, mirror count, last activity. Sortable
by every column, 50 rows per page, with a search box matching the identifier.
Read-only: no editing, impersonation, deletion or bulk actions exist.

"Last activity" is the newest of the account's newest web session
(`account_sessions.last_seen_at`), its chat presence ping
(`account_presence.ts`) and the last check-in of any node it owns
(`nodes.last_seen`). An account with none of these shows "no activity
recorded".

### Repositories

One row per catalog record: `owner/name`, owner (the human account when the
owner is a node claimed by one, else the catalog owner), visibility, size as
reported by the publishing node, mirror count, clones in the last 30 days,
created date. Sortable and paginated. Below it, clones per day over the last
30 days across all repositories.

"Mirror count" is the number of catalog records sharing the repository's root
commit (or its name when no root is published), the same grouping the network
page uses; a repository nobody else mirrors counts 1.

"Created" is the record's `hostedSince`, the millisecond timestamp the desktop
node stamps when it first hosts the repository. Records without a usable value
show "unknown" and are left out of the per-day chart and the window changes,
and the Overview says how many were left out.

### Acquisition

Rendered as **pending** with a short note. UTM attribution is not merged; the
section carries no placeholder data. When the attribution work lands it should
show sessions, clone-URL copies and signups by `utm_source` and
`utm_campaign` for a fixed last-7-days window, bots excluded with the excluded
count stated, and the note that clone-URL copies are an intent signal, not a
clone count.

Because that copy is fixed, the section is served without building a report at
all: draining and decrypting every account and repository row is most of the
CPU a dashboard request spends, and this page displayed none of it, which was
enough on its own to exceed the Worker CPU limit. `VIEWS_WITHOUT_REPORT` in
`product_analysis.py` lists the views served that way, and a view must leave
that set in the same change that gives it figures to show. A `HEAD` request is
skipped for the same reason: its response carries no body.

## Signup origin derivation

`users.signup_origin` holds one of `job_flow`, `referral`, `organic`,
`unknown` (default). It is a plain category beside `is_admin`, written once by
the backfill and stamped for every new account at signup, never computed at
query time.

Rules, in order (version `v1`):

1. **referral**: the account has a `referral_attributions` row (it arrived
   with a valid referral link, whatever the later credit decision was), or
   its encrypted signup marker names a referrer.
2. **job_flow**: the signup time falls inside a configured job-posting window
   (`PRODUCT_ANALYSIS_JOB_FLOW_WINDOWS`). **No window is configured.** The
   careers page takes applications by email, nothing in the code records a
   job-application signup, and the founders chose to leave the bucket empty
   until they supply the posting dates rather than guess.
3. **organic**: the signup marker was captured and recorded no referrer. The
   marker (`signup.referralCaptured` / `signup.referrer`) exists on accounts
   created since 2026-08-12.
4. **unknown**: everything else. That includes every account created before
   2026-08-12, accounts created by a desktop key-bound flow or federation, and
   node reservations promoted later without a marker.

Known limitation: until job-flow windows exist, a job applicant who signed up
directly after 2026-08-12 is counted as organic. The Signup sources section
and the Overview chart say so.

### How the backfill runs

- Migration `0175_product_analysis.sql` adds the column and applies rule 1 in
  SQL, because `referral_attributions` is plaintext.
- Rules 2 to 4 read the encrypted account record, which SQL cannot. The Worker
  performs that pass the first time an authorised founder opens a dashboard
  view that builds a report (see Acquisition above: the pending sections do
  not), once, guarded by the `schema_meta` key
  `product_analysis_signup_origin_backfill:v1`. It only ever replaces
  `unknown` (or a stale value) with something the rules prove; it never
  downgrades a known origin to `unknown`.
- To classify job flow later: add the `(start_ms, end_ms)` UTC windows to
  `PRODUCT_ANALYSIS_JOB_FLOW_WINDOWS`, bump the marker to `v2`, record the
  windows here, and the next dashboard load re-runs the pass. Accounts already
  marked `organic` inside a window move to `job_flow`.
- New accounts are stamped by `_record_signup_origin` in `entry.py` right after
  the account row is saved, using the same rules. A database still behind the
  migration lacks the column; the stamp is best-effort and never fails the
  signup.

## Data correctness

- Every source table is drained with keyset pagination
  (`scan_table` in `app/src/product_analysis.py`, 200 rows per read) until a
  short page arrives, then the collected row count is compared with
  `COUNT(*)`; grouped reads compare their group count with a matching
  `COUNT`. A mismatch raises, and the page renders an error saying which
  source failed and what to check, in place of every figure that depends on
  it. No default page size can cap an aggregate.
- Sources fail independently: a missing `product_clone_daily` table blanks
  only the clone figures.
- `app/tests/test_product_analysis.py` inserts 250 accounts with the scan
  page forced to 100 and asserts the report returns 250.
- Charts with fewer than seven days of data are replaced by a table of those
  days.

## Where the request runs

Every `/product-analysis` request is serialized into its own App ingress
Durable Object (shard `1`, `_serialized_http_request` and
`_app_ingress_shard` in `app/src/entry.py`). The stateless Worker request has
roughly a 10 ms Pyodide CPU allowance; a Durable Object request has the
30-second Workers budget. One report does not fit the former:

- Building a report drains `users` and `repositories` and AES-GCM decrypts
  every row. Measured against production on 2026-09-10: 240 account records
  and 53 catalog records, 1.41 MB of plaintext.
- **88.9% of that plaintext is `avatar_png`** — profile images stored inside
  the encrypted account record, up to `MAX_AVATAR_BYTES` (256 KB) each. Seven
  accounts held 1.26 MB of the 1.41 MB. The dashboard decrypts and JSON-parses
  every one of those images to read a signup timestamp beside it.
- `GET /product-analysis?view=users` answered 503/1102 at 462 ms CPU
  (1036 ms wall). Overview, Signup sources and Repositories failed
  intermittently beside it, whichever load landed on a busy isolate.

The shard is the route's own. Nothing else may be moved onto it: no other
traffic family should share an isolate with a load that heavy, and a founder
opening the dashboard should not be poisoned by one either.

The ingress placement buys headroom; it does not make the report cheap. The
cost grows linearly with accounts, and the avatar payload dominates it, so
moving avatars out of the account record is the fix that would make every
account read cheaper — login, profile, World roster and notification digests
all decrypt the same records.

## Operations

- **Rotate the password**: `pywrangler secret put PRODUCT_ANALYSIS_PASS`. Every
  session is invalid from the next request.
- **Disable the route**: delete either secret. The path returns 404.
- **D1 writes**: every clone served through the public HTTPS mirror route now
  performs one extra upsert (`product_clone_daily`) beside the existing
  serve-counter upsert. Both are best-effort and never fail the served read.
- **Down migration**: `app/migrations/down/0175_product_analysis.sql` drops the
  column, the daily table and the backfill marker.

## Decisions this build made that the brief did not settle

- **Access model**: the founders asked for a username and password from
  environment secrets, which overrides the brief's `is_admin` session
  reuse. Consequence: the sign-in form is the one page an unauthenticated
  visitor can see; everything else answers 404.
- **Job flow is unclassified** by the founders' decision; the bucket exists
  and the rule slot is empty.
- **Organic** means "direct signup with a recorded referral marker", which the
  marker's introduction date bounds to accounts created since 2026-08-12.
- **Anonymous clones** are clones through the public HTTPS mirror route; that
  route only serves non-private repositories and has no account identity.
  Counting starts at deploy, so the figure has no history.
- **A mirror is a catalog record**; the per-repository mirror count uses the
  network page's root-commit grouping.
- **"Created" for a repository** is `hostedSince`; older records without it
  are reported as undated rather than dated from another field.
- **Activation is a first mirror within 7 days of signup** (added
  2026-10-04 by the Marketing special agent). Counting any mirror ever
  created would favour older cohorts, which have had longer to activate, so
  the two 30-day windows could not be compared. Seven days is the window the
  first-repository experiment (`docs/experiments/first-repository-activation-2026-10.md`)
  reads.
- **Retention is a count, not a cohort curve**: the stores keep only each
  account's latest activity, so the figure can say who came back in the last
  30 days but not who was active in an earlier window.
- **Timezone** is UTC because every stored timestamp already is.
- **The backfill for encrypted signals runs in the Worker** (once, marked in
  `schema_meta`) because a D1 migration cannot decrypt account records.
- **The signup-sources chart plots organic against everything else** rather
  than organic against job flow, because job flow is empty; the table below
  it gives every origin exactly.
- **Docs live at `www/docs/admin-dashboard.md`**, the repository's docs tree,
  since there is no top-level `docs/` directory.
