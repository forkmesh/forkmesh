# 404 tracking and the admin 404s list

Every request that answers 404 on a ForkMesh site is counted in one grouped
list in the is_admin console, Reliability, "404s" (`?view=not-found` on the
console path). When a group reaches 10 hits, every platform administrator
gets one Ping. Added 2026-10-07; the hourly chart and exceptions were added
the same day.

## Code map

| Code | Role |
| --- | --- |
| `app/src/not_found_log.py` | Recording, pruning, the 10-hit alert, the `/api/not-found` relay endpoint and the admin page |
| `app/src/app_runtime.py` (`_fetch_inner`) | Calls `record_response` for every App response with status 404; routes `/api/not-found`; dispatches `?view=not-found` |
| `app/src/admin_shell.py` | The "404s" sidebar entry and its badge |
| `www/worker.js` (`reportNotFound`) | Relays forkmesh.com's own static-asset 404s to the App |
| `app/migrations/0239_not_found_paths.sql` | The `not_found_paths` table |
| `app/migrations/0242_not_found_hours_and_ignores.sql` | The `not_found_hours` (hourly counts) and `not_found_ignores` (exceptions) tables |
| `app/migrations/0244_not_found_last_client.sql` | The encrypted `last_client` column (last visitor) |

## How hits are grouped

One row of `not_found_paths` per (site, method, path):

- **site** is the request's host, lower-cased, with a leading `www.` removed.
  The App records its own host; the forkmesh.com relay always records under
  the host of `WWW_ORIGIN`, whatever the request body claims.
- **method**: `HEAD` counts as `GET`; anything outside the usual verbs is
  `OTHER`.
- **path** has no query string. Repository routes first go through
  `_privacy_safe_log_path`, the same redaction the error log uses, so a 404
  on a private or missing repository never shows its name. Path segments
  that look like ids or tokens (UUIDs, 16+ hex characters, 24+ character
  tokens that contain a digit) become `[id]`, so `/api/notes/<uuid>` is one
  group and a secret in a URL is not stored. Cut at 300 characters.
- **referrer** keeps the last non-empty Referer: host and path for a ForkMesh
  site (so a broken internal link can be found), host only for anyone else.

Each 404 is one upsert (`INSERT … ON CONFLICT DO UPDATE … RETURNING`) on
the group and one on its hour in `not_found_hours`.

## Hourly counts

`not_found_hours` holds one row per group and hour (`hour` is the Unix time
in ms divided by 3,600,000). It backs the page's charts, the same shape as
the errors page: a "Previous 24 hours" bar chart of all hits by hour, and a
24-hour sparkline with that day's count on every group. Hours older than 48
are deleted once per process per hour, by the first hit of a new hour.
Clearing or ignoring a group deletes its hours too, so the chart never
counts a group that is no longer listed. A group dropped by the 5000-group
cap keeps its hours until they age out.

## Exceptions

An exception stops a path from being logged at all: use it for a page, file
or repository that was deleted on purpose and keeps being requested, where
Clear would only start the count again. Each row has an "Ignore" button
(that site and exact path, any method); the Exceptions card below the list
adds one by hand, with "and everything under it" for a prefix such as a
deleted repository. A path typed by hand goes through the same normalisation
as a recorded one (so a UUID becomes `[id]`) but not through the repository
redaction, so a removed private repository has to be ignored from its row,
under its redacted name. A prefix of `/` is refused: it would silence a whole
site.

`not_found_ignores` has one row per (site, path); `prefix` marks a rule that
also covers `path/…`. Adding a rule deletes the groups (and hours) it
covers. A request that matches a rule writes nothing to the groups and
instead bumps the rule's `hits` and `last_seen`, shown as "Suppressed", so an
exception that is still being hit can be seen. Removing a rule logs that
path again from its next hit. Both actions are CSRF-checked and audited
(`admin.console_ignore_not_found`, `admin.console_unignore_not_found`).

Each process keeps the rules in memory for a minute, so checking them costs
no query per 404; the process that changes a rule drops its copy at once and
the other nodes pick it up within a minute. At most 500 rules are kept.

## Last visitor

Added 2026-10-07 so an administrator can tell whether a 404 came from us
(our own browser, desktop, a core node or a monitor) or from outside. Each
group keeps the client of its most recent hit in `last_client`:

- **ip** from `cf-connecting-ip`, which Caddy sets to the TCP peer (any
  client-sent `Cf-*` header is dropped) and the host replaces with the
  visitor from a verified edge-client header (`deploy/core/edge_client.md`).
  Anything that does not parse as an IP address is dropped.
