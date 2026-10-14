# Acquisition attribution

How UTM parameters are captured, how later actions are attributed to the
channel that brought a visitor, and how it is reported. Correctness matters
more than breadth: an undercounted metric is recoverable, a silently wrong one
is not.

Code: `app/src/attribution.py` (capture, storage, bot rules, reporting),
`www/worker.js` (`captureAttribution`, the marketing-origin relay),
`app/src/app_runtime.py` (routes and the served-page hook), migration
`app/migrations/0174_attribution.sql`. Tests: `app/tests/test_attribution.py`,
`www/tests/attribution_capture.test.mjs`.

## Attribution model

- **Session:** one anonymous session per `fm_attr` cookie holding a random
  UUID only (`HttpOnly; Secure; SameSite=Lax; Path=/; Domain=<site domain>;
  Max-Age=7776000`, 90 days). The parent domain carries a session from the
  marketing site to signup on the app.
- **Server-side capture** on top-level HTML navigations. On the app the hook
  only adds a `Set-Cookie`; on the marketing site the Worker serves the static
  page and forwards one observation to `POST /api/attribution/capture` in the
  background. Pure static assets (blog, docs, asset-only app shells) are not
  captured.
- **UTM handling:** the five `utm_*` values are lowercased, sanitised to
  `[a-z0-9_\-.]`, truncated to 64 characters, and `source`, `medium`,
  `campaign` are checked against `ATTRIBUTION_ALLOWED_*` allowlists; off-list
  values become `other` (the sanitised original is kept in event metadata).
- **First-touch and last-touch** are both stored; reporting uses first-touch.
- **Events** (`attribution_events`) are exactly `attribution.EVENT_NAMES`:

  | Event | Emitted by |
  | --- | --- |
  | `landing_view` | captured navigation (throttled per session unless tagged) |
  | `clone_url_copied` | the copy-clone-URL control (`POST /api/attribution/events`, the only browser-reportable event) |
  | `signup_started` | first signup POST of a session |
  | `signup_completed` | account saved; links the session to the account |
  | `first_mirror_created` | an owner's first published or browser-created repository |
  | `contribution_made` | a new accepted pull |
  | `managed_waitlist_submitted` | waitlist signup (email kept only as a blind index) |
  | `discovery_call_accepted` | an admin action on the acquisition page |

- **Join:** UTM values are never copied onto the account; reporting joins
  through the session. A signup without a session is recorded as
  `first_utm_source = 'unknown'`. Events attribute to the session's
  first-touch channel for the session's lifetime.

## The clone gap

Git sends no browser cookie, and identifiers in clone URLs would persist in
users' `.git/config`, so clones are never attributed. Two separate metrics
exist and are never summed: `clone_url_copied` (attributed intent) and clones
per repository per day (unattributed deltas of daily snapshots of
`mirror_serve_counters`, taken by cron).

## Bot rules

Sessions are flagged (`is_bot`, `bot_reason`) and excluded by default when:
`user_agent` matches the crawler list (`ATTRIBUTION_BOT_UA_PATTERNS`; such
requests create no session and are counted in `attribution_filtered_hits`),
`bounce` (a single landing view with no follow-up within 2 seconds, judged
hourly and lifted if the session returns), or `ip_rate` (more than 30 sessions
from one address in an hour). Flags are kept for audit; the dashboard can
include bots.

## Privacy and retention

- No IP address is stored on sessions or events; the rate rule uses a keyed,
  daily-salted digest in `attribution_ip_hourly`, purged after 24 hours.
- Referrers are stored as hostname only.
- `DNT: 1`, `Sec-GPC: 1` and visitors from `ATTRIBUTION_SKIP_COUNTRIES`
  (default EU and EEA, until a consent banner exists) get no cookie, session
  or event.
- Retention: `ATTRIBUTION_RETENTION_DAYS` (default 400).
- `DELETE /api/attribution/session` deletes a visitor's session and events.
- `/privacy` documents collection, the cookie, and email tracking
  ([MAIL_LOG.md](MAIL_LOG.md)).

## Reporting rules

- One fixed timezone (`ATTRIBUTION_TIMEZONE`, default `UTC`); weeks run Monday
  to Sunday; the default range is the last complete week; the range end is
  clamped to the report start.
- Every list read is keyset-paginated (`{items, next_cursor, total_count}`);
  `collect_all` raises `ReportIntegrityError` if counts disagree, and the UI
  shows an error instead of a number. No page size acts as a cap. The events
  list accepts `?event=<name>` to confirm a single account's event.

## Surfaces

- `/admin/acquisition` (admin session): channel table by first-touch source
  and campaign, funnel conversion, date range, bot toggle, CSV export
  (`?format=csv`), the clone diagnostic and the discovery-call action; JSON at
  `/api/admin/acquisition/report`, `/sessions`, `/events`.
- `/transparency/acquisition`: public methodology page behind
  `PUBLIC_ACQUISITION_PAGE` (default off), coarse rounded aggregates only.

## Operational notes

- Marketing pages in the www Worker's `run_worker_first` pass through the
  Worker; the relay is fire-and-forget and never changes the page.
- `POST /api/attribution/capture` is trusted only as much as a page view: a
  direct caller can only create a session for itself with sanitised values.
- Cost per captured navigation: one read and at most two writes.
