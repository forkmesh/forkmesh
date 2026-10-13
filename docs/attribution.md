# Acquisition attribution

> **ForkMesh Next:** the code is production's. Hosts read `next.forkmesh.com`
> and `app.next.forkmesh.com` here, and the cookie domain is
> `ATTRIBUTION_COOKIE_DOMAIN = "next.forkmesh.com"` (`www/wrangler.toml`), so
> read `forkmesh.com` below as `next.forkmesh.com`.

How ForkMesh captures UTM parameters, attributes later actions to the
channel that brought a visitor, and reports on it. This is the measurement
substrate for the acquisition experiments. Correctness matters more than
breadth: an undercounted metric is recoverable, a silently wrong one is not.

The bounded public-index experiment for issue #478 is recorded in
[`docs/experiments/public-index-search-to-clone-2026-09.md`](experiments/public-index-search-to-clone-2026-09.md).

Code: `app/src/attribution.py` (capture, storage, bot rules, reporting),
`www/worker.js` (`captureAttribution`, the marketing-origin relay),
`app/src/app_runtime.py` (routes and the served-page hook), migration
`app/migrations/0174_attribution.sql`. Tests: `app/tests/test_attribution.py`
and `www/tests/attribution_capture.test.mjs`.

## Attribution model

* **Session.** One anonymous browser session per `fm_attr` cookie. The cookie
  holds one cryptographically random UUID and nothing else
  (`HttpOnly; Secure; SameSite=Lax; Path=/; Domain=forkmesh.com;
  Max-Age=7776000`, i.e. 90 days). The shared parent domain is what carries a
  session from the marketing origin (`forkmesh.com`) to signup on
  `app.forkmesh.com`. Every `*.forkmesh.com` host receives the cookie; it is
  useless without database access.
* **Capture is server-side**, on every served top-level `text/html` GET
  navigation (`Accept: text/html`, `Sec-Fetch-Dest: document` when present).
  On `app.forkmesh.com` the hook runs in `AppRuntime._fetch_inner` after the
  response exists and can only add a `Set-Cookie`. On `forkmesh.com` the
  marketing Worker serves the static asset, sets the cookie if needed, and
  forwards one observation to `POST /api/attribution/capture` inside
  `waitUntil`, so the page never waits on the write. Blog posts (`/blog/*`)
  and docs (`/docs/*`) remain pure static assets and are **not** captured;
  campaign links should point at top-level pages. Likewise on
  `app.forkmesh.com`, only Worker-routed pages are captured (`/signup`,
  repository pages, anything the Worker renders): the page shells that
  `app/wrangler.toml` excludes from `run_worker_first` (`/dashboard`,
  `/login`, `/status`, `/leaderboards`, …) are served straight from Assets
  and never reach the hook.
* **UTM handling.** `utm_source`, `utm_medium`, `utm_campaign`, `utm_term`,
  `utm_content` are lowercased, trimmed, stripped of control characters and
  anything outside `[a-z0-9_\-.]`, and truncated to 64 characters; an empty
  result is stored as null. `source`, `medium` and `campaign` are then
  checked against the configured allowlists (`ATTRIBUTION_ALLOWED_*` in
  `app/wrangler.toml`); anything off-list is stored as `other`, with the
  sanitised original kept in the `landing_view` event's `metadata.utm_other`
  for review. This is what stops cardinality explosion and dashboard
  injection.
* **First-touch and last-touch** are both stored. First-touch fields are
  written once (when null) and never overwritten; last-touch fields update on
  every tagged visit. Reporting uses first-touch.
* **Events** (`attribution_events`) are exactly the eight names in
  `attribution.EVENT_NAMES`; nothing else is accepted:

  | event | emitted by |
  | --- | --- |
  | `landing_view` | every captured navigation (throttled to one per session per 60 s unless the visit is tagged) |
  | `clone_url_copied` | the dashboard's copy-clone-URL control → `POST /api/attribution/events` (the only browser-reportable event) |
  | `signup_started` | first real `POST /api/accounts/signup` from a session (deduped) |
  | `signup_completed` | account saved in `_account_signup`; also sets `attribution_sessions.user_id` |
  | `first_mirror_created` | an owner's first published repository: the catalog publish path (`catalog_routes.py`), and since 2026-10-06 also a repository created or imported from the browser (`hosted_git.py`, both the single write node and the core nodes). Before that, the browser path wrote its catalog row directly and emitted nothing, so `/admin/acquisition` missed every first repository made in the browser while "New accounts activated" on `/product-analysis`, which reads the catalog, counted them |
  | `contribution_made` | a new pull accepted by `pulls_handler` |
  | `managed_waitlist_submitted` | `POST /api/waitlist` (the email survives only as a blind index in metadata) |
  | `discovery_call_accepted` | an admin marking a waitlist email accepted on `/admin/acquisition` |