- **country** from `cf-ipcountry`, two letters, shown with its flag; `XX`
  and `T1` (unknown, Tor) are dropped. Only requests that came through the
  forkmesh.com Worker carry a country today: Caddy sets none for direct
  requests, as no GeoIP lookup is built yet, so those rows show the IP and
  User-Agent without a flag.
- **ua**, the User-Agent, control characters turned into spaces, cut at 300
  characters.

The three fields are sealed with `encrypt_row` (the data key) before they
are written, so the database holds no plaintext IP addresses; only the admin
page decrypts them. Every hit replaces the previous visitor, including with
nothing when a hit carries none of the fields. The 90-day and 5000-group
pruning removes them with their group, and "Clear" deletes them at once.

## The alert

When a hit takes a group to 10 or more and the group has not alerted yet,
the row is claimed with `UPDATE … SET alerted_at=? WHERE … AND alerted_at=0`,
so concurrent hits send it once. Each administrator (service accounts
skipped) gets an `operational_alert` Ping titled "404 reached 10 hits: <path>"
that links to the 404s page. It reuses the Operational alerts kind, so the
existing Pings setting for that kind controls it.

At most 5 groups alert per hour. A vulnerability scanner can push hundreds of
distinct paths past 10 in minutes; the cap keeps that to five Pings. A group
held back by the cap stays unclaimed (shown as "Pending") and alerts on its
next hit after the hour.

## Admin page

KPIs (groups, hits, groups at 10+, groups seen in the last 24 hours), the
24-hour chart, then the groups sorted by most hits, busiest in the last 24
hours or most recent, 50 per page. Each row shows
the request (a link for GET paths without `[id]`), a Page/API/File/Git source
label, its 24-hour sparkline, the last referrer, the last visitor (flag,
country, IP and User-Agent, cut to 80 characters with the full string on
hover), first and last seen, and the alert state. "Ignore" adds an exception
(see above); "Clear"
deletes a group after a CSRF check and records an
`admin.console_clear_not_found` audit entry. Clear a group once its link is
fixed: if it comes back it counts from one again and can alert again.

The sidebar badge counts groups that alerted in the last 24 hours.

## Bounds

- Groups not seen for 90 days are deleted.
- At most 5000 groups are kept; the least recently seen go first.
- Pruning runs on the first new group per process and then on every 50th new
  group, not on every hit.
- Hourly counts older than 48 hours are deleted.
- At most 500 exceptions.
- Read-only mode records nothing.

## forkmesh.com relay

forkmesh.com serves static assets from the www Worker, which never reaches
the App. When its asset lookup answers 404 for a GET or HEAD, `reportNotFound`
posts `{path, referrer, userAgent}` to `POST /api/not-found` on `APP_ORIGIN` inside
`waitUntil`, so the visitor's 404 page never waits on it and a failed relay
changes nothing. The endpoint is unauthenticated on purpose: all it can do is
add a 404 for a forkmesh.com path, which requesting that path on forkmesh.com
does anyway. The visitor's IP and country reach the App through the signed
edge-client header the relay already carries; the User-Agent comes from the
body, so a direct caller can name any agent but only its own IP.

## Fixed sources

- `GET /private-or-unpublished-repository/[api:blob]`, last linked from
  `/forkmesh/forkmesh/issues/499` (Ping of 2026-10-07 13:44). An issue
  record lives at `.forkmesh/issues/open/<n>/`, `closed/<n>/` or the legacy
  `<n>/`, and the issue page and the sub-issue/parent summaries asked
  `/blob` for each in turn. Every view of a closed issue therefore answered
  404 for the `open/` path first. `fetchRepoIssueBlob`
  (`app/public/dashboard/js/06-repo-content.js`) now asks for all three in
  one `/blobs` request, which answers a missing path with `null` rather than
  404. It falls back to the one-by-one `/blob` probe only when the batch
  fails or holds none of them (a missing issue, or a batch that dropped a
  path on an upstream timeout). The page-reads bundle (`plan_issue` in
  `app/host/forkmesh_host/repo_views.py`) seeds that same `/blobs` read, so
  nodes that read locally still answer the page from the bundle. The group
  is under the redacted name because `_privacy_safe_log_path` redacts any
  repository that it cannot confirm is public under the requested
  owner/name.

## Tests

`app/tests/test_not_found_log.py`, `www/tests/not_found_relay.test.mjs`, and
the badge case in `app/tests/test_admin_shell.py`. The issue-record batch:
`test_closed_issue_is_read_without_a_404_probe` in
`app/tests/test_issue_subissues_frontend.py` and `test_issue_list_and_detail`
in `app/tests/host/test_repo_views.py`.