* **Signup join.** At `signup_completed` the session gets `user_id =
  users.user_bi`. UTM values are never copied onto the account; reporting
  joins through the session so there is one place attribution can be
  corrected. A signup with no session (cookies blocked, DNT/GPC, skipped
  country) is recorded under `first_utm_source = 'unknown'` — not guessed,
  not dropped. Node-originated events (first mirror, pull) join through the
  session linked at signup; an account that predates attribution records
  nothing.
* **Attribution window.** Events are attributed to their session's
  first-touch channel for as long as the session exists (the 90-day cookie).
  The dashboard selects sessions by `first_seen_at` in the chosen range and
  counts their events whenever they occurred.

## The clone gap

Git's HTTP client sends no browser cookie, so an anonymous `git clone` can
never be joined to a session. ForkMesh will not work around this by putting
tokens or identifiers in clone URLs: they would be written permanently into
the user's `.git/config`, which is a privacy problem and a support problem.

Two separate metrics exist instead:

* `clone_url_copied` — an attributed browser intent signal.
* Clones per repository per day — an unattributed server-side count, derived
  as the delta between daily snapshots (`attribution_clone_snapshots`) of the
  cumulative `mirror_serve_counters` that the clone-serving path already
  maintains. The snapshot runs from cron at 00:05 UTC; nothing runs on, gates
  or observes a clone request. Data begins at the first snapshot.

They are never summed, and the copy count is never presented as a clone
count. The dashboard shows their ratio only as a labelled estimate. The
limitation is written into `attribution.clone_count_report`'s docstring so
nobody later "fixes" it by conflating the two.

## Bot rules

A session is flagged (`is_bot = 1`, `bot_reason` set) and excluded from
reporting by default when any of:

* **`user_agent`** — the User-Agent matches the crawler pattern list
  (`ATTRIBUTION_BOT_UA_PATTERNS`, default list in `attribution.py`). Such
  requests create no session and get no cookie (crawlers keep none, and a row
  per request would only burn D1's write budget); they are counted per UTC
  day and reason in `attribution_filtered_hits` so the exclusion remains
  auditable.
* **`bounce`** — the session has a `landing_view`, no other event, and no
  further request after 2 seconds (`last_seen_at - first_seen_at < 2000`).
  Judged by the hourly maintenance sweep once the session is at least an
  hour old; lifted automatically if the session is seen again, since the rule
  is then provably wrong.
* **`ip_rate`** — more than 30 sessions from one address within an hour; all
  of that hour's sessions from the address are flagged.

Flags are stored, never deleted, so filtering can be audited and the
transparency page can state what was filtered. `/admin/acquisition` shows the
excluded count and has a toggle to include bots.

## Privacy and retention

* No IP address is stored on a session or an event. The rate rule keeps a
  keyed digest whose input includes the UTC day (a rotating daily salt) in
  `attribution_ip_hourly`, purged after 24 hours.
* Referrer is stored as hostname only (`first_referrer_host`), never the URL.
* `DNT: 1` and `Sec-GPC: 1`: no cookie, no session, no event; the page is
  served normally.
* Visitors from `ATTRIBUTION_SKIP_COUNTRIES` (default EU-27 + EEA) are
  treated the same way until a consent banner exists. This is the owner's
  decision, recorded on 2026-09-08; the list is configuration in both
  `app/wrangler.toml` and `www/wrangler.toml` and can be emptied to `""`
  only by editing the default, or set to an explicit list.
* Sessions and events are retained for `ATTRIBUTION_RETENTION_DAYS` (default
  400) from `first_seen_at`, then purged by the hourly sweep.
* Self-serve deletion: `DELETE /api/attribution/session` removes the session
  row and its events and clears the cookie; the privacy page exposes a button
  for it.
* `/privacy` describes what is collected, why, and the 90-day cookie.
* Email open and click tracking is disclosed on `/privacy` under **Email we
  send**; `docs/MAIL_LOG.md` (Delivery tracking) records what it stores and its
  90-day deletion.

## Timezone and week definition

Reporting uses one fixed timezone, `ATTRIBUTION_TIMEZONE` (`UTC` or a fixed
`+HH:MM` offset; default `UTC`). Weeks run **Monday–Sunday** in that
timezone. The dashboard's default range is the most recent *complete*
Monday–Sunday week; custom `from`/`to` days are inclusive local days, and
the range's end is clamped to the moment the report started so rows created
during pagination cannot fail the integrity check. Clone snapshots and
filtered-hit counts are keyed by UTC day.

## Pagination correctness

Every list read (`list_sessions`, `list_events`, `list_clone_snapshots`, the
snapshot cron's counter read) is keyset-paginated and returns
`{items, next_cursor, total_count}`. `collect_all` consumes a reader until
`next_cursor` is null and raises `ReportIntegrityError` if the collected
row count differs from `total_count`; the dashboard and the public page then
show an error rather than a number. No default page size acts as a cap. The
admin API exposes the same shape at `/api/admin/acquisition/sessions` and
`/api/admin/acquisition/events`. The events list also takes `?event=<name>`
(one of `EVENT_NAMES`; anything else is a 400 `unknown_event`). The dashboard
and the report only show totals by channel, so this is how a release check
confirms one account's event. For example,
`?from=<day>&to=<day>&event=first_mirror_created` returns the event with
`{"repo": "owner/name"}` in its metadata (added 2026-10-06).

## Surfaces

* `/admin/acquisition` — private dashboard, account session + `is_admin`.
  Channel table by first-touch `utm_source` × `utm_campaign` (sessions,
  clone URL copied, signups, first mirrors, waitlist, and the other events),
  conversion between consecutive funnel stages, date range selector, bot
  toggle with the excluded count, CSV export (`?format=csv`), row totals
  alongside rows shown, the clone diagnostic, and the discovery-call action.
  JSON at `/api/admin/acquisition/report`.
* `/transparency/acquisition` — public methodology page behind
  `PUBLIC_ACQUISITION_PAGE` (default `false`, not enabled by this change).
  Methodology, bot rules, limitations, and coarse rounded aggregates only:
  sessions by channel and total unattributed clones. No signup, conversion or
  revenue figures; not a leaderboard. On `forkmesh.com` the path 308s to the
  App origin like other two-segment paths.

## Running it locally

```sh
cd app
echo "DATA_KEY=local-dev-$(python3 -c 'import secrets;print(secrets.token_urlsafe(24))')" > .dev.vars   # once
python3 tools/dev_local_config.py
uvx --from workers-py pywrangler dev --config .wrangler/dev-local.toml \
    --env '' --ip 127.0.0.1 --port 8787 --persist-to .wrangler/state
```

The derived config drops the `[ai]` binding (which otherwise forces an
authenticated remote session even for local dev) and the custom-domain route
(which otherwise makes every local request look like `app.forkmesh.com`,
disabling the localhost demo login and the host-only cookie). Then:

* open `http://127.0.0.1:8787/signup?utm_source=reddit&utm_campaign=forever_launch`
  — the response sets `fm_attr` and writes a session;
* log in at `http://127.0.0.1:8787/login` with the built-in local demo
  account (`demo@forkmesh.local` / `forkmesh-demo`, created on first login
  with `is_admin=1`);
* open `http://127.0.0.1:8787/admin/acquisition`. The local D1 lives under
  `app/.wrangler/state/v3/d1/`.

## Operational notes

* Marketing pages listed in `www/wrangler.toml`'s `run_worker_first` now pass
  through the www Worker (previously pure static assets). The Worker still
  answers from the asset; the relay is fire-and-forget and a failing App
  never changes the page.
* `POST /api/attribution/capture` is answered on the App Worker's stateless
  path (no ingress Durable Object hop). It is trusted exactly as much as a
  page view: a direct caller can only create a session for its own address
  with sanitised, allowlisted values.
* D1 cost per captured navigation: one read plus at most two writes for a new
  session; one read and usually one write for a returning one.
